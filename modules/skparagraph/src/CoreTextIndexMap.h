// Copyright 2026 Google LLC
#ifndef CoreTextIndexMap_DEFINED
#define CoreTextIndexMap_DEFINED

#include "include/core/SkSpan.h"
#include "include/core/SkTypes.h"
#include "include/private/SkTemplates.h"
#include "include/private/SkTo.h"
#include "modules/skparagraph/include/DartTypes.h"
#include "modules/skparagraph/include/TextStyle.h"

#include "src/core/SkUTF.h"

#include <CoreFoundation/CoreFoundation.h>

namespace skia {
namespace textlayout {

// UTF-8 byte offset <-> UTF-16 code unit mapping and the UTF-16 text for CoreText interop
// Stack buffers avoid heap allocations on layout hot paths for lines under 256 bytes
// TODO(jlavrova: 1406897): Fold malformed-UTF8 handling into
// SkUnicode::extractUtfConversionMapping once CoreText and SkUnicode callers are unified
class CoreTextIndexMap {
public:
    // Discards the mapping and the UTF-16 text and sizes both tables and the text for utf8Bytes
    // entries, an upper bound on the UTF-16 length too. The caller decodes the text itself with
    // next_utf8_scalar: start(), then append() per scalar in text order
    void start(size_t utf8Bytes) {
        fUtf16Length = 0;
        fUtf8ToUtf16.reset(SkToInt(utf8Bytes));
        fUtf16ToUtf8.reset(SkToInt(utf8Bytes));
        fUtf16.reset(SkToInt(utf8Bytes));
    }

    // Maps every byte in [utf8Start, utf8End) to the next UTF-16 position and its 1 or 2 UTF-16
    // units back to utf8Start, and appends those units of unichar to utf16()
    void append(size_t utf8Start, size_t utf8End, SkUnichar unichar) {
        SkASSERT(utf8Start < utf8End);
        SkASSERT(utf8End <= fUtf8ToUtf16.size());
        size_t u16Pos = fUtf16Length;
        for (size_t i = utf8Start; i < utf8End; ++i) {
            fUtf8ToUtf16[SkToInt(i)] = SkTo<uint32_t>(u16Pos);
        }
        fUtf16ToUtf8[SkToInt(u16Pos)] = SkTo<uint32_t>(utf8Start);
        fUtf16Length = u16Pos + 1;
        if (unichar >= 0x10000) {
            fUtf16ToUtf8[SkToInt(u16Pos + 1)] = SkTo<uint32_t>(utf8Start);
            ++fUtf16Length;
        }
        SkASSERT(fUtf16Length <= fUtf16.size());
        SkUTF::ToUTF16(unichar, &fUtf16[SkToInt(u16Pos)]);
    }

    // Length of the text given to start()
    size_t utf8Length() const { return fUtf8ToUtf16.size(); }

    size_t utf16Length() const { return fUtf16Length; }

    // The UTF-16 text appended so far, exactly utf16Length() units
    SkSpan<const uint16_t> utf16() const { return {fUtf16.data(), fUtf16Length}; }

    // Returns the UTF-16 index of the scalar that holds utf8Index: a byte inside a scalar maps
    // to its first code unit, an index at or past the end to the UTF-16 length
    size_t toUtf16Index(size_t utf8Index) const {
        if (utf8Index >= fUtf8ToUtf16.size()) {
            return fUtf16Length;
        }
        return fUtf8ToUtf16[SkTo<int>(utf8Index)];
    }

    // Returns the UTF-8 index of the first byte of the scalar that holds UTF-16 unit utf16Index,
    // so both units of a surrogate pair map to the same byte; an index at or past the UTF-16
    // length maps to the UTF-8 length
    size_t toUtf8Index(size_t utf16Index) const {
        if (utf16Index >= fUtf16Length) {
            return fUtf8ToUtf16.size();
        }
        return fUtf16ToUtf8[SkTo<int>(utf16Index)];
    }

    // Converts a half-open UTF-8 TextRange, start <= end, to the half-open UTF-16 CFRange
    // CoreText takes; a range inside one scalar gives an empty CFRange
    CFRange utf8RangeToCFRange(TextRange utf8Range) const {
        // toUtf16Index is monotonic, so start <= end keeps the length non-negative
        SkASSERT(utf8Range.start <= utf8Range.end);
        size_t loc = this->toUtf16Index(utf8Range.start);
        size_t end = this->toUtf16Index(utf8Range.end);
        return CFRangeMake(SkTo<CFIndex>(loc), SkTo<CFIndex>(end - loc));
    }

    // Converts a half-open UTF-16 CFRange with non-negative location and length to the
    // half-open UTF-8 TextRange for ParagraphImpl
    TextRange cfrangeToTextRange(CFRange cfRange) const {
        // toUtf8Index is monotonic, so the UTF-8 end never precedes the start
        SkASSERT(cfRange.location >= 0);
        SkASSERT(cfRange.length >= 0);
        size_t start = this->toUtf8Index(SkTo<size_t>(cfRange.location));
        size_t end = this->toUtf8Index(SkTo<size_t>(cfRange.location + cfRange.length));
        return TextRange(start, end);
    }

private:
    size_t fUtf16Length = 0;
    skia_private::AutoSTArray<256, uint32_t> fUtf8ToUtf16;
    skia_private::AutoSTArray<256, uint32_t> fUtf16ToUtf8;
    // Sized like the tables; only the first fUtf16Length units are text
    skia_private::AutoSTArray<256, uint16_t> fUtf16;
};

}  // namespace textlayout
}  // namespace skia

#endif  // CoreTextIndexMap_DEFINED
