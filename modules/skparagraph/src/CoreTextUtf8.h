// Copyright 2026 Google LLC
#ifndef CoreTextUtf8_DEFINED
#define CoreTextUtf8_DEFINED

#include "include/core/SkTypes.h"
#include "src/core/SkUTF.h"

namespace skia {
namespace textlayout {

// U+FFFD, what CFString and CoreTextIndexMap hold for each invalid UTF-8 byte
inline constexpr SkUnichar kReplacementCharacter = 0xFFFD;

// Decodes the scalar at *ptr < end and moves ptr past it; an invalid or truncated sequence
// yields invalid (one U+FFFD per byte by default) and moves ptr by one byte
inline SkUnichar next_utf8_scalar(const char** ptr,
                                  const char* end,
                                  SkUnichar invalid = kReplacementCharacter) {
    SkASSERT(*ptr < end);
    const char* start = *ptr;
    SkUnichar u = SkUTF::NextUTF8(ptr, end);
    // SkUTF::NextUTF8 jumps to end on an error; one byte keeps the rest of the text decodable
    if (u < 0) {
        *ptr = start + 1;
        return invalid;
    }
    return u;
}

}  // namespace textlayout
}  // namespace skia

#endif  // CoreTextUtf8_DEFINED
