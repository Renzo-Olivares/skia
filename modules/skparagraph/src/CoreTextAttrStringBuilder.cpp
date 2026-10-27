// Copyright 2026 Google LLC
#include "modules/skparagraph/src/CoreTextAttrStringBuilder.h"

#include "include/core/SkString.h"
#include "include/private/SkAttributes.h"
#include "include/private/SkTemplates.h"
#include "include/private/SkTo.h"
#include "src/core/SkUTF.h"

#include <algorithm>
#include <iterator>

namespace skia {
namespace textlayout {

namespace {

static const CFStringRef kStyleGroupAttributeName = CFSTR("SkParagraphStyleGroup");

// Styles that share all font, metric, rasterization, and shaping attributes can coalesce into
// one CFAttributedString style span so paint-only boundaries (color, decoration, shadows) do not
// split CTRuns or ligatures
bool can_coalesce_styles(const TextStyle& a, const TextStyle& b) {
    return a.matchOneAttribute(StyleType::kFont, b) &&
           a.getTypeface() == b.getTypeface() &&
           a.getFontFeatures() == b.getFontFeatures() &&
           SkScalarNearlyEqual(a.getLetterSpacing(), b.getLetterSpacing());
}

// The font restyled by attrs, or font itself when there is nothing to apply or CoreText
// rejects the descriptor
SkUniqueCFRef<CTFontRef> copy_with_attributes(CTFontRef font,
                                              CGFloat fontSize,
                                              CFDictionaryRef attrs) {
    SkASSERT(font);
    if (CFDictionaryGetCount(attrs) > 0) {
        SkUniqueCFRef<CTFontDescriptorRef> desc(CTFontDescriptorCreateWithAttributes(attrs));
        CTFontRef styled = CTFontCreateCopyWithAttributes(font, fontSize, nullptr, desc.get());
        if (styled) {
            return SkUniqueCFRef<CTFontRef>(styled);
        }
    }
    return SkUniqueCFRef<CTFontRef>((CTFontRef)CFRetain(font));
}

constexpr SkFourByteTag kLatn = SkSetFourByteTag('L', 'a', 't', 'n');

struct ScriptBlock {
    SkUnichar fFirst;
    SkUnichar fLast;
    SkFourByteTag fScript;
};

// Tag for letters of scripts kScriptBlocks does not list. Their only consumers are
// Run::isCursiveScript, which turns letterSpacing off, and the run merge in ParagraphImpl, so a
// non-cursive placeholder is enough; SkUnicode has no script API to name them exactly
constexpr SkFourByteTag kUnlistedLetterScript = SkSetFourByteTag('Z', 'z', 'z', 'z');

// Unicode blocks with their scripts, sorted for binary search. Every block of the scripts
// Run::isCursiveScript calls cursive is listed, so no cursive letter falls through to
// kUnlistedLetterScript. A 0 entry is a block of Common code points (punctuation, symbols,
// emoji) that joins the neighbouring run without the CoreFoundation lookup
constexpr ScriptBlock kScriptBlocks[] = {
        {0x00C0, 0x02AF, kLatn},
        {0x0370, 0x03FF, SkSetFourByteTag('G', 'r', 'e', 'k')},
        {0x0400, 0x052F, SkSetFourByteTag('C', 'y', 'r', 'l')},
        {0x0530, 0x058F, SkSetFourByteTag('A', 'r', 'm', 'n')},
        {0x0590, 0x05FF, SkSetFourByteTag('H', 'e', 'b', 'r')},
        {0x0600, 0x06FF, SkSetFourByteTag('A', 'r', 'a', 'b')},
        {0x0700, 0x074F, SkSetFourByteTag('S', 'y', 'r', 'c')},
        {0x0750, 0x077F, SkSetFourByteTag('A', 'r', 'a', 'b')},
        {0x07C0, 0x07FF, SkSetFourByteTag('N', 'k', 'o', 'o')},
        {0x0840, 0x085F, SkSetFourByteTag('M', 'a', 'n', 'd')},
        {0x0860, 0x086F, SkSetFourByteTag('S', 'y', 'r', 'c')},
        {0x0870, 0x089F, SkSetFourByteTag('A', 'r', 'a', 'b')},
        {0x08A0, 0x08FF, SkSetFourByteTag('A', 'r', 'a', 'b')},
        {0x0900, 0x097F, SkSetFourByteTag('D', 'e', 'v', 'a')},
        {0x0E00, 0x0E7F, SkSetFourByteTag('T', 'h', 'a', 'i')},
        {0x10A0, 0x10FF, SkSetFourByteTag('G', 'e', 'o', 'r')},
        {0x1100, 0x11FF, SkSetFourByteTag('H', 'a', 'n', 'g')},
        {0x1800, 0x18AF, SkSetFourByteTag('M', 'o', 'n', 'g')},
        {0x1E00, 0x1EFF, kLatn},
        {0x1F00, 0x1FFF, SkSetFourByteTag('G', 'r', 'e', 'k')},
        {0x2000, 0x2BFF, 0},
        {0x2D00, 0x2D2F, SkSetFourByteTag('G', 'e', 'o', 'r')},
        {0x2DE0, 0x2DFF, SkSetFourByteTag('C', 'y', 'r', 'l')},
        {0x3000, 0x303F, 0},
        {0x3040, 0x309F, SkSetFourByteTag('H', 'i', 'r', 'a')},
        {0x30A0, 0x30FF, SkSetFourByteTag('K', 'a', 'n', 'a')},
        {0x3130, 0x318F, SkSetFourByteTag('H', 'a', 'n', 'g')},
        {0x31F0, 0x31FF, SkSetFourByteTag('K', 'a', 'n', 'a')},
        {0x3400, 0x4DBF, SkSetFourByteTag('H', 'a', 'n', 'i')},
        {0x4E00, 0x9FFF, SkSetFourByteTag('H', 'a', 'n', 'i')},
        {0xA640, 0xA69F, SkSetFourByteTag('C', 'y', 'r', 'l')},
        {0xA840, 0xA87F, SkSetFourByteTag('P', 'h', 'a', 'g')},
        {0xA8E0, 0xA8FF, SkSetFourByteTag('D', 'e', 'v', 'a')},
        {0xAC00, 0xD7AF, SkSetFourByteTag('H', 'a', 'n', 'g')},
        {0xF900, 0xFAFF, SkSetFourByteTag('H', 'a', 'n', 'i')},
        {0xFB1D, 0xFB4F, SkSetFourByteTag('H', 'e', 'b', 'r')},
        {0xFB50, 0xFDFF, SkSetFourByteTag('A', 'r', 'a', 'b')},
        {0xFE00, 0xFE0F, 0},
        {0xFE70, 0xFEFF, SkSetFourByteTag('A', 'r', 'a', 'b')},
        {0xFFF0, 0xFFFF, 0},
        {0x10D00, 0x10D3F, SkSetFourByteTag('R', 'o', 'h', 'g')},
        {0x11660, 0x1167F, SkSetFourByteTag('M', 'o', 'n', 'g')},
        {0x1F000, 0x1FAFF, 0},
        {0x20000, 0x2EBEF, SkSetFourByteTag('H', 'a', 'n', 'i')},
        {0xE0000, 0xE007F, 0},
};

constexpr bool blocks_sorted_and_disjoint() {
    for (size_t i = 0; i < std::size(kScriptBlocks); ++i) {
        if (kScriptBlocks[i].fFirst > kScriptBlocks[i].fLast ||
            (i > 0 && kScriptBlocks[i - 1].fLast >= kScriptBlocks[i].fFirst)) {
            return false;
        }
    }
    return true;
}
static_assert(blocks_sorted_and_disjoint(), "upper_bound in ResolveCodepointScript needs it");

// Classifies a code point outside kScriptBlocks: a combining mark still joins its base and a
// letter starts a non-cursive run instead of taking a cursive neighbour's script (0 and
// kUnlistedLetterScript). Never inlined: inlined into the decode loop it made the builder
// 1.17-1.28x slower on every bench text, ASCII too, against 1.01-1.02x as a call
SK_NEVER_INLINE SkFourByteTag classify_unlisted(SkUnichar c) {
    static const CFCharacterSetRef marks = CFCharacterSetGetPredefined(kCFCharacterSetNonBase);
    static const CFCharacterSetRef letters = CFCharacterSetGetPredefined(kCFCharacterSetLetter);
    if (CFCharacterSetIsLongCharacterMember(marks, static_cast<UTF32Char>(c))) {
        return 0;
    }
    return CFCharacterSetIsLongCharacterMember(letters, static_cast<UTF32Char>(c))
                   ? kUnlistedLetterScript
                   : 0;
}

// Returns the kScriptBlocks entry that holds c, or null when no listed block does
const ScriptBlock* find_block(SkUnichar c) {
    const ScriptBlock* next = std::upper_bound(
            std::begin(kScriptBlocks),
            std::end(kScriptBlocks),
            c,
            [](SkUnichar u, const ScriptBlock& block) { return u < block.fFirst; });
    return next != std::begin(kScriptBlocks) && c <= (next - 1)->fLast ? next - 1 : nullptr;
}

// Returns the script of c as ResolveCodepointScript defines it. lastBlock is the caller's last
// table hit, checked before the binary search and updated on a miss
SkFourByteTag resolve_script(SkUnichar c, const ScriptBlock** lastBlock) {
    // ASCII letters are the hot case and the only code points below U+00C0 the table calls Latin
    if (c < 0x80) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ? kLatn : 0;
    }
    const ScriptBlock* block = *lastBlock;
    if (!block || c < block->fFirst || c > block->fLast) {
        block = find_block(c);
        *lastBlock = block;
    }
    return block ? block->fScript : classify_unlisted(c);
}

}  // namespace

SkFourByteTag CoreTextAttrStringBuilder::ResolveCodepointScript(SkUnichar c) {
    const ScriptBlock* lastBlock = nullptr;
    return resolve_script(c, &lastBlock);
}

CoreTextAttrStringBuilder::CoreTextAttrStringBuilder(
        const char* utf8, size_t utf8Bytes,
        TextDirection baseDirection,
        sk_sp<FontCollection> fontCollection)
    : fUtf8(utf8)
    , fUtf8Bytes(utf8Bytes)
    , fBaseDirection(baseDirection)
    , fFontCollection(std::move(fontCollection))
    , fUtf16(SkToInt(utf8Bytes)) {
    SkASSERT(fUtf8);
    SkASSERT(fFontCollection);

    SkFourByteTag current = 0;
    size_t runStart = 0;
    // Neighbouring code points mostly share a block, so the last hit skips the binary search:
    // with it the builder takes 0.89-1.00x the time of the callback version on the bench texts,
    // without it up to 1.04x on mixed text
    const ScriptBlock* lastBlock = nullptr;
    // One decode pass gives the index map, the UTF-16 text for build(), and the script runs
    // TODO(jlavrova): decode once per paragraph and give each segment a slice of that UTF-16
    // text and map. Blockers: SkUnicode::extractUtfConversionMapping breaks on invalid UTF-8,
    // and leading Common code points after '\n' would join the previous segment's script
    fIndexMap.start(fUtf8Bytes);
    const char* ptr = fUtf8;
    const char* const end = fUtf8 + fUtf8Bytes;
    // NextScalar consumes at least one byte, so the loop ends on malformed UTF-8 too
    while (ptr < end) {
        const size_t utf8Start = ptr - fUtf8;
        const SkUnichar u = CoreTextIndexMap::NextScalar(&ptr, end);
        const size_t utf16Start = fIndexMap.append(utf8Start, ptr - fUtf8, u);
        SkASSERT(utf16Start + SkUTF::ToUTF16(u) <= SkToSizeT(fUtf16.count()));
        SkUTF::ToUTF16(u, fUtf16.data() + utf16Start);
        SkFourByteTag script = resolve_script(u, &lastBlock);
        if (script == 0 || script == current) {
            continue;
        }
        if (current != 0) {
            fScriptRuns.push_back({TextRange(runStart, utf8Start), current});
            runStart = utf8Start;
        }
        current = script;
    }
    fIndexMap.finish();
    fScriptRuns.push_back({TextRange(runStart, fUtf8Bytes), current != 0 ? current : kLatn});
}

SkFourByteTag CoreTextAttrStringBuilder::scriptAt(size_t utf8Index) const {
    SkASSERT(!fScriptRuns.empty());
    for (const ScriptRun& run : fScriptRuns) {
        if (utf8Index < run.fRange.end) {
            return run.fScript;
        }
    }
    return fScriptRuns.back().fScript;
}

void CoreTextAttrStringBuilder::addStyle(TextRange utf8Range, const TextStyle& style) {
    if (utf8Range.start >= utf8Range.end || utf8Range.start >= fUtf8Bytes) {
        return;
    }
    size_t end = std::min(utf8Range.end, fUtf8Bytes);
    // Merge adjacent spans that differ only in paint attributes so ligatures and kerning are
    // preserved across paint-only Block boundaries
    if (!fStyles.empty() &&
        fStyles.back().fRange.end == utf8Range.start &&
        can_coalesce_styles(fStyles.back().fStyle, style)) {
        fStyles.back().fRange.end = end;
        return;
    }
    fStyles.push_back({TextRange(utf8Range.start, end), style});
}

SkUniqueCFRef<CFAttributedStringRef> CoreTextAttrStringBuilder::build() {
    // CFStringCreateWithBytes drops a leading U+FEFF from UTF-8 as a byte order mark, which
    // would shift every CFRange from CoreTextIndexMap; UTF-16 input keeps it as a character.
    // One U+FFFD per invalid byte, exactly as CoreTextIndexMap counts them
    const size_t utf16Length = fIndexMap.toUtf16Index(fUtf8Bytes);
    SkUniqueCFRef<CFStringRef> cfStr(CFStringCreateWithCharacters(
            kCFAllocatorDefault, reinterpret_cast<const UniChar*>(fUtf16.data()),
            SkTo<CFIndex>(utf16Length)));
    if (!cfStr) {
        return nullptr;
    }

    CFIndex utf16Len = CFStringGetLength(cfStr.get());
    SkUniqueCFRef<CFMutableAttributedStringRef> attrStr(
        CFAttributedStringCreateMutable(kCFAllocatorDefault, utf16Len));
    CFAttributedStringReplaceString(attrStr.get(), CFRangeMake(0, 0), cfStr.get());

    if (utf16Len == 0) {
        return SkUniqueCFRef<CFAttributedStringRef>(attrStr.release());
    }

    CFRange fullRange = CFRangeMake(0, utf16Len);

    // Base writing direction controls CoreText paragraph-level BiDi embedding
    CTWritingDirection dir = (fBaseDirection == TextDirection::kRtl)
        ? kCTWritingDirectionRightToLeft
        : kCTWritingDirectionLeftToRight;
    CTParagraphStyleSetting dirSetting = {
        kCTParagraphStyleSpecifierBaseWritingDirection,
        sizeof(CTWritingDirection),
        &dir
    };
    SkUniqueCFRef<CTParagraphStyleRef> pStyle(CTParagraphStyleCreate(&dirSetting, 1));
    if (pStyle) {
        CFAttributedStringSetAttribute(
            attrStr.get(), fullRange, kCTParagraphStyleAttributeName, pStyle.get());
    }

    for (int spanIdx = 0; spanIdx < fStyles.size(); ++spanIdx) {
        const auto& span = fStyles[spanIdx];
        CFRange cfRange = fIndexMap.utf8RangeToCFRange(span.fRange);
        if (cfRange.length <= 0) {
            continue;
        }
        // Tag each non-coalesced span with a distinct attribute value so CoreText splits CTRuns at
        // span boundaries even when two spans resolve to the same CTFont (e.g. LastResort fallback
        // or differing SkFont edging/baselineShift)
        if (spanIdx > 0) {
            SkUniqueCFRef<CFNumberRef> groupNum(
                CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &spanIdx));
            if (groupNum) {
                CFAttributedStringSetAttribute(
                    attrStr.get(), cfRange, kStyleGroupAttributeName, groupNum.get());
            }
        }

        sk_sp<SkTypeface> primary = span.fStyle.refTypeface();
        std::vector<sk_sp<SkTypeface>> typefaces;
        const std::optional<FontArguments>& fontArgs = span.fStyle.getFontArguments();
        // Empty font families must bypass FontCollection fallback to allow CoreText to bind
        // .AppleSystemUIFont
        if (!primary && !span.fStyle.getFontFamilies().empty()) {
            // Clones carry the variation in their CTFontDescriptor, so the primary font and
            // every cascade entry below shape with the same axes as the HarfBuzz path
            typefaces = CoreTextFontRegistry::Instance().findTypefaces(
                    *fFontCollection,
                    span.fStyle.getFontFamilies(),
                    span.fStyle.getFontStyle(),
                    fontArgs);
            if (!typefaces.empty()) {
                primary = typefaces.front();
            }
        }

        CGFloat fontSize = span.fStyle.getFontSize() > 0.0f ? span.fStyle.getFontSize() : 14.0f;
        SkUniqueCFRef<CTFontRef> baseFont = CoreTextFontRegistry::Instance().createFont(
                primary.get(), span.fStyle.getFontStyle(), fontSize);

        SkUniqueCFRef<CFMutableDictionaryRef> descAttrs(
            CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
                                      &kCFTypeDictionaryKeyCallBacks,
                                      &kCFTypeDictionaryValueCallBacks));

