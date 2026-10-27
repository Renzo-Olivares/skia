// Copyright 2026 Google LLC
#ifndef CoreTextIndexMap_DEFINED
#define CoreTextIndexMap_DEFINED

#include "include/core/SkTypes.h"
#include "include/private/SkTemplates.h"
#include "include/private/SkTo.h"
#include "modules/skparagraph/include/DartTypes.h"
#include "modules/skparagraph/include/ParagraphStyle.h"
#include "modules/skparagraph/include/TextStyle.h"

#include "src/core/SkUTF.h"

#include <CoreFoundation/CoreFoundation.h>

namespace skia {
namespace textlayout {

// Bidirectional UTF-8 byte offset <-> UTF-16 code unit mapping for CoreText interop
// Stack buffer avoids heap allocations on layout hot paths for lines under 256 bytes
// TODO(jlavrova: 1406897): Fold malformed-UTF8 handling into
// SkUnicode::extractUtfConversionMapping once CoreText and SkUnicode callers are unified
class CoreTextIndexMap {
public:
    CoreTextIndexMap();
    CoreTextIndexMap(const char* utf8, size_t utf8Bytes);

    // Returns the scalar that ptr points at and moves ptr past it; an invalid or truncated
    // sequence gives one U+FFFD and moves ptr by exactly one byte. ptr must point before end.
    // Every decode loop over text this map indexes uses it, so all count invalid bytes alike
    static SkUnichar NextScalar(const char** ptr, const char* end);

    // Replaces the mapping with one for utf8[0, utf8Bytes); a null utf8 gives the empty map
    // and each invalid byte counts as one U+FFFD
    void reset(const char* utf8, size_t utf8Bytes);

    // Discards the mapping and sizes both tables for utf8Bytes + 1 entries, an upper bound on
    // both lengths, so the heap is used from 256 bytes up. For a caller that decodes the text
    // itself: start(), append() per scalar in text order, then finish() give what reset() gives
    void start(size_t utf8Bytes);
    // Maps the scalar at [utf8Start, utf8End), right after the previous one, to the next UTF-16
    // position (two units above U+FFFF) and returns that position
    size_t append(size_t utf8Start, size_t utf8End, SkUnichar unichar);
    // Writes the end entries of both tables; the mapping is complete after it
    void finish();

    // Returns the UTF-16 index of the scalar that holds utf8Index: a byte inside a scalar maps
    // to its first code unit, an index at or past the end to the UTF-16 length
    size_t toUtf16Index(size_t utf8Index) const;
    // CoreText uses UTF-16 code units; intra-codepoint offsets resolve to leading scalar byte
    size_t toUtf8Index(size_t utf16Index) const;

    // Converts a half-open UTF-8 TextRange to the half-open UTF-16 CFRange CoreText takes;
    // an inverted range gives an empty CFRange at its start
    CFRange utf8RangeToCFRange(TextRange utf8Range) const;
    // Converts half-open UTF-16 CFRange to half-open UTF-8 TextRange for ParagraphImpl
    TextRange cfrangeToTextRange(CFRange cfRange) const;

private:
    size_t fUtf8Length;
    size_t fUtf16Length;
    skia_private::AutoSTArray<256, uint32_t> fUtf8ToUtf16;
    skia_private::AutoSTArray<256, uint32_t> fUtf16ToUtf8;
};

inline SkUnichar CoreTextIndexMap::NextScalar(const char** ptr, const char* end) {
    SkASSERT(*ptr < end);
    const char* start = *ptr;
    SkUnichar u = SkUTF::NextUTF8(ptr, end);
    // SkUTF::NextUTF8 jumps to end on an error; one byte keeps the rest of the text decodable
    if (u < 0) {
        *ptr = start + 1;
        return 0xFFFD;
    }
    return u;
}

// Inline: CoreTextAttrStringBuilder calls it once per scalar on the layout path
inline size_t CoreTextIndexMap::append(size_t utf8Start, size_t utf8End, SkUnichar unichar) {
    SkASSERT(utf8Start < utf8End && utf8End <= fUtf8Length);
    const size_t u16Pos = fUtf16Length;
    // Map trailing UTF-8 bytes to the first UTF-16 unit of the scalar value
    for (size_t i = utf8Start; i < utf8End; ++i) {
        fUtf8ToUtf16[SkToInt(i)] = SkTo<uint32_t>(u16Pos);
    }
    // Supplementary code points take a surrogate pair; both units map back to the first byte
    const size_t u16Units = unichar >= 0x10000 ? 2 : 1;
    for (size_t i = 0; i < u16Units; ++i) {
        fUtf16ToUtf8[SkToInt(u16Pos + i)] = SkTo<uint32_t>(utf8Start);
    }
    fUtf16Length = u16Pos + u16Units;
    return u16Pos;
}

}  // namespace textlayout
}  // namespace skia

#endif  // CoreTextIndexMap_DEFINED
