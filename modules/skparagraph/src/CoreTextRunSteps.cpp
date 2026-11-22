// Copyright 2026 Google LLC
#include "modules/skparagraph/src/CoreTextRunSteps.h"

#include "include/core/SkString.h"
#include "include/private/SkTemplates.h"
#include "include/private/SkTo.h"
#include "modules/skparagraph/src/CoreTextFontRegistry.h"
#include "src/core/SkNoDestructor.h"
#include "src/utils/mac/SkCGBase.h"

#include <algorithm>
#include <array>
#include <type_traits>
#include <utility>

namespace skia {
namespace textlayout {

namespace {

// Writes each glyph's advance from CoreText's base advances, rounded to whole pixels when subpixel
// is off (the meaning of SkFont::isSubpixel), and when splitOffsets is non-empty moves half of
// each spacing glyph's pair kerning (base advance minus nominal advance) onto the next glyph's
// advance and x-offset so drawn x stays unchanged while the caret sits at kern/2
void coretext_advances(bool subpixel,
                       SkSpan<const CGSize> nominal,
                       SkSpan<const CGSize> advances,
                       SkSpan<SkScalar> out,
                       SkSpan<SkScalar> splitOffsets) {
    SkASSERT(advances.size() == out.size());
    // Half a kerning pair would put the next glyph off the pixel grid
    SkASSERT(subpixel || splitOffsets.empty());
    SkASSERT(splitOffsets.empty() ||
             (splitOffsets.size() == advances.size() && nominal.size() == advances.size()));
    const size_t count = advances.size();
    SkScalar carry = 0;
    for (size_t k = 0; k < count; ++k) {
        const SkScalar advance = SkScalarFromCGFloat(advances[k].width);
        if (splitOffsets.empty()) {
            out[k] = subpixel ? advance : SkScalarRoundToScalar(advance);
            continue;
        }
        splitOffsets[k] = carry;
        out[k] = advance + carry;
        carry = 0;
        // Split kerning only between adjacent non-zero-advance glyphs within the run
        if (advance != 0 && k + 1 < count && advances[k + 1].width != 0) {
            carry = (advance - SkScalarFromCGFloat(nominal[k].width)) / 2;
            out[k] -= carry;
        }
    }
}

// Compares cfStr and UTF-8 skStr case-insensitively without copying skStr's bytes
bool cfstring_equals_skstring(CFStringRef cfStr, const SkString& skStr) {
    SkASSERT(cfStr);
    if (skStr.isEmpty()) {
        return false;
    }
    // LIFETIME: kCFAllocatorNull borrows skStr's UTF-8 bytes; skCFStr is released at scope exit,
    // before the caller's SkString can change or die
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

}  // namespace

bool is_allowed_font(CTFontRef ctFont,
                     const std::vector<sk_sp<SkTypeface>>& allowedTypefaces,
                     const std::vector<SkString>& fontFamilies) {
    SkASSERT(ctFont);
    // Both names come from the font's name table: a data-backed font with an empty one (a
    // subsetted web font) has no family name, so a null name is data, not a failure
    SkUniqueCFRef<CFStringRef> ctPsName(CTFontCopyPostScriptName(ctFont));
    SkUniqueCFRef<CFStringRef> ctFamName(CTFontCopyFamilyName(ctFont));

    auto matchesPs = [&](const SkString& name) {
        return ctPsName && cfstring_equals_skstring(ctPsName.get(), name);
    };
    auto matchesFamily = [&](const SkString& name) {
        return ctFamName && cfstring_equals_skstring(ctFamName.get(), name);
    };
    auto matchesTypeface = [&](const SkTypeface* tf) {
        SkASSERT(tf);
        SkString tfPs, tfFam;
        tf->getPostScriptName(&tfPs);
        tf->getFamilyName(&tfFam);
        // FontArguments clone .AppleSystemUIFont* onto .SF NS* / .New York while createFont
        // shapes from .AppleSystemUIFont* at the span size, so compare UI design too
        return matchesPs(tfPs) || matchesFamily(tfFam) ||
               CoreTextFontRegistry::sameSystemFontDesign(
                       ctFamName.get(), tf, {tfFam.c_str(), tfFam.size()});
    };
    for (const auto& tf : allowedTypefaces) {
        if (matchesTypeface(tf.get())) {
            return true;
        }
    }
    return std::any_of(fontFamilies.begin(), fontFamilies.end(), [&](const SkString& fam) {
        return matchesFamily(fam) || matchesPs(fam);
    });
}

CTFontRef ct_run_font(CTRunRef ctRun) {
    // CoreText manufactures run attributes during font substitution, and they always carry the
    // font the glyphs were shaped with
    CTFontRef ctFont =
            (CTFontRef)CFDictionaryGetValue(CTRunGetAttributes(ctRun), kCTFontAttributeName);
    SkASSERT(ctFont);
    return ctFont;
}

bool is_last_resort_font(CTFontRef font) {
    SkASSERT(font);
    SkUniqueCFRef<CFStringRef> psName(CTFontCopyPostScriptName(font));
    if (!psName) {
        return false;
    }
    // Substring match handles system-prefixed PostScript names such as .LastResort alongside
    // plain LastResort
    const CFRange r = CFStringFind(psName.get(), CFSTR("LastResort"), kCFCompareCaseInsensitive);
    return r.location != kCFNotFound;
}

void apply_font_style_props(SkFont* font, const TextStyle& style, bool fakeMissingFontStyles) {
    SkASSERT(font);
    font->setEdging(style.getFontEdging());
    font->setHinting(style.getFontHinting());
    font->setSubpixel(style.getSubpixel());

    // CoreText binds the closest real face and never synthesizes a missing bold or italic,
    // so the run gets the same fake flags OneLineShaper derives from the typeface gap
    const SkTypeface* typeface = font->getTypeface();
    if (!fakeMissingFontStyles || !typeface) {
        return;
    }
    const SkFontStyle wanted = style.getFontStyle();
    const SkFontStyle bound = CoreTextFontRegistry::faceStyle(typeface);
    const bool fakeBold = wanted.weight() >= SkFontStyle::kSemiBold_Weight &&
                          wanted.weight() - bound.weight() >= 200;
    const bool fakeItalic = wanted.slant() == SkFontStyle::kItalic_Slant &&
                            bound.slant() != SkFontStyle::kItalic_Slant;
    font->setEmbolden(fakeBold);
    font->setSkewX(fakeItalic ? -SK_Scalar1 / 4 : 0);
}

bool has_non_positive_font_size(const TextStyle& style) { return !(style.getFontSize() > 0); }

SkFont make_run_font(sk_sp<SkTypeface> typeface,
                     CTFontRef ctFont,
                     const TextStyle& style,
                     bool fakeMissingFontStyles) {
    // CoreText reads a zero point size as its 12pt default, so the attributed string shapes
    // such text at a stand-in size and the run collapses to a zero-size font with zero
    // advances, matching HarfBuzz, so builds without HarfBuzz lay it out too
    const SkScalar fontSize =
            has_non_positive_font_size(style) ? 0 : SkScalarFromCGFloat(CTFontGetSize(ctFont));
    SkFont font(std::move(typeface), fontSize);
    apply_font_style_props(&font, style, fakeMissingFontStyles);
    return font;
}

uint8_t find_bidi_level(SkSpan<const SkUnicode::BidiRegion> regions,
                        size_t utf8Pos,
                        uint8_t defaultLevel) {
    // getBidiRegions returns disjoint regions in logical order; called once per run and
    // placeholder, so a linear scan would be quadratic in the region count
    const SkUnicode::BidiRegion* next = std::upper_bound(
            regions.begin(), regions.end(), utf8Pos, [](size_t pos, const auto& region) {
                return pos < region.start;
            });
    if (next != regions.begin() && utf8Pos < next[-1].end) {
        return next[-1].level;
    }
    return defaultLevel;
}

uint8_t reconcile_bidi_level(uint8_t level, CTRunRef ctRun) {
    const bool ctIsRtl = (CTRunGetStatus(ctRun) & kCTRunStatusRightToLeft) != 0;
    if ((level % 2 != 0) != ctIsRtl) {
        // Promote an LTR CTRun inside an odd level to level + 1 rather than dropping 1 -> 0 so
        // surrounding RTL runs at level 1 still reverse right-to-left around it
        return ctIsRtl ? (level | 1) : (level < 254 ? level + 1 : level - 1);
    }
    return level;
}

void normalize_cluster_indices(SkSpan<uint32_t> clusters, bool ltr) {
    const int glyphCount = SkToInt(clusters.size());
    // Sweep from the logical end toward the start (right-to-left in LTR, left-to-right in RTL)
    // so a reordered glyph clamps down to the earliest following cluster index, never past run end
    if (ltr) {
        for (int k = glyphCount - 2; k >= 0; --k) {
            clusters[k] = std::min(clusters[k], clusters[k + 1]);
        }
    } else {
        for (int k = 1; k < glyphCount; ++k) {
            clusters[k] = std::min(clusters[k], clusters[k - 1]);
        }
    }
}

SkUniqueCFRef<CTLineRef> typeset_line(CFAttributedStringRef attrStr) {
    // CTTypesetterCreateWithAttributedString disables GSUB/GPOS/morx/kerx above 10,240 UTF-16
    // code units unless unbounded layout is explicitly enabled
    static const SkNoDestructor<SkUniqueCFRef<CFDictionaryRef>> kUnboundedOptions([] {
        std::array<const void*, 1> keys = {kCTTypesetterOptionAllowUnboundedLayout};
        std::array<const void*, 1> values = {kCFBooleanTrue};
        SkUniqueCFRef<CFDictionaryRef> opts(CFDictionaryCreate(kCFAllocatorDefault,
                                                               keys.data(),
                                                               values.data(),
                                                               SkTo<CFIndex>(keys.size()),
                                                               &kCFTypeDictionaryKeyCallBacks,
                                                               &kCFTypeDictionaryValueCallBacks));
        SkASSERT_RELEASE(opts);
        return opts;
    }());
    const CFRange range = CFRangeMake(0, CFAttributedStringGetLength(attrStr));
    SkUniqueCFRef<CTTypesetterRef> typesetter(
            CTTypesetterCreateWithAttributedStringAndOptions(attrStr, kUnboundedOptions->get()));
    SkASSERT_RELEASE(typesetter);
    SkUniqueCFRef<CTLineRef> line(CTTypesetterCreateLine(typesetter.get(), range));
    SkASSERT_RELEASE(line);
    return line;
}

SkPoint glyph_draw_offset(CGPoint origin, bool zeroSize, SkScalar splitOffsetX) {
    const CGPoint pinned = zeroSize ? CGPointZero : origin;
    return SkPoint::Make(SkScalarFromCGFloat(pinned.x) + splitOffsetX,
                         -SkScalarFromCGFloat(pinned.y));
}

AdvanceMode advance_mode(bool zeroSize, bool subpixel, bool splitKerning) {
    // Half a kerning pair would put the next glyph off the pixel grid, and a zero-size run has no
    // kerning to split
    SkASSERT(!splitKerning || (subpixel && !zeroSize));
    if (zeroSize) {
        return AdvanceMode::kZero;
    }
    if (!subpixel) {
        return AdvanceMode::kRounded;
    }
    return splitKerning ? AdvanceMode::kSplitKerning : AdvanceMode::kSubpixel;
}

void extract_ctrun_glyphs(CTRunRef ctRun,
                          CTFontRef ctFont,
                          AdvanceMode mode,
                          SkSpan<SkGlyphID> glyphsOut,
                          SkSpan<SkPoint> offsetsOut,
                          SkSpan<SkScalar> advancesOut) {
    static_assert(std::is_same_v<CGGlyph, SkGlyphID>);
    const bool zeroSize = mode == AdvanceMode::kZero;
    const bool splitKerning = mode == AdvanceMode::kSplitKerning;
    SkASSERT(glyphsOut.size() == offsetsOut.size());
    SkASSERT(glyphsOut.size() == advancesOut.size());
    const CFIndex count = SkTo<CFIndex>(glyphsOut.size());
    const CFRange range = CFRangeMake(0, count);
    CTRunGetGlyphs(ctRun, range, glyphsOut.data());
    skia_private::AutoSTArray<64, CGSize> baseAdvances(glyphsOut.size());
    skia_private::AutoSTArray<64, CGPoint> origins(glyphsOut.size());
    // CTRunGetAdvances derives advances from drawn x, so a zero-advance mark drawn d off the pen
    // shows as +d on the glyph before it in the run and -d on the mark. Base advances leave mark
    // positioning to the origins, as HarfBuzz leaves it to x_offset
    CTRunGetBaseAdvancesAndOrigins(ctRun, range, baseAdvances.data(), origins.data());
    skia_private::AutoSTArray<64, SkScalar> splitOffsets(splitKerning ? count : 0);
    // Zero out the advances of the stand-in size CoreTextAttrStringBuilder gives zero-size spans
    if (zeroSize) {
        std::fill(advancesOut.begin(), advancesOut.end(), 0.0f);
    } else {
        skia_private::AutoSTArray<64, CGSize> nominal(splitOffsets.size());
        if (splitKerning) {
            SkASSERT(ctFont);
            // Query nominal glyph advances only when kerning is split; subtracting them from
            // baseAdvances isolates pair kerning without touching non-split runs
            CTFontGetAdvancesForGlyphs(
                    ctFont, kCTFontOrientationHorizontal, glyphsOut.data(), nominal.data(), count);
        }
        // TODO(jlavrova): this keeps CoreText's advance to the next 28pt tab stop, where HarfBuzz
        // gives U+0009 its hmtx advance; the stop counts from the CTLine start (hard break or
        // placeholder), so after a soft wrap it is off the line's stops — remove when tabs get the
        // HarfBuzz advance or line-relative stops
        coretext_advances(mode != AdvanceMode::kRounded,
                          {nominal.data(), nominal.size()},
                          {baseAdvances.data(), baseAdvances.size()},
                          advancesOut,
                          {splitOffsets.data(), splitOffsets.size()});
    }
    for (CFIndex k = 0; k < count; ++k) {
        offsetsOut[k] = glyph_draw_offset(origins[k], zeroSize, splitKerning ? splitOffsets[k] : 0);
    }
}

}  // namespace textlayout
}  // namespace skia