        // Cascade list provides font fallback chain for characters outside primary typeface
        const bool fallbackDisabled = !fFontCollection->fontFallbackEnabled();
        if (typefaces.size() > 1 || fallbackDisabled) {
            SkUniqueCFRef<CFMutableArrayRef> cascadeList(
                CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks));
            for (size_t i = 1; i < typefaces.size(); ++i) {
                SkUniqueCFRef<CTFontDescriptorRef> desc =
                        CoreTextFontRegistry::Instance().getDescriptor(typefaces[i].get());
                if (desc) {
                    CFArrayAppendValue(cascadeList.get(), desc.get());
                }
            }
            if (fallbackDisabled) {
                SkUniqueCFRef<CTFontRef> lrFont(
                        CTFontCreateWithName(CFSTR("LastResort"), fontSize, nullptr));
                if (lrFont) {
                    SkUniqueCFRef<CTFontDescriptorRef> lrDesc(
                            CTFontCopyFontDescriptor(lrFont.get()));
                    if (lrDesc) {
                        CFArrayAppendValue(cascadeList.get(), lrDesc.get());
                    }
                }
            }
            if (CFArrayGetCount(cascadeList.get()) > 0) {
                CFDictionarySetValue(
                        descAttrs.get(), kCTFontCascadeListAttribute, cascadeList.get());
            }
        }

        // CoreText expects 4-character OpenType feature tags packed into descriptor settings
        const auto& features = span.fStyle.getFontFeatures();
        const SkScalar letterSpacing = span.fStyle.getLetterSpacing();
        // letterSpacing needs feature settings even without user features; the extra array
        // slot holds its liga=0
        if (!features.empty() || letterSpacing != 0) {
            SkUniqueCFRef<CFMutableArrayRef> featureArray(
                CFArrayCreateMutable(kCFAllocatorDefault, features.size() + 1,
                                     &kCFTypeArrayCallBacks));
            auto appendFeature = [&](const char* tag, int val) {
                SkUniqueCFRef<CFStringRef> tagStr(
                    CFStringCreateWithCString(kCFAllocatorDefault, tag, kCFStringEncodingASCII));
                SkUniqueCFRef<CFNumberRef> valNum(
                    CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &val));
                const void* fKeys[] = {kCTFontOpenTypeFeatureTag, kCTFontOpenTypeFeatureValue};
                const void* fVals[] = { tagStr.get(), valNum.get() };
                SkUniqueCFRef<CFDictionaryRef> featDict(CFDictionaryCreate(
                    kCFAllocatorDefault, fKeys, fVals, 2,
                    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
                CFArrayAppendValue(featureArray.get(), featDict.get());
            };
            for (const auto& feat : features) {
                if (feat.fName.size() == 4) {
                    appendFeature(feat.fName.c_str(), feat.fValue);
                }
            }
            // KEEPER-DEBT: OneLineShaper keeps liga for negative letterSpacing; native tracking and
            // CSS Text 3 drop optional ligatures for any nonzero spacing. Remove once it uses != 0.
            // Spacing goes between clusters, so a ligature would hide it; liga goes off for the
            // whole block after the user features, cursive scripts included, as in OneLineShaper
            if (letterSpacing != 0) {
                appendFeature("liga", 0);
            }
            if (CFArrayGetCount(featureArray.get()) > 0) {
                CFDictionarySetValue(
                        descAttrs.get(), kCTFontFeatureSettingsAttribute, featureArray.get());
            }
        }

        // The registry rebuilds system fonts by name and drops cloned variations, so only the
        // requested axes are re-applied: CoreText merges them and keeps deriving opsz from the
        // point size, which a full design position would pin. Palette overrides have no
        // CoreText equivalent and stay HarfBuzz-only
        if (fontArgs.has_value() && !fontArgs->getVariationCoordinates().empty()) {
            SkUniqueCFRef<CFMutableDictionaryRef> variation(
                CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
                                          &kCFTypeDictionaryKeyCallBacks,
                                          &kCFTypeDictionaryValueCallBacks));
            // css-fonts-4: the last value given for an axis wins, which SetValue provides
            for (const auto& coord : fontArgs->getVariationCoordinates()) {
                int64_t axisTag = coord.axis;
                double axisValue = coord.value;
                SkUniqueCFRef<CFNumberRef> tagNum(
                    CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &axisTag));
                SkUniqueCFRef<CFNumberRef> valNum(
                    CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType, &axisValue));
                if (tagNum && valNum) {
                    CFDictionarySetValue(variation.get(), tagNum.get(), valNum.get());
                }
            }
            if (CFDictionaryGetCount(variation.get()) > 0) {
                CFDictionarySetValue(descAttrs.get(), kCTFontVariationAttribute, variation.get());
            }
        }

        SkUniqueCFRef<CTFontRef> font =
                copy_with_attributes(baseFont.get(), fontSize, descAttrs.get());
        CFAttributedStringSetAttribute(attrStr.get(), cfRange, kCTFontAttributeName, font.get());

        // Han unification leaves one code point per ideograph, so CoreText picks the regional
        // cascade (and locl forms) from the string language; without the tag it follows the
        // machine's preferred languages, unlike the HarfBuzz path where
        // FontCollection::defaultFallback already receives this locale
        SkString locale = span.fStyle.getLocale();
        if (!locale.isEmpty()) {
            SkUniqueCFRef<CFStringRef> language(CFStringCreateWithCString(
                kCFAllocatorDefault, locale.c_str(), kCFStringEncodingUTF8));
            if (language) {
                CFAttributedStringSetAttribute(
                    attrStr.get(), cfRange, kCTLanguageAttributeName, language.get());
            }
        }

        // Run::fBaselineShift is the sole carrier of the style's baselineShift: TextLine paints
        // each run at baseline + shift, so a kCTBaselineOffsetAttributeName (y-up) would land in
        // glyph fY with the opposite sign and cancel the visual shift
    }

    return SkUniqueCFRef<CFAttributedStringRef>(attrStr.release());
}

}  // namespace textlayout
}  // namespace skia
