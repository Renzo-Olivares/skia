// Copyright 2026 Google LLC
#include "modules/skparagraph/src/CoreTextIndexMap.h"

#include "include/private/SkTo.h"

namespace skia {
namespace textlayout {

CoreTextIndexMap::CoreTextIndexMap()
    : fUtf8Length(0)
    , fUtf16Length(0) {
    fUtf8ToUtf16.reset(1);
    fUtf8ToUtf16[0] = 0;
    fUtf16ToUtf8.reset(1);
    fUtf16ToUtf8[0] = 0;
}

CoreTextIndexMap::CoreTextIndexMap(const char* utf8, size_t utf8Bytes)
    : CoreTextIndexMap() {
    this->reset(utf8, utf8Bytes);
}

void CoreTextIndexMap::reset(const char* utf8, size_t utf8Bytes) {
    this->start(utf8 ? utf8Bytes : 0);
    const char* p = utf8;
    const char* const end = utf8 + fUtf8Length;
    // NextScalar consumes at least one byte, so the loop ends on malformed UTF-8 too
    while (p < end) {
        const char* prev = p;
        const SkUnichar u = NextScalar(&p, end);
        this->append(prev - utf8, p - utf8, u);
    }
    this->finish();
}

void CoreTextIndexMap::start(size_t utf8Bytes) {
    fUtf8Length = utf8Bytes;
    fUtf16Length = 0;
    // A scalar never takes more UTF-16 units than UTF-8 bytes, so utf8Bytes + 1 bounds both
    fUtf8ToUtf16.reset(SkToInt(fUtf8Length + 1));
    fUtf16ToUtf8.reset(SkToInt(fUtf8Length + 1));
}

void CoreTextIndexMap::finish() {
    fUtf8ToUtf16[SkToInt(fUtf8Length)] = SkTo<uint32_t>(fUtf16Length);
    fUtf16ToUtf8[SkToInt(fUtf16Length)] = SkTo<uint32_t>(fUtf8Length);
}

size_t CoreTextIndexMap::toUtf16Index(size_t utf8Index) const {
    if (utf8Index >= fUtf8Length) {
        return fUtf16Length;
    }
    return fUtf8ToUtf16[SkTo<int>(utf8Index)];
}

size_t CoreTextIndexMap::toUtf8Index(size_t utf16Index) const {
    if (utf16Index >= fUtf16Length) {
        return fUtf8Length;
    }
    return fUtf16ToUtf8[SkTo<int>(utf16Index)];
}

CFRange CoreTextIndexMap::utf8RangeToCFRange(TextRange utf8Range) const {
    size_t loc = this->toUtf16Index(utf8Range.start);
    size_t end = this->toUtf16Index(utf8Range.end);
    if (end < loc) {
        end = loc;
    }
    return CFRangeMake(SkTo<CFIndex>(loc), SkTo<CFIndex>(end - loc));
}

TextRange CoreTextIndexMap::cfrangeToTextRange(CFRange cfRange) const {
    if (cfRange.location < 0) {
        return TextRange(0, 0);
    }
    size_t start = this->toUtf8Index(SkTo<size_t>(cfRange.location));
    // CoreText CTRun string indices reference UTF-16 code units; resolve exclusive end through next
    // scalar boundary
    size_t endCf = cfRange.length > 0 ? SkTo<size_t>(cfRange.location + cfRange.length)
                                      : SkTo<size_t>(cfRange.location);
    size_t end = this->toUtf8Index(endCf);
    if (end < start) {
        end = start;
    }
    return TextRange(start, end);
}

}  // namespace textlayout
}  // namespace skia
