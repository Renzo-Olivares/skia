// Copyright 2026 Google LLC
#include "modules/skparagraph/src/CoreTextLineShaper.h"

#include "include/core/SkFont.h"
#include "include/core/SkFontMetrics.h"
#include "include/core/SkPoint.h"
#include "include/core/SkString.h"
#include "include/core/SkTypeface.h"
#include "include/ports/SkTypeface_mac.h"
#include "include/private/SkTArray.h"
#include "include/private/SkTemplates.h"
#include "include/private/SkTo.h"
#include "modules/skparagraph/include/FontCollection.h"
#include "modules/skparagraph/src/CoreTextAttrStringBuilder.h"
#include "modules/skparagraph/src/ParagraphImpl.h"
#include "modules/skparagraph/src/Run.h"
#include "src/core/SkTHash.h"
#include "src/core/SkUTF.h"
#include "src/utils/mac/SkCGBase.h"
#include "src/utils/mac/SkUniqueCFRef.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <utility>

namespace skia {
namespace textlayout {

namespace {

// Writes each glyph's advance from CoreText's base advances, rounded to whole pixels when subpixel
// is off (the meaning of SkFont::isSubpixel), and when splitOffsets is non-null moves half of each
// spacing glyph's pair kerning (base advance minus nominal advance) onto the next glyph's advance
// and x-offset so drawn x stays unchanged while the caret sits at kern/2
static void coretext_advances(bool subpixel,
                              const CGSize* nominal,
                              const CGSize* advances,
                              SkScalar* out,
                              SkScalar* splitOffsets,
                              CFIndex count) {
    // Half a kerning pair would put the next glyph off the pixel grid
    SkASSERT(subpixel || !splitOffsets);
    SkASSERT(!splitOffsets || nominal);
    SkScalar carry = 0;
    for (CFIndex k = 0; k < count; ++k) {
        const SkScalar advance = SkScalarFromCGFloat(advances[k].width);
        if (!splitOffsets) {
            out[k] = subpixel ? advance : SkScalarRoundToScalar(advance);
            continue;
        }
        splitOffsets[k] = carry;
        out[k] = advance + carry;
        carry = 0;
        // CoreText kerns no pair across zero-advance glyphs or past the run end
        if (advance != 0 && k + 1 < count && advances[k + 1].width != 0) {
            carry = (advance - SkScalarFromCGFloat(nominal[k].width)) / 2;
            out[k] -= carry;
        }
    }
}

static bool cfstring_equals_skstring(CFStringRef cfStr, const SkString& skStr) {
    SkASSERT(cfStr);
    if (skStr.isEmpty()) {
        return false;
    }
    SkUniqueCFRef<CFStringRef> skCFStr(
        CFStringCreateWithBytesNoCopy(kCFAllocatorDefault,
                                      reinterpret_cast<const UInt8*>(skStr.c_str()),
                                      skStr.size(),
                                      kCFStringEncodingUTF8,
                                      false,
                                      kCFAllocatorNull));
    if (!skCFStr) {
        return false;
    }
    return CFStringCompare(cfStr, skCFStr.get(), kCFCompareCaseInsensitive) == kCFCompareEqualTo;
}

static bool is_allowed_font(CTFontRef ctFont,
                            const std::vector<sk_sp<SkTypeface>>& allowedTypefaces,
                            const std::vector<SkString>& fontFamilies) {
    SkASSERT(ctFont);
    SkUniqueCFRef<CFStringRef> ctPsName(CTFontCopyPostScriptName(ctFont));
    SkUniqueCFRef<CFStringRef> ctFamName(CTFontCopyFamilyName(ctFont));

    for (const auto& tf : allowedTypefaces) {
        SkASSERT(tf);
        SkString tfPs;
        tf->getPostScriptName(&tfPs);
        if (ctPsName && cfstring_equals_skstring(ctPsName.get(), tfPs)) {
            return true;
        }
        SkString tfFam;
        tf->getFamilyName(&tfFam);
        if (ctFamName && cfstring_equals_skstring(ctFamName.get(), tfFam)) {
            return true;
        }
    }

    for (const auto& fam : fontFamilies) {
        if (ctFamName && cfstring_equals_skstring(ctFamName.get(), fam)) {
            return true;
        }
        if (ctPsName && cfstring_equals_skstring(ctPsName.get(), fam)) {
            return true;
        }
    }

    return false;
}

// Bound CTFont is passed when available so system UI italic faces whose descriptor omits
// kCTFontSlantTrait are still recognized as italic
static void apply_font_style_props(SkFont& font,
                                   CTFontRef ctFont,
                                   const TextStyle& style,
                                   bool fakeMissingFontStyles) {
    font.setEdging(style.getFontEdging());
    font.setHinting(style.getFontHinting());
    font.setSubpixel(style.getSubpixel());

    // CoreText binds the closest real face and never synthesizes a missing bold or italic,
    // so the run gets the same fake flags OneLineShaper derives from the typeface gap
    if (fakeMissingFontStyles && font.getTypeface()) {
        const SkFontStyle wanted = style.getFontStyle();
        const SkFontStyle bound = font.getTypeface()->fontStyle();
        const bool fakeBold = wanted.weight() >= SkFontStyle::kSemiBold_Weight &&
                              wanted.weight() - bound.weight() >= 200;
        // System UI italic faces (.SFNS-RegularItalic) set kCTFontTraitItalic on the CTFont but
        // omit kCTFontSlantTrait from CTFontCopyFontDescriptor, so SkTypeface::fontStyle() reports
        // upright; inspect the bound CTFont before adding synthetic skew
        const bool isCtItalic =
                ctFont && ((CTFontGetSymbolicTraits(ctFont) & kCTFontTraitItalic) != 0 ||
                           CTFontGetSlantAngle(ctFont) != 0);
        const bool fakeItalic = wanted.slant() == SkFontStyle::kItalic_Slant &&
                                bound.slant() != SkFontStyle::kItalic_Slant && !isCtItalic;
        font.setEmbolden(fakeBold);
        font.setSkewX(fakeItalic ? -SK_Scalar1 / 4 : 0);
    }
}

// Reordered complex-script glyphs (kCTRunStatusNonMonotonic) can report out-of-order string
// indices; Run::iterateThroughClustersInTextOrder requires non-decreasing indices for LTR and
// non-increasing for RTL while keeping CoreText's glyph IDs and positions untouched
static void normalize_cluster_indices(uint32_t* clusters, CFIndex glyphCount, bool ltr) {
    if (glyphCount <= 1) {
        return;
    }
    if (ltr) {
        for (CFIndex k = glyphCount - 2; k >= 0; --k) {
            if (clusters[k] > clusters[k + 1]) {
                clusters[k] = clusters[k + 1];
            }
        }
    } else {
        for (CFIndex k = 1; k < glyphCount; ++k) {
            if (clusters[k] > clusters[k - 1]) {
                clusters[k] = clusters[k - 1];
            }
        }
    }
}

// One CTLine for the whole of attrStr. Every font in it keeps its trak, fallback fonts too, so
// the letterSpacing SkParagraph adds later stacks on the trak as native tracking does, except
// in cursive scripts, which SkParagraph does not space
static SkUniqueCFRef<CTLineRef> typeset_line(CFAttributedStringRef attrStr) {
    const CFRange range = CFRangeMake(0, CFAttributedStringGetLength(attrStr));
    SkUniqueCFRef<CTTypesetterRef> typesetter(CTTypesetterCreateWithAttributedString(attrStr));
    if (!typesetter) {
        return nullptr;
    }
    return SkUniqueCFRef<CTLineRef>(CTTypesetterCreateLine(typesetter.get(), range));
}

}  // namespace

bool CoreTextLineShaper::IsZeroWidthCodepoint(SkUnichar utf8) {
    // Default-ignorable format characters and non-spacing combining marks take no advance of their
    // own, so a fallback .notdef box is not widened for them. Neither do the C0/C1 controls with
    // Bidi_Class BN, which a native CTLine drops (tab, separators and U+0085 are not BN)
    static constexpr std::array<std::pair<SkUnichar, SkUnichar>, 17> ranges {{
            {0x0001, 0x0008},
            {0x000E, 0x001B},
            {0x007F, 0x0084},
            {0x0086, 0x009F},
            {0x00AD, 0x00AD},
            {0x0300, 0x036F},
            {0x061C, 0x061C},
            {0x1AB0, 0x1AFF},
            {0x1DC0, 0x1DFF},
            {0x200B, 0x200F},
            {0x202A, 0x202E},
            {0x2060, 0x2069},
            {0x20D0, 0x20FF},
            {0xFE00, 0xFE0F},
            {0xFE20, 0xFE2F},
            {0xFEFF, 0xFEFF},
            {0xE0100, 0xE01EF},
    }};
    auto it = std::ranges::lower_bound(
            ranges, utf8, {}, &std::pair<SkUnichar, SkUnichar>::second);
    return it != ranges.end() && utf8 >= it->first;
}

bool CoreTextLineShaper::isLastResortFont(CTFontRef font) {
    SkASSERT(font);
    SkUniqueCFRef<CFStringRef> psName(CTFontCopyPostScriptName(font));
    if (psName) {
        CFRange r = CFStringFind(psName.get(), CFSTR("LastResort"), kCFCompareCaseInsensitive);
        if (r.location != kCFNotFound) {
            return true;
        }
    }
    return false;
}

void CoreTextLineShaper::ShapeAffix(const SkString& text,
                                    const TextStyle& style,
                                    sk_sp<SkTypeface> clusterTypeface,
                                    bool fakeMissingFontStyles,
                                    uint8_t bidiLevel,
                                    sk_sp<FontCollection> fontCollection,
                                    SkShaper::RunHandler* handler) {
    SkASSERT(handler);
    if (text.isEmpty() || !fontCollection) {
        return;
    }
    const TextDirection baseDir = (bidiLevel & 1) ? TextDirection::kRtl : TextDirection::kLtr;
    // Inherit the line's resolved cluster typeface when the ellipsis style does not specify an
    // explicit typeface or font family list
    TextStyle affixStyle = style;
    if (!affixStyle.refTypeface() && affixStyle.getFontFamilies().empty() && clusterTypeface) {
        affixStyle.setTypeface(clusterTypeface);
    }

    std::vector<sk_sp<SkTypeface>> allowedTypefaces =
            CoreTextFontRegistry::Instance().findTypefaces(*fontCollection,
                                                           affixStyle.getFontFamilies(),
                                                           affixStyle.getFontStyle(),
                                                           affixStyle.getFontArguments());
    sk_sp<SkTypeface> primary = affixStyle.refTypeface();
    if (primary) {
        allowedTypefaces.insert(allowedTypefaces.begin(), primary);
    } else if (!allowedTypefaces.empty()) {
        primary = allowedTypefaces.front();
    }
    if (clusterTypeface) {
        allowedTypefaces.push_back(clusterTypeface);
    }

    const bool fallbackDisabled = !fontCollection->fontFallbackEnabled();
    CoreTextAttrStringBuilder builder(
            text.c_str(), text.size(), baseDir, std::move(fontCollection));
    builder.addStyle(TextRange(0, text.size()), affixStyle);
    SkUniqueCFRef<CFAttributedStringRef> attrStr = builder.build();
    if (!attrStr) {
        return;
    }
    SkUniqueCFRef<CTLineRef> line = typeset_line(attrStr.get());
    if (!line) {
        return;
    }
    CFArrayRef runs = CTLineGetGlyphRuns(line.get());
    const CFIndex runCount = runs ? CFArrayGetCount(runs) : 0;

    // ShapeHandler::runBuffer accepts at most one Run, so multi-run affixes coalesce into one
    // buffer with the first resolved CTRun's font and mark glyphs from other fonts unresolved
    CFIndex totalGlyphs = 0;
    CTRunRef primaryRun = nullptr;
    CTFontRef primaryCtFont = nullptr;
    for (CFIndex i = 0; i < runCount; ++i) {
        CTRunRef ctRun = (CTRunRef)CFArrayGetValueAtIndex(runs, i);
        CFIndex gc = CTRunGetGlyphCount(ctRun);
        if (gc <= 0) {
            continue;
        }
        totalGlyphs += gc;
        CTFontRef ctFont =
                (CTFontRef)CFDictionaryGetValue(CTRunGetAttributes(ctRun), kCTFontAttributeName);
        SkASSERT(ctFont);
        bool disallowed =
                isLastResortFont(ctFont) ||
                (fallbackDisabled &&
                 !is_allowed_font(ctFont, allowedTypefaces, affixStyle.getFontFamilies()));
        if (!primaryRun || (!disallowed && isLastResortFont(primaryCtFont))) {
            primaryRun = ctRun;
            primaryCtFont = ctFont;
        }
    }
    if (totalGlyphs == 0 || !primaryRun) {
        return;
    }

    const bool zeroSize = affixStyle.getFontSize() <= 0;
    const bool primaryDisallowed =
            isLastResortFont(primaryCtFont) ||
            (fallbackDisabled &&
             !is_allowed_font(primaryCtFont, allowedTypefaces, affixStyle.getFontFamilies()));
    sk_sp<SkTypeface> runTypeface =
            primaryDisallowed ? primary : SkMakeTypefaceFromCTFont(primaryCtFont);
    SkFont font(std::move(runTypeface),
                zeroSize ? 0 : SkScalarFromCGFloat(CTFontGetSize(primaryCtFont)));
    // Only pass primaryCtFont when it was not rejected as LastResort or disallowed fallback
    apply_font_style_props(font,
                           primaryDisallowed ? nullptr : primaryCtFont,
                           affixStyle,
                           fakeMissingFontStyles);

    const bool ctIsRtl = (CTRunGetStatus(primaryRun) & kCTRunStatusRightToLeft) != 0;
    uint8_t level = bidiLevel;
    if ((level % 2 != 0) != ctIsRtl) {
        level = ctIsRtl ? (level | 1) : (level > 0 ? level - 1 : 0);
    }

    // Fetched once per run; the pen walk below reads them again
    skia_private::AutoSTArray<16, CGGlyph> glyphs(totalGlyphs);
    skia_private::AutoSTArray<16, CGSize> advances(totalGlyphs);
    skia_private::AutoSTArray<16, CGPoint> origins(totalGlyphs);
    skia_private::AutoSTArray<16, SkScalar> glyphAdvances(totalGlyphs);
    // The part of a kerning pair moved into each glyph's offset, as in shapeTextRegion
    skia_private::AutoSTArray<16, SkScalar> splitOffsets(totalGlyphs);
    std::fill_n(splitOffsets.data(), totalGlyphs, 0.0f);
    const bool affixHasTab = memchr(text.c_str(), '\t', text.size()) != nullptr;
    SkScalar totalAdvance = 0;
    CFIndex runStart = 0;
    for (CFIndex i = 0; i < runCount; ++i) {
        CTRunRef ctRun = (CTRunRef)CFArrayGetValueAtIndex(runs, i);
        const CFIndex gc = CTRunGetGlyphCount(ctRun);
        if (gc <= 0) {
            continue;
        }
        CTRunGetGlyphs(ctRun, CFRangeMake(0, gc), glyphs.data() + runStart);
        // Base advances keep a mark's drawn offset off the glyph before it, as in shapeTextRegion
        CTRunGetBaseAdvancesAndOrigins(
                ctRun, CFRangeMake(0, gc), advances.data() + runStart, origins.data() + runStart);
        // A zero-size affix keeps zero advances, as in shapeTextRegion
        if (!zeroSize) {
            // Left-to-right subpixel runs without tabs split pair kerning as in shapeTextRegion
            const bool splitKerning =
                    font.isSubpixel() && !affixHasTab &&
                    (CTRunGetStatus(ctRun) & kCTRunStatusRightToLeft) == 0;
            skia_private::AutoSTArray<16, CGSize> nominal(splitKerning ? gc : 0);
            if (splitKerning) {
                const CTFontRef ctFont = (CTFontRef)CFDictionaryGetValue(
                        CTRunGetAttributes(ctRun), kCTFontAttributeName);
                SkASSERT(ctFont);
                CTFontGetAdvancesForGlyphs(ctFont,
                                           kCTFontOrientationHorizontal,
                                           glyphs.data() + runStart,
                                           nominal.data(),
                                           gc);
            }
            coretext_advances(font.isSubpixel(),
                              splitKerning ? nominal.data() : nullptr,
                              advances.data() + runStart,
                              glyphAdvances.data() + runStart,
                              splitKerning ? splitOffsets.data() + runStart : nullptr,
                              gc);
            for (CFIndex k = 0; k < gc; ++k) {
                totalAdvance += glyphAdvances[runStart + k];
            }
        }
        runStart += gc;
    }

    const SkString language = affixStyle.getLocale();
    const SkShaper::RunHandler::RunInfo info = {
            font,
            level,
            builder.scriptAt(0),
            language.c_str(),
            SkVector::Make(totalAdvance, 0),
            static_cast<size_t>(totalGlyphs),
            SkShaper::RunHandler::Range(0, text.size())};
    handler->beginLine();
    handler->runInfo(info);
    handler->commitRunInfo();
    SkShaper::RunHandler::Buffer buffer = handler->runBuffer(info);

    static_assert(sizeof(CGGlyph) == sizeof(SkGlyphID));
    CFIndex outIdx = 0;
    // Same pen as in shapeTextRegion: base advances summed in float, with each origin apart in
    // the offset when the handler takes offsets, as SkShaper's HarfBuzz runs do
    SkScalar penX = 0;
    for (CFIndex i = 0; i < runCount; ++i) {
        CTRunRef ctRun = (CTRunRef)CFArrayGetValueAtIndex(runs, i);
        const CFIndex gc = CTRunGetGlyphCount(ctRun);
        if (gc <= 0) {
            continue;
        }
        CTFontRef ctFont =
                (CTFontRef)CFDictionaryGetValue(CTRunGetAttributes(ctRun), kCTFontAttributeName);
        const bool unresolvedRun =
                primaryDisallowed || isLastResortFont(ctFont) ||
                (fallbackDisabled &&
                 !is_allowed_font(ctFont, allowedTypefaces, affixStyle.getFontFamilies())) ||
                !CFEqual(ctFont, primaryCtFont);

        skia_private::AutoSTArray<16, CFIndex> indices(gc);
        CTRunGetStringIndices(ctRun, CFRangeMake(0, gc), indices.data());
        for (CFIndex k = 0; k < gc; ++k) {
            const CFIndex g = outIdx + k;
            buffer.glyphs[g] = unresolvedRun ? 0 : glyphs[g];
            // CoreText is y-up, Skia y-down; splitOffsets restores drawn x after moving half a
            // kerning pair onto this glyph's advance
            const CGPoint origin = zeroSize ? CGPointZero : origins[g];
            const SkPoint offset = SkPoint::Make(SkScalarFromCGFloat(origin.x) + splitOffsets[g],
                                                 -SkScalarFromCGFloat(origin.y));
            if (buffer.offsets) {
                buffer.positions[g] = buffer.point + SkPoint::Make(penX, 0);
                buffer.offsets[g] = offset;
            } else {
                buffer.positions[g] = buffer.point + SkPoint::Make(penX, 0) + offset;
            }
            if (!zeroSize) {
                penX += glyphAdvances[g];
            }
            if (buffer.clusters) {
                buffer.clusters[g] = SkToU32(builder.indexMap().toUtf8Index(indices[k]));
            }
        }
        outIdx += gc;
    }
    if (buffer.clusters) {
        normalize_cluster_indices(buffer.clusters, totalGlyphs, (level % 2) == 0);
    }
    handler->commitRunBuffer(info);
    handler->commitLine();
}

CoreTextLineShaper::CoreTextLineShaper(ParagraphImpl* paragraph)
    : fParagraph(paragraph)
    , fUnresolvedGlyphs(0) {
}

bool CoreTextLineShaper::shape() {
    if (fParagraph->text().size() == 0) {
        return false;
    }

    fUnresolvedGlyphs = 0;
    // Release the set, do not clear it: a ParagraphCache entry may share it
    fParagraph->fUnresolvedCodepoints.reset();
    fParagraph->fFontSwitches.clear();
    fParagraph->fRuns.clear();

    SkScalar advanceX = 0;

    if (fParagraph->fPlaceholders.empty()) {
        TextRange entireText(0, fParagraph->text().size());
        if (!this->shapeTextRange(entireText, advanceX)) {
            return false;
        }
    } else {
        for (auto& placeholder : fParagraph->fPlaceholders) {
            if (placeholder.fTextBefore.width() > 0) {
                if (!this->shapeTextRange(placeholder.fTextBefore, advanceX)) {
                    return false;
                }
            }

            if (placeholder.fRange.width() == 0) {
                continue;
            }

            uint8_t placeholderLevel = 2;
            for (const auto& r : fParagraph->fBidiRegions) {
                if (placeholder.fRange.start >= r.start && placeholder.fRange.start < r.end) {
                    placeholderLevel = r.level;
                    break;
                }
            }

            std::vector<sk_sp<SkTypeface>> typefaces =
                    CoreTextFontRegistry::Instance().findTypefaces(
                            *fParagraph->fFontCollection,
                            placeholder.fTextStyle.getFontFamilies(),
                            placeholder.fTextStyle.getFontStyle(),
                            placeholder.fTextStyle.getFontArguments());
            sk_sp<SkTypeface> typeface = typefaces.empty() ? nullptr : typefaces.front();
            SkFont font(std::move(typeface), placeholder.fTextStyle.getFontSize());

            font.setEdging(placeholder.fTextStyle.getFontEdging());
            font.setHinting(placeholder.fTextStyle.getFontHinting());
            font.setSubpixel(placeholder.fTextStyle.getSubpixel());

            const SkShaper::RunHandler::RunInfo runInfo = {
                font,
                placeholderLevel,
                0,
                "",
                SkPoint::Make(placeholder.fStyle.fWidth, placeholder.fStyle.fHeight),
                1,
                SkShaper::RunHandler::Range(0, placeholder.fRange.width())
            };

            auto& run = fParagraph->fRuns.emplace_back(
                fParagraph,
                runInfo,
                placeholder.fRange.start,
                0.0f,
                false,
                0.0f,
                fParagraph->fRuns.size(),
                advanceX
            );

            run.allocateGlyphData(1, /*hasOffsets=*/false);
            run.fGlyphData->glyphs()[0] = 0;
            run.fGlyphData->positions()[0] = { advanceX, 0 };
            // CoreText runs keep the end sentinel on the baseline
            run.fGlyphData->positions()[1] = { advanceX + placeholder.fStyle.fWidth, 0 };
            run.fGlyphData->clusterIndexes()[0] = 0;
            run.fPlaceholderIndex = &placeholder - fParagraph->fPlaceholders.begin();
            advanceX += placeholder.fStyle.fWidth;
        }
    }

    return true;
}

// CoreText typesetter produces a single visual line; isolating hard line breaks keeps each
// paragraph segment in one CTLine while CoreTextAttrStringBuilder splits CTRuns across blocks
// with differing Run-level font or metric properties
bool CoreTextLineShaper::shapeTextRange(TextRange range, SkScalar& advanceX) {
    if (range.width() == 0) {
        return true;
    }

    const char* text = fParagraph->text().data();
    size_t segStart = range.start;

    while (segStart < range.end) {
        // The '\n' ends its segment, so each CTLine holds one hard-break paragraph for BiDi and
        // typesetting
        size_t segEnd = segStart;
        while (segEnd < range.end && text[segEnd++] != '\n') {
        }

        // Shape the entire hard-break segment in a single CTLine so contextual joining and BiDi
        // reordering operate across adjacent Block boundaries
        if (!this->shapeTextRegion(TextRange(segStart, segEnd), advanceX)) {
            return false;
        }

        segStart = segEnd;
    }

    return true;
}

bool CoreTextLineShaper::shapeTextRegion(TextRange regionRange, SkScalar& advanceX) {
    if (regionRange.width() == 0) {
        return true;
    }

    const char* regionUtf8 = fParagraph->text().data() + regionRange.start;
    size_t regionBytes = regionRange.width();
    TextDirection baseDir = fParagraph->paragraphStyle().getTextDirection();

    CoreTextAttrStringBuilder builder(
        regionUtf8, regionBytes, baseDir, fParagraph->fFontCollection);

    auto blockRange = fParagraph->findAllBlocks(regionRange);
    for (const auto& block : fParagraph->blocks(blockRange)) {
        size_t start = std::max(block.fRange.start, regionRange.start);
        size_t end = std::min(block.fRange.end, regionRange.end);
        if (start >= end) {
            continue;
        }
        TextRange relRange(start - regionRange.start, end - regionRange.start);
        builder.addStyle(relRange, block.fStyle);
    }

    SkUniqueCFRef<CFAttributedStringRef> attrStr = builder.build();
    if (!attrStr) {
        return false;
    }

    if (CFAttributedStringGetLength(attrStr.get()) == 0) {
        return true;
    }

    SkUniqueCFRef<CTLineRef> line = typeset_line(attrStr.get());
    if (!line) {
        return false;
    }

    CFArrayRef runs = CTLineGetGlyphRuns(line.get());
    CFIndex runCount = runs ? CFArrayGetCount(runs) : 0;
    if (runCount == 0) {
        return true;
    }

    struct CTRunItem {
        CTRunRef run;
        CFIndex location;
        CFIndex length;
    };
    skia_private::STArray<16, CTRunItem> sortedRuns(runCount);
    for (CFIndex i = 0; i < runCount; ++i) {
        CTRunRef ctRun = (CTRunRef)CFArrayGetValueAtIndex(runs, i);
        CFRange cfRange = CTRunGetStringRange(ctRun);
        sortedRuns.push_back({ctRun, cfRange.location, cfRange.length});
    }

    // CTRuns arrive in visual order from CTLineGetGlyphRuns
    // Logical ascending sort prevents BiDi inversion downstream in cluster layout
    std::stable_sort(sortedRuns.begin(), sortedRuns.end(),
                     [](const CTRunItem& a, const CTRunItem& b) {
                         return a.location < b.location;
                     });

    skia_private::THashMap<CTFontRef, sk_sp<SkTypeface>> typefaceCache;
    const bool fallbackDisabled = !fParagraph->fFontCollection->fontFallbackEnabled();

    // Neighboring runs mostly share a style block, so the typeface lookups and the
    // allowed-font verdicts are rebuilt only when the block changes, not for every run
    bool hasBlockState = false;
    const Block* runBlock = nullptr;
    const TextStyle* style = nullptr;
    sk_sp<SkTypeface> primary;
    std::vector<sk_sp<SkTypeface>> allowedTypefaces;
    SkString language;
    skia_private::THashMap<CTFontRef, bool> allowedFontCache;
    // Run::commit keeps the glyph offsets only when one is not zero, as for SkShaper runs
    Run::ShapingBuffer shapingBuffer;

    const auto& textStyles = fParagraph->fTextStyles;
    for (const auto& item : sortedRuns) {
        CTRunRef ctRun = item.run;
        CFIndex glyphCount = CTRunGetGlyphCount(ctRun);
        if (glyphCount == 0) {
            continue;
        }

        CFRange cfRange = CFRangeMake(item.location, item.length);
        TextRange relUtf8Range = builder.indexMap().cfrangeToTextRange(cfRange);
        TextRange runUtf8Range(regionRange.start + relUtf8Range.start,
                               regionRange.start + relUtf8Range.end);
        if (runUtf8Range.width() == 0) {
            continue;
        }

        // fTextStyles is sorted by start, so the candidate is the last block starting at or
        // before the run; a run in a gap between blocks keeps the paragraph default style
        const Block* nextBlock = std::upper_bound(
                textStyles.begin(),
                textStyles.end(),
                runUtf8Range.start,
                [](size_t pos, const Block& block) { return pos < block.fRange.start; });
        const Block* block = nullptr;
        if (nextBlock != textStyles.begin() && runUtf8Range.start < nextBlock[-1].fRange.end) {
            block = nextBlock - 1;
        }

        if (!hasBlockState || block != runBlock) {
            hasBlockState = true;
            runBlock = block;
            style = block ? &block->fStyle : &fParagraph->paragraphStyle().getTextStyle();
            language = style->getLocale();
            allowedFontCache.reset();
            allowedTypefaces.clear();
            primary = style->refTypeface();
            if (primary) {
                allowedTypefaces.push_back(primary);
            } else {
                // Same clone the attributed string was built from, so LastResort runs and the
                // allowed-font check see the varied face (or default .AppleSystemUIFont) rather
                // than a null typeface
                allowedTypefaces = CoreTextFontRegistry::Instance().findTypefaces(
                        *fParagraph->fFontCollection,
                        style->getFontFamilies(),
                        style->getFontStyle(),
                        style->getFontArguments());
                if (!allowedTypefaces.empty()) {
                    primary = allowedTypefaces.front();
                }
            }
        }

        CFDictionaryRef runAttrs = CTRunGetAttributes(ctRun);
        // CoreText manufactures run attributes during font substitution, and they always
        // carry the font the glyphs were shaped with
        CTFontRef ctFont = (CTFontRef)CFDictionaryGetValue(runAttrs, kCTFontAttributeName);
        SkASSERT(ctFont);
        bool isFallback = isLastResortFont(ctFont);
        if (!isFallback && fallbackDisabled) {
            bool* allowed = allowedFontCache.find(ctFont);
            if (!allowed) {
                allowed = allowedFontCache.set(
                        ctFont,
                        is_allowed_font(ctFont, allowedTypefaces, style->getFontFamilies()));
            }
            isFallback = !*allowed;
        }

        // CoreText reads a zero point size as its 12pt default, so the attributed string shapes
        // such text at a stand-in size and the run collapses to a zero-size font with zero
        // advances, matching HarfBuzz, so builds without HarfBuzz lay it out too
        const bool zeroSize = style->getFontSize() <= 0;
        CGFloat fontSize = zeroSize ? 0 : CTFontGetSize(ctFont);
        SkFont font;
        if (isFallback) {
            font = SkFont(primary, SkScalarFromCGFloat(fontSize));
        } else {
            sk_sp<SkTypeface> typeface;
            if (sk_sp<SkTypeface>* cached = typefaceCache.find(ctFont)) {
                typeface = *cached;
            } else {
                typeface = SkMakeTypefaceFromCTFont(ctFont);
                typefaceCache.set(ctFont, typeface);
            }
            font = SkFont(std::move(typeface), SkScalarFromCGFloat(fontSize));
        }

        // Fallback runs use the primary typeface rather than the bound CTFont
        apply_font_style_props(font,
                               isFallback ? nullptr : ctFont,
                               *style,
                               fParagraph->paragraphStyle().fakeMissingFontStyles());

        CTRunStatus status = CTRunGetStatus(ctRun);
        bool ctIsRtl = (status & kCTRunStatusRightToLeft) != 0;
        uint8_t bidiLevel = 0;
        bool foundRegion = false;
        for (const auto& r : fParagraph->fBidiRegions) {
            if (runUtf8Range.start >= r.start && runUtf8Range.start < r.end) {
                bidiLevel = r.level;
                foundRegion = true;
                break;
            }
        }
        if (foundRegion) {
            // Reconcile ICU embedding level parity with CoreText run direction to ensure downstream
            // visual reordering matches
            if ((bidiLevel % 2 != 0) != ctIsRtl) {
                bidiLevel = ctIsRtl ? (bidiLevel | 1) : (bidiLevel > 0 ? bidiLevel - 1 : 0);
            }
        } else {
            bidiLevel = ctIsRtl ? 1 : 0;
        }

        // The itemization that keeps cursive ranges unspaced also names the run's
        // script, so Run::isCursiveScript agrees with the glyphs CoreText produced
        SkFourByteTag script = builder.scriptAt(relUtf8Range.start);

        skia_private::AutoSTArray<64, CFIndex> indexBuffer(glyphCount);
        CTRunGetStringIndices(ctRun, CFRangeMake(0, glyphCount), indexBuffer.data());

        skia_private::AutoSTArray<64, uint32_t> clusterBuffer(glyphCount);
        for (CFIndex k = 0; k < glyphCount; ++k) {
            size_t u8InRegion = builder.indexMap().toUtf8Index(indexBuffer[k]);
            size_t globalU8 = regionRange.start + u8InRegion;
            size_t clusterInRun =
                    (globalU8 >= runUtf8Range.start) ? (globalU8 - runUtf8Range.start) : 0;
            clusterBuffer[k] = SkToU32(clusterInRun);
        }
        const bool runIsLtr = (bidiLevel % 2) == 0;
        normalize_cluster_indices(clusterBuffer.data(), glyphCount, runIsLtr);

        auto glyphTextRange = [&](CFIndex k) -> TextRange {
            size_t cStart = clusterBuffer[k];
            size_t cEnd = cStart;
            if (runIsLtr) {
                cEnd = (k + 1 < glyphCount) ? clusterBuffer[k + 1] : runUtf8Range.width();
            } else {
                cEnd = (k > 0) ? clusterBuffer[k - 1] : runUtf8Range.width();
            }
            if (cStart > cEnd) {
                std::swap(cStart, cEnd);
            }
            return TextRange(runUtf8Range.start + cStart, runUtf8Range.start + cEnd);
        };

        // Control and zero-width format characters never count toward unresolved glyphs or
        // codepoints, matching HarfBuzz OneLineShaper::iterateThroughFontStyles
        auto recordUnresolvedGlyph = [&](TextRange gRange) {
            const char* p = fParagraph->text().data() + gRange.start;
            const char* pEnd = fParagraph->text().data() + gRange.end;
            bool hasNonControl = false;
            while (p < pEnd) {
                size_t bytePos = p - fParagraph->text().data();
                SkUnichar u = SkUTF::NextUTF8(&p, pEnd);
                if (u < 0) {
                    break;
                }
                if (!fParagraph->codeUnitHasProperty(bytePos, SkUnicode::CodeUnitFlags::kControl) &&
                    !IsZeroWidthCodepoint(u)) {
                    hasNonControl = true;
                    if (fParagraph->fUnresolvedCodepoints == nullptr) {
                        fParagraph->fUnresolvedCodepoints =
                                std::make_shared<std::unordered_set<SkUnichar>>();
                    }
                    // No cache entry may see the set change
                    SkASSERT(fParagraph->fUnresolvedCodepoints.use_count() == 1);
                    fParagraph->fUnresolvedCodepoints->emplace(u);
                }
            }
            if (hasNonControl) {
                ++fUnresolvedGlyphs;
            }
        };

        skia_private::AutoSTArray<64, SkScalar> glyphAdvances(glyphCount);
        skia_private::AutoSTArray<64, CGSize> advances(isFallback ? 0 : glyphCount);
        skia_private::AutoSTArray<64, CGPoint> origins(isFallback ? 0 : glyphCount);
        skia_private::AutoSTArray<64, CGGlyph> glyphBuffer(isFallback ? 0 : glyphCount);
        // Only left-to-right subpixel runs split pair kerning; a tab's advance to its stop is not
        // kerning, so runs containing a tab keep whole advances
        const bool splitKerning =
                !isFallback && !zeroSize && runIsLtr && font.isSubpixel() &&
                !memchr(fParagraph->text().data() + runUtf8Range.start, '\t', runUtf8Range.width());
        skia_private::AutoSTArray<64, SkScalar> splitOffsets(splitKerning ? glyphCount : 0);
        SkScalar runAdvanceX = 0;

        if (isFallback) {
            SkScalar notdefAdvance = font.getWidth(0);
            if (SkScalarNearlyZero(notdefAdvance)) {
                notdefAdvance = font.getSize();
            }
            // .notdef gets the same advance as any other glyph, whole pixels included
            if (!font.isSubpixel()) {
                notdefAdvance = SkScalarRoundToScalar(notdefAdvance);
            }

            for (CFIndex k = 0; k < glyphCount; ++k) {
                TextRange gRange = glyphTextRange(k);
                const char* p = fParagraph->text().data() + gRange.start;
                const char* pEnd = fParagraph->text().data() + gRange.end;
                size_t visibleCount = 0;
                while (p < pEnd) {
                    SkUnichar u = SkUTF::NextUTF8(&p, pEnd);
                    if (u < 0) {
                        break;
                    }
                    if (!IsZeroWidthCodepoint(u)) {
                        ++visibleCount;
                    }
                }

                glyphAdvances[k] = visibleCount * notdefAdvance;
                runAdvanceX += glyphAdvances[k];
            }
        } else {
            CTRunGetGlyphs(ctRun, CFRangeMake(0, glyphCount), glyphBuffer.data());
            // CTRunGetAdvances derives advances from drawn x, so a zero-advance mark drawn d off
            // the pen shows as +d on the glyph before it in the run and -d on the mark. Base
            // advances leave mark positioning to the origins, as HarfBuzz leaves it to x_offset
            CTRunGetBaseAdvancesAndOrigins(
                    ctRun, CFRangeMake(0, glyphCount), advances.data(), origins.data());
            // The 12pt stand-in advances CoreText gives zero-size text never reach the run
            if (zeroSize) {
                std::fill_n(glyphAdvances.data(), glyphCount, 0.0f);
            } else {
                skia_private::AutoSTArray<64, CGSize> nominal(splitKerning ? glyphCount : 0);
                if (splitKerning) {
                    CTFontGetAdvancesForGlyphs(ctFont,
                                               kCTFontOrientationHorizontal,
                                               glyphBuffer.data(),
                                               nominal.data(),
                                               glyphCount);
                }
                // KEEPER-DEBT: this keeps CoreText's advance to the next 28pt tab stop, where
                // HarfBuzz gives U+0009 its hmtx advance; the stop counts from the CTLine start
                // (hard break or placeholder), so after a soft wrap it is off the line's stops.
                // Remove when tabs get the HarfBuzz advance or line-relative stops
                coretext_advances(font.isSubpixel(),
                                  splitKerning ? nominal.data() : nullptr,
                                  advances.data(),
                                  glyphAdvances.data(),
                                  splitKerning ? splitOffsets.data() : nullptr,
                                  glyphCount);
            }
            for (CFIndex k = 0; k < glyphCount; ++k) {
                // CoreText gives '\n' no advance, HarfBuzz the advance of the primary font's
                // cmap glyph (.notdef when it has none); that advance moves all later lines, so
                // their float positions and widths differ unless '\n' gets one here too
                TextRange gRange = glyphTextRange(k);
                if (!zeroSize && gRange.width() == 1 && fParagraph->text()[gRange.start] == '\n') {
                    // KEEPER-DEBT: this is the run font's U+000A glyph; with a primary font
                    // that lacks U+000A, CoreText puts '\n' in a fallback run, so this is not
                    // the primary .notdef HarfBuzz uses and its advance can differ. Remove when
                    // '\n' stays in the primary font's run
                    SkScalar newlineAdvance = font.getWidth(font.unicharToGlyph('\n'));
                    // Whole pixels for a non-subpixel font, as for every other glyph
                    glyphAdvances[k] = font.isSubpixel() ? newlineAdvance
                                                         : SkScalarRoundToScalar(newlineAdvance);
                }
                runAdvanceX += glyphAdvances[k];
            }
        }

        SkFontMetrics fontMetrics;
        font.getMetrics(&fontMetrics);
        SkScalar runHeight = style->getHeightOverride() ? style->getHeight() : 0.0f;

        const SkShaper::RunHandler::RunInfo runInfo = {
                font,
                bidiLevel,
                script,
                language.c_str(),
                SkPoint::Make(runAdvanceX,
                              fontMetrics.fDescent - fontMetrics.fAscent + fontMetrics.fLeading),
                static_cast<size_t>(glyphCount),
                SkShaper::RunHandler::Range(0, runUtf8Range.width())};

        auto& run = fParagraph->fRuns.emplace_back(
            fParagraph,
            runInfo,
            runUtf8Range.start,
            runHeight,
            style->getHalfLeading(),
            style->getBaselineShift(),
            fParagraph->fRuns.size(),
            advanceX
        );
        // Also writes the N+1 sentinel (run end position and cluster index) that line breaking
        // and caret hit-testing interpolate against
        run.allocateGlyphData(static_cast<size_t>(glyphCount), /*hasOffsets=*/false);
        SkPoint* positions = run.fGlyphData->positions();
        uint32_t* clusterIndexes = run.fGlyphData->clusterIndexes();
        SkGlyphID* glyphs = run.fGlyphData->glyphs();

        if (isFallback) {
            // CoreText renders missing glyphs through LastResort; remap to glyph 0 (.notdef) to
            // honor SkParagraph unresolved contract
            SkScalar accumulatedX = 0;
            for (CFIndex k = 0; k < glyphCount; ++k) {
                glyphs[k] = 0;
                positions[k] = SkPoint::Make(advanceX + accumulatedX, 0);
                accumulatedX += glyphAdvances[k];
                clusterIndexes[k] = clusterBuffer[k];
                recordUnresolvedGlyph(glyphTextRange(k));
            }
        } else {
            if (shapingBuffer.fOffsets.size() < SkToInt(glyphCount)) {
                shapingBuffer.fOffsets.resize_back(SkToInt(glyphCount));
            }
            SkPoint* offsets = shapingBuffer.fOffsets.data();
            // CTRunGetPositions folds mark draw offsets in and lacks the non-subpixel and newline
            // adjustments, so the pen walks glyphAdvances instead
            SkScalar penX = 0;
            // A run of only characters CoreText drops (C0 controls) keeps one zero-advance
            // kCGFontIndexInvalid placeholder, no glyph id of the font; space draws nothing
            // KEEPER-DEBT: a font without a space glyph gets .notdef at zero advance; remove
            // once Run can hold a glyph-less cluster
            const SkGlyphID invisibleGlyph = font.unicharToGlyph(' ');
            for (CFIndex k = 0; k < glyphCount; ++k) {
                glyphs[k] =
                        glyphBuffer[k] == kCGFontIndexInvalid ? invisibleGlyph : glyphBuffer[k];

                // Zero-size text pins the origins to 0, as its advances
                const CGPoint origin = zeroSize ? CGPointZero : origins[k];
                // Cluster widths and hit-testing read the positions, and extendedVisit and
                // getPath hand the positions out without the offsets, so the whole origin goes
                // into the offset as SkShaper's HarfBuzz runs put x_offset and y_offset there
                positions[k] = SkPoint::Make(advanceX + penX, 0);
                penX += glyphAdvances[k];
                // CoreText is y-up, Skia y-down; splitOffsets restores drawn x after moving half
                // a kerning pair onto this glyph's advance
                offsets[k] = SkPoint::Make(
                        SkScalarFromCGFloat(origin.x) + (splitKerning ? splitOffsets[k] : 0),
                        -SkScalarFromCGFloat(origin.y));
                clusterIndexes[k] = clusterBuffer[k];
                if (glyphs[k] == 0) {
                    recordUnresolvedGlyph(glyphTextRange(k));
                }
            }
            run.commit(shapingBuffer);
        }

        // CoreText runs keep the end sentinel on the baseline, where allocateGlyphData puts it at
        // fOffset + fAdvance. commit() may have moved the glyph data, so read it again
        run.fGlyphData->positions()[glyphCount] = SkPoint::Make(advanceX + runAdvanceX, 0);

        fParagraph->fFontSwitches.emplace_back(runUtf8Range.start, run.font());
        advanceX += runAdvanceX;
    }

    return true;
}

}  // namespace textlayout
}  // namespace skia
