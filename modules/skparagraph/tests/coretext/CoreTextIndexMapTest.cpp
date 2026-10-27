/*
 * Copyright 2026 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "include/core/SkString.h"
#include "include/core/SkTypes.h"
#include "include/private/SkTo.h"
#include "modules/skparagraph/src/CoreTextIndexMap.h"
#include "modules/skparagraph/src/CoreTextUtf8.h"
#include "tests/Test.h"

#include <CoreFoundation/CoreFoundation.h>
#include <string.h>

#include <string_view>

using namespace skia::textlayout;

namespace {
// Fills a map for utf8 through start/append with next_utf8_scalar, the same decode loop
// CoreTextAttrStringBuilder runs, so the IndexMap tests check the production path
CoreTextIndexMap build_index_map(std::string_view utf8) {
    CoreTextIndexMap map;
    map.start(utf8.size());
    const char* ptr = utf8.data();
    const char* const end = ptr + utf8.size();
    // next_utf8_scalar consumes at least one byte, so the loop ends on malformed UTF-8 too
    while (ptr < end) {
        const size_t utf8Start = SkToSizeT(ptr - utf8.data());
        const SkUnichar u = next_utf8_scalar(&ptr, end);
        map.append(utf8Start, SkToSizeT(ptr - utf8.data()), u);
    }
    return map;
}

// Checks text of units scalars that take width UTF-8 bytes and one UTF-16 unit each: unit i
// maps to byte i * width, and every byte of scalar i maps back to unit i
void check_fixed_width(skiatest::Reporter* reporter,
                       const char* label,
                       const CoreTextIndexMap& map,
                       size_t width,
                       size_t units) {
    for (size_t u16 = 0; u16 < units; ++u16) {
        skiatest::ReporterContext ctx(reporter, SkStringPrintf("%s u16=%zu", label, u16));
        REPORTER_ASSERT(reporter,
                        map.toUtf8Index(u16) == u16 * width,
                        "got %zu, want %zu",
                        map.toUtf8Index(u16),
                        u16 * width);
        for (size_t b = 0; b < width; ++b) {
            REPORTER_ASSERT(reporter,
                            map.toUtf16Index(u16 * width + b) == u16,
                            "got %zu, want %zu",
                            map.toUtf16Index(u16 * width + b),
                            u16);
        }
    }
}
}  // namespace

// Empty, ASCII, 2-byte and 3-byte text map both ways and round-trip ranges; a byte inside a
// scalar must resolve to that scalar's code unit, or CFRanges would split characters
DEF_TEST(CoreTextPipeline_IndexMap_AsciiAndMultibyte, reporter) {
    CoreTextIndexMap emptyMap = build_index_map("");
    REPORTER_ASSERT(reporter, emptyMap.toUtf16Index(0) == 0, "got %zu", emptyMap.toUtf16Index(0));
    REPORTER_ASSERT(reporter, emptyMap.toUtf8Index(0) == 0, "got %zu", emptyMap.toUtf8Index(0));
    REPORTER_ASSERT(reporter, emptyMap.toUtf16Index(10) == 0, "got %zu", emptyMap.toUtf16Index(10));
    REPORTER_ASSERT(reporter, emptyMap.toUtf8Index(10) == 0, "got %zu", emptyMap.toUtf8Index(10));

    // Validate fast path for single-byte encoding and range roundtrips
    const char* ascii = "Hello, world!";
    size_t asciiLen = strlen(ascii);
    CoreTextIndexMap asciiMap = build_index_map({ascii, asciiLen});
    for (size_t i = 0; i <= asciiLen; ++i) {
        skiatest::ReporterContext ctx(reporter, SkStringPrintf("ascii %zu", i));
        REPORTER_ASSERT(reporter,
                        asciiMap.toUtf16Index(i) == i,
                        "got %zu, want %zu",
                        asciiMap.toUtf16Index(i),
                        i);
        REPORTER_ASSERT(reporter,
                        asciiMap.toUtf8Index(i) == i,
                        "got %zu, want %zu",
                        asciiMap.toUtf8Index(i),
                        i);
    }
    CFRange cfRange = asciiMap.utf8RangeToCFRange(TextRange(0, 5));
    REPORTER_ASSERT(reporter,
                    cfRange.location == 0 && cfRange.length == 5,
                    "got (%ld, %ld), want (0, 5)",
                    static_cast<long>(cfRange.location),
                    static_cast<long>(cfRange.length));
    TextRange u8Range = asciiMap.cfrangeToTextRange(cfRange);
    REPORTER_ASSERT(reporter,
                    u8Range.start == 0 && u8Range.end == 5,
                    "got [%zu, %zu), want [0, 5)",
                    u8Range.start,
                    u8Range.end);

    // Cyrillic code points expand to 2 bytes in UTF-8 but 1 code unit in UTF-16
    const char* cyrillic = "Привет";
    size_t cyrBytes = strlen(cyrillic);
    REPORTER_ASSERT(reporter, cyrBytes == 12, "got %zu", cyrBytes);
    CoreTextIndexMap cyrMap = build_index_map({cyrillic, cyrBytes});
    check_fixed_width(reporter, "cyrillic", cyrMap, 2, 6);
    CFRange cyrCf = cyrMap.utf8RangeToCFRange(TextRange(0, 4));
    REPORTER_ASSERT(reporter,
                    cyrCf.location == 0 && cyrCf.length == 2,
                    "got (%ld, %ld), want (0, 2)",
                    static_cast<long>(cyrCf.location),
                    static_cast<long>(cyrCf.length));
    TextRange cyrBack = cyrMap.cfrangeToTextRange(cyrCf);
    REPORTER_ASSERT(reporter,
                    cyrBack.start == 0 && cyrBack.end == 4,
                    "got [%zu, %zu), want [0, 4)",
                    cyrBack.start,
                    cyrBack.end);

    const char* cjk = "日本語";
    size_t cjkBytes = strlen(cjk);
    REPORTER_ASSERT(reporter, cjkBytes == 9, "got %zu", cjkBytes);
    CoreTextIndexMap cjkMap = build_index_map({cjk, cjkBytes});
    check_fixed_width(reporter, "cjk", cjkMap, 3, 3);
}

// A 4-byte scalar takes two UTF-16 units, and both map back to its first byte; 400 bytes also
// check the tables past the 256-entry stack buffer
DEF_TEST(CoreTextPipeline_IndexMap_EmojiAndSurrogatePairs, reporter) {
    // Emoji code points require 4 UTF-8 bytes and surrogate pairs in UTF-16
    const char* singleEmoji = "😀";
    size_t emojiBytes = strlen(singleEmoji);
    REPORTER_ASSERT(reporter, emojiBytes == 4, "got %zu", emojiBytes);
    CoreTextIndexMap emojiMap = build_index_map({singleEmoji, emojiBytes});

    for (size_t b = 0; b < 4; ++b) {
        skiatest::ReporterContext ctx(reporter, SkStringPrintf("emoji byte %zu", b));
        REPORTER_ASSERT(
                reporter, emojiMap.toUtf16Index(b) == 0, "got %zu", emojiMap.toUtf16Index(b));
    }
    REPORTER_ASSERT(reporter, emojiMap.toUtf16Index(4) == 2, "got %zu", emojiMap.toUtf16Index(4));
    REPORTER_ASSERT(reporter, emojiMap.toUtf8Index(0) == 0, "got %zu", emojiMap.toUtf8Index(0));
    REPORTER_ASSERT(reporter, emojiMap.toUtf8Index(1) == 0, "got %zu", emojiMap.toUtf8Index(1));
    REPORTER_ASSERT(reporter, emojiMap.toUtf8Index(2) == 4, "got %zu", emojiMap.toUtf8Index(2));

    CFRange cfEmoji = emojiMap.utf8RangeToCFRange(TextRange(0, 4));
    REPORTER_ASSERT(reporter,
                    cfEmoji.location == 0 && cfEmoji.length == 2,
                    "got (%ld, %ld), want (0, 2)",
                    static_cast<long>(cfEmoji.location),
                    static_cast<long>(cfEmoji.length));
    TextRange u8Emoji = emojiMap.cfrangeToTextRange(cfEmoji);
    REPORTER_ASSERT(reporter,
                    u8Emoji.start == 0 && u8Emoji.end == 4,
                    "got [%zu, %zu), want [0, 4)",
                    u8Emoji.start,
                    u8Emoji.end);

    const char* mixed = "A😀B";
    size_t mixedBytes = strlen(mixed);
    REPORTER_ASSERT(reporter, mixedBytes == 6, "got %zu", mixedBytes);
    CoreTextIndexMap mixedMap = build_index_map({mixed, mixedBytes});

    REPORTER_ASSERT(reporter, mixedMap.toUtf16Index(0) == 0, "got %zu", mixedMap.toUtf16Index(0));
    REPORTER_ASSERT(reporter, mixedMap.toUtf16Index(1) == 1, "got %zu", mixedMap.toUtf16Index(1));
    REPORTER_ASSERT(reporter, mixedMap.toUtf16Index(5) == 3, "got %zu", mixedMap.toUtf16Index(5));
    REPORTER_ASSERT(reporter, mixedMap.toUtf16Index(6) == 4, "got %zu", mixedMap.toUtf16Index(6));

    REPORTER_ASSERT(reporter, mixedMap.toUtf8Index(0) == 0, "got %zu", mixedMap.toUtf8Index(0));
    REPORTER_ASSERT(reporter, mixedMap.toUtf8Index(1) == 1, "got %zu", mixedMap.toUtf8Index(1));
    REPORTER_ASSERT(reporter, mixedMap.toUtf8Index(2) == 1, "got %zu", mixedMap.toUtf8Index(2));
    REPORTER_ASSERT(reporter, mixedMap.toUtf8Index(3) == 5, "got %zu", mixedMap.toUtf8Index(3));
    REPORTER_ASSERT(reporter, mixedMap.toUtf8Index(4) == 6, "got %zu", mixedMap.toUtf8Index(4));

    CFRange cfB = mixedMap.utf8RangeToCFRange(TextRange(5, 6));
    REPORTER_ASSERT(reporter,
                    cfB.location == 3 && cfB.length == 1,
                    "got (%ld, %ld), want (3, 1)",
                    static_cast<long>(cfB.location),
                    static_cast<long>(cfB.length));
    TextRange u8B = mixedMap.cfrangeToTextRange(cfB);
    REPORTER_ASSERT(reporter,
                    u8B.start == 5 && u8B.end == 6,
                    "got [%zu, %zu), want [5, 6)",
                    u8B.start,
                    u8B.end);

    // Verify dynamic buffer expansion when input exceeds the 256-element stack threshold
    SkString manyEmojis;
    constexpr int kEmojiCount = 100;
    for (int i = 0; i < kEmojiCount; ++i) {
        manyEmojis.append(singleEmoji);
    }
    REPORTER_ASSERT(reporter, manyEmojis.size() == 400, "got %zu", manyEmojis.size());
    CoreTextIndexMap heapMap = build_index_map({manyEmojis.c_str(), manyEmojis.size()});
    for (int i = 0; i < kEmojiCount; ++i) {
        skiatest::ReporterContext ctx(reporter, SkStringPrintf("emoji %d", i));
        const size_t wantU16 = static_cast<size_t>(i * 2);
        const size_t wantU8 = static_cast<size_t>(i * 4);
        REPORTER_ASSERT(reporter,
                        heapMap.toUtf16Index(wantU8) == wantU16,
                        "got %zu, want %zu",
                        heapMap.toUtf16Index(wantU8),
                        wantU16);
        REPORTER_ASSERT(reporter,
                        heapMap.toUtf8Index(wantU16) == wantU8,
                        "got %zu, want %zu",
                        heapMap.toUtf8Index(wantU16),
                        wantU8);
    }
}
