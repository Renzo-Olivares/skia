// Copyright 2026 Google LLC
#include "modules/skparagraph/src/CoreTextAttrStringBuilder.h"

#include "include/core/SkSpan.h"
#include "include/core/SkString.h"
#include "include/private/SkTo.h"
#include "modules/skparagraph/src/CoreTextUtf8.h"
#include "modules/skparagraph/src/Run.h"
#include "modules/skunicode/include/SkUnicode.h"

#include <algorithm>
#include <array>

namespace skia {
namespace textlayout {

namespace {

const CFStringRef kStyleGroupAttributeName = CFSTR("SkParagraphStyleGroup");

// ISO 15924 tags from SkUnicode::getScript. Latn also labels a run with no own-script letters
constexpr SkFourByteTag kLatn = SkSetFourByteTag('L', 'a', 't', 'n');
// Digits, spaces, punctuation (Zyyy) and combining marks (Zinh) join the neighbouring run
constexpr SkFourByteTag kCommon = SkSetFourByteTag('Z', 'y', 'y', 'y');
constexpr SkFourByteTag kInherited = SkSetFourByteTag('Z', 'i', 'n', 'h');

// Returns the script that starts or continues a script run at u, or 0 for Common and Inherited
// code points, which join the neighbouring run as in SkShaper's script iterator. Unknown
// ('Zzzz': unassigned and private use) starts its own run there too
SkFourByteTag run_script(SkUnicode* unicode, SkUnichar u) {
    // ASCII is the hot case and its scripts never change (letters Latn, the rest Common), so it
    // skips the virtual call and the table search
    if (u < 0x80) {
        return (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') ? kLatn : 0;
    }
    const SkFourByteTag script = unicode->getScript(u);
    return script == kCommon || script == kInherited ? 0 : script;
}

// Sets kCTParagraphStyleAttributeName to direction over fullRange
void set_base_direction(CFMutableAttributedStringRef attrStr,
                        CFRange fullRange,
                        TextDirection direction) {
    // CTParagraphStyle is immutable and safe to share across threads, and every segment of every
    // paragraph needs one of only two, so both live for the whole process: [0] LTR, [1] RTL
    static const auto kStyles = [] {
        std::array<CTParagraphStyleRef, 2> styles{};
        for (int i = 0; i < 2; ++i) {
            CTWritingDirection dir =
                    i ? kCTWritingDirectionRightToLeft : kCTWritingDirectionLeftToRight;
            CTParagraphStyleSetting dirSetting = {
                    kCTParagraphStyleSpecifierBaseWritingDirection, sizeof(dir), &dir};
            styles[i] = CTParagraphStyleCreate(&dirSetting, 1);
            SkASSERT_RELEASE(styles[i]);
        }
        return styles;
    }();
    CFAttributedStringSetAttribute(attrStr,
                                   fullRange,
                                   kCTParagraphStyleAttributeName,
                                   kStyles[direction == TextDirection::kRtl ? 1 : 0]);
}

// Tags cfRange of attrStr with group, a distinct value per non-coalesced span, so CoreText splits
// CTRuns at span boundaries even when two spans resolve to the same CTFont (e.g. LastResort
// fallback or differing SkFont edging/baselineShift)
void set_style_group(CFMutableAttributedStringRef attrStr, CFRange cfRange, int group) {
    SkUniqueCFRef<CFNumberRef> groupNum(
            CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &group));
    SkASSERT_RELEASE(groupNum);
    CFAttributedStringSetAttribute(attrStr, cfRange, kStyleGroupAttributeName, groupNum.get());
}

// Tags non-empty locale as kCTLanguageAttributeName on cfRange: Han unification leaves one code
// point per ideograph, so CoreText picks the regional cascade and locl forms from the string
// language rather than the machine's preferred languages (FontCollection::defaultFallback already
// receives locale on the HarfBuzz path)
void set_language(CFMutableAttributedStringRef attrStr, CFRange cfRange, const SkString& locale) {
    if (locale.isEmpty()) {
        return;
    }
    SkUniqueCFRef<CFStringRef> language(
            CFStringCreateWithCString(kCFAllocatorDefault, locale.c_str(), kCFStringEncodingUTF8));
    if (!language) {
        return;
    }
    CFAttributedStringSetAttribute(attrStr, cfRange, kCTLanguageAttributeName, language.get());
}

// CoreText ignores an OpenType 'kern'=0 feature dictionary entry; kCTKernAttributeName = 0 on the
// range suppresses pair kerning while keeping the font's trak table. The last 'kern' entry wins,
// as when HarfBuzz applies the feature list in order
bool disables_kerning(const TextStyle& style) {
    const auto& features = style.getFontFeatures();
    auto kern = std::find_if(features.rbegin(), features.rend(), [](const FontFeature& f) {
        return f.fName.equals("kern");
    });
    return kern != features.rend() && kern->fValue == 0;
}

// Sets kCTKernAttributeName to 0 on cfRange, see disables_kerning
void set_zero_kern(CFMutableAttributedStringRef attrStr, CFRange cfRange) {
    // One immutable CFNumber serves every span of every paragraph for the whole process
    static const CFNumberRef kZeroKern = [] {
        const float zero = 0.0f;
        CFNumberRef number = CFNumberCreate(kCFAllocatorDefault, kCFNumberFloatType, &zero);
        SkASSERT_RELEASE(number);
        return number;
    }();
    CFAttributedStringSetAttribute(attrStr, cfRange, kCTKernAttributeName, kZeroKern);
}

}  // namespace

CoreTextAttrStringBuilder::CoreTextAttrStringBuilder(std::string_view utf8,
                                                     TextDirection baseDirection,
                                                     SkUnicode* unicode)
        : fBaseDirection(baseDirection) {
    SkASSERT(unicode);

    SkFourByteTag current = 0;
    size_t runStart = 0;
    // TODO(jlavrova): decode once per paragraph and give each segment a slice of that UTF-16
    // text and map. Blockers: SkUnicode::extractUtfConversionMapping breaks on invalid UTF-8,
    // and leading Common code points after '\n' would join the previous segment's script
    fIndexMap.start(utf8.size());
    const char* ptr = utf8.data();
    const char* const end = ptr + utf8.size();
    // next_utf8_scalar consumes at least one byte, so the loop ends on malformed UTF-8 too
    while (ptr < end) {
        const size_t utf8Start = SkToSizeT(ptr - utf8.data());
        const SkUnichar u = next_utf8_scalar(&ptr, end);
        fIndexMap.append(utf8Start, SkToSizeT(ptr - utf8.data()), u);
        const SkFourByteTag script = run_script(unicode, u);
        if (script == 0 || script == current) {
            continue;
        }
        if (current != 0) {
            fScriptRuns.push_back({TextRange(runStart, utf8Start), current});
            runStart = utf8Start;
        }
        current = script;
    }
    fScriptRuns.push_back({TextRange(runStart, utf8.size()), current != 0 ? current : kLatn});
}

SkFourByteTag CoreTextAttrStringBuilder::scriptAt(size_t utf8Index) const {
    SkASSERT(!fScriptRuns.empty());
    for (const ScriptRun& run : fScriptRuns) {
        if (utf8Index < run.range.end) {
            return run.script;
        }
    }
    return fScriptRuns.back().script;
}

void CoreTextAttrStringBuilder::addStyle(TextRange utf8Range, const TextStyle& style) {
    if (utf8Range.start >= utf8Range.end || utf8Range.start >= fIndexMap.utf8Length()) {
        return;
    }
    size_t end = std::min(utf8Range.end, fIndexMap.utf8Length());
    // Merge adjacent spans that differ only in paint attributes so ligatures and kerning are
    // preserved across paint-only Block boundaries. A placeholder's U+FFFC never merges: the
    // shaper swaps its CTRun for the placeholder
    if (!fStyles.empty() && fStyles.back().range.end == utf8Range.start && !style.isPlaceholder() &&
        !fStyles.back().style.isPlaceholder() && can_coalesce_styles(fStyles.back().style, style)) {
        fStyles.back().range.end = end;
        return;
    }
    fStyles.push_back({TextRange(utf8Range.start, end), style});
}

SkUniqueCFRef<CFAttributedStringRef> CoreTextAttrStringBuilder::build(
        CoreTextRequestedFonts* fonts) {
    SkASSERT(fonts);
    // CFStringCreateWithBytes drops a leading U+FEFF from UTF-8 as a byte order mark, which
    // would shift every CFRange from CoreTextIndexMap; UTF-16 input keeps it as a character.
    // One U+FFFD per invalid byte, exactly as CoreTextIndexMap counts them
    const SkSpan<const uint16_t> utf16 = fIndexMap.utf16();
    const CFIndex utf16Length = SkTo<CFIndex>(utf16.size());
    SkUniqueCFRef<CFStringRef> cfStr(CFStringCreateWithCharacters(
            kCFAllocatorDefault, reinterpret_cast<const UniChar*>(utf16.data()), utf16Length));
    SkASSERT_RELEASE(cfStr);
    // CFStringCreateWithCharacters copies exactly utf16Length code units
    SkASSERT(CFStringGetLength(cfStr.get()) == utf16Length);

    SkUniqueCFRef<CFMutableAttributedStringRef> attrStr(
            CFAttributedStringCreateMutable(kCFAllocatorDefault, utf16Length));
    SkASSERT_RELEASE(attrStr);
    CFAttributedStringReplaceString(attrStr.get(), CFRangeMake(0, 0), cfStr.get());

    if (utf16Length == 0) {
        return SkUniqueCFRef<CFAttributedStringRef>(attrStr.release());
    }

    set_base_direction(attrStr.get(), CFRangeMake(0, utf16Length), fBaseDirection);

    for (int spanIdx = 0; spanIdx < fStyles.size(); ++spanIdx) {
        const StyleSpan& span = fStyles[spanIdx];
        const CFRange cfRange = fIndexMap.utf8RangeToCFRange(span.range);
        if (cfRange.length <= 0) {
            continue;
        }
        // Span 0 stays untagged so single-span text avoids CFNumber allocation while still
        // differing from groups 1..N on later spans
        if (spanIdx > 0) {
            set_style_group(attrStr.get(), cfRange, spanIdx);
        }
        CoreTextRequestedFonts::Entry* entry = fonts->findOrMake(span.style);
        CFAttributedStringSetAttribute(attrStr.get(), cfRange, kCTFontAttributeName, entry->font());
        // Only a letter-spaced span has liga=0 to take back from its cursive runs
        if (span.style.getLetterSpacing() != 0) {
            this->setCursiveRunsFont(attrStr.get(), span.range, entry);
        }
        set_language(attrStr.get(), cfRange, span.style.getLocale());
        if (disables_kerning(span.style)) {
            set_zero_kern(attrStr.get(), cfRange);
        }

        // Run::fBaselineShift is the sole carrier of the style's baselineShift: TextLine paints
        // each run at baseline + shift, so a kCTBaselineOffsetAttributeName (y-up) would land in
        // glyph fY with the opposite sign and cancel the visual shift
    }

    return SkUniqueCFRef<CFAttributedStringRef>(attrStr.release());
}

void CoreTextAttrStringBuilder::setCursiveRunsFont(CFMutableAttributedStringRef attrStr,
                                                   TextRange spanRange,
                                                   CoreTextRequestedFonts::Entry* entry) const {
    SkASSERT(entry);
    for (const ScriptRun& run : fScriptRuns) {
        // Script runs ignore style spans, so only the part inside this span changes font
        const size_t start = std::max(run.range.start, spanRange.start);
        const size_t end = std::min(run.range.end, spanRange.end);
        if (start >= end || !Run::IsCursiveScript(run.script)) {
            continue;
        }
        CFAttributedStringSetAttribute(attrStr,
                                       fIndexMap.utf8RangeToCFRange(TextRange(start, end)),
                                       kCTFontAttributeName,
                                       entry->cursiveFont());
    }
}

}  // namespace textlayout
}  // namespace skia
