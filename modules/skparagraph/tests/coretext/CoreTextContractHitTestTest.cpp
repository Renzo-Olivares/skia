/*
 * Copyright 2026 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

// Contract tests for the CoreText pipeline through the public Paragraph API
// (CoreTextContract_<ID>_<Property>) on TextShapingEngine::kCoreText; glyph positions are never
// compared for equality with a native CTLine or with HarfBuzz. Where an open bug breaks part of a
// contract, the test covers the passing part and names the bug

#include "include/core/SkRefCnt.h"
#include "include/core/SkScalar.h"
#include "include/core/SkString.h"
#include "include/core/SkTypes.h"
#include "modules/skparagraph/include/DartTypes.h"
#include "modules/skparagraph/include/FontCollection.h"
#include "modules/skparagraph/include/Paragraph.h"
#include "modules/skparagraph/include/ParagraphStyle.h"
#include "modules/skparagraph/include/TextStyle.h"
#include "modules/skparagraph/src/ParagraphBuilderImpl.h"
#include "modules/skparagraph/tests/coretext/CoreTextTestUtils.h"
#include "src/utils/mac/SkUniqueCFRef.h"
#include "tests/Test.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreText/CoreText.h>

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

using namespace skia::textlayout;
using namespace skia::textlayout::coretext_test;

namespace {
// Sum of box widths; a range split across glyphs or lines may come back as several boxes
SkScalar boxes_width(const std::vector<TextBox>& boxes) {
    SkScalar width = 0;
    for (const TextBox& box : boxes) {
        width += box.rect.width();
    }
    return width;
}

#if !defined(SK_PARAGRAPH_DISABLE_HARFBUZZ)
// Same paragraph as layout_text on TextShapingEngine::kDefault, the engine whose hit-testing
// the Flutter framework is written against
std::unique_ptr<Paragraph> default_engine_layout(const sk_sp<FontCollection>& fontCollection,
                                                 ParagraphStyle paragraphStyle,
                                                 const TextStyle& style,
                                                 std::string_view text,
                                                 SkScalar width) {
    paragraphStyle.setTextShapingEngine(TextShapingEngine::kDefault);
    ParagraphBuilderImpl builder(paragraphStyle, fontCollection, contract_unicode());
    builder.pushStyle(style);
    builder.addText(text.data(), text.size());
    std::unique_ptr<Paragraph> paragraph = builder.Build();
    paragraph->layout(width);
    return paragraph;
}
#endif
}  // namespace

// HT1: each grapheme of a ligature or of a base plus mark gets a non-empty box in every box
// style, "fi" splits its width between the letters, and no hit lands inside a grapheme
DEF_TEST(CoreTextContract_HT1_GraphemeBoxesAndCarets, reporter) {
    if (!has_font_families({"Helvetica"})) {
        return;
    }
    struct Case {
        const char* text;
        std::vector<std::pair<unsigned, unsigned>> graphemes;
    };
    const Case kCases[] = {{"fi", {{0, 1}, {1, 2}}}, {"ab\u0301c", {{0, 1}, {1, 3}, {3, 4}}}};
    const RectHeightStyle kHeightStyles[] = {RectHeightStyle::kTight,
                                             RectHeightStyle::kMax,
                                             RectHeightStyle::kIncludeLineSpacingMiddle,
                                             RectHeightStyle::kIncludeLineSpacingTop,
                                             RectHeightStyle::kIncludeLineSpacingBottom,
                                             RectHeightStyle::kStrut};
    for (const Case& c : kCases) {
        auto paragraph = layout_text(reporter,
                                     system_collection(),
                                     contract_paragraph_style(),
                                     contract_text_style("Helvetica", 40.0f),
                                     c.text,
                                     1000.0f);
        for (RectHeightStyle heightStyle : kHeightStyles) {
            for (RectWidthStyle widthStyle : {RectWidthStyle::kTight, RectWidthStyle::kMax}) {
                for (const auto& [start, end] : c.graphemes) {
                    skiatest::ReporterContext ctx(reporter,
                                                  SkStringPrintf("%s [%u,%u) height %d width %d",
                                                                 c.text,
                                                                 start,
                                                                 end,
                                                                 static_cast<int>(heightStyle),
                                                                 static_cast<int>(widthStyle)));
                    auto boxes = paragraph->getRectsForRange(start, end, heightStyle, widthStyle);
                    REPORTER_ASSERT(reporter, !boxes.empty());
                    for (const TextBox& box : boxes) {
                        REPORTER_ASSERT(reporter,
                                        box.rect.width() > 0 && box.rect.height() > 0,
                                        "got %g x %g",
                                        box.rect.width(),
                                        box.rect.height());
                    }
                }
            }
        }
    }

    auto ligature = layout_text(reporter,
                                system_collection(),
                                contract_paragraph_style(),
                                contract_text_style("Helvetica", 40.0f),
                                "fi",
                                1000.0f);
    // The split only means something if "fi" is one glyph
    const int glyphs = static_cast<int>(drawn_glyphs(ligature.get()).size());
    REPORTER_ASSERT(reporter, glyphs == 1, "got %d", glyphs);
    const SkScalar f = boxes_width(tight_boxes(ligature.get(), 0, 1));
    const SkScalar i = boxes_width(tight_boxes(ligature.get(), 1, 2));
    const SkScalar fi = boxes_width(tight_boxes(ligature.get(), 0, 2));
    REPORTER_ASSERT(reporter,
                    f > 0 && i > 0 && SkScalarNearlyEqual(f + i, fi, 0.01f),
                    "f %g + i %g, fi %g",
                    f,
                    i,
                    fi);

    auto marked = layout_text(reporter,
                              system_collection(),
                              contract_paragraph_style(),
                              contract_text_style("Helvetica", 40.0f),
                              "ab\u0301c",
                              1000.0f);
    for (SkScalar x = 0; x < marked->getMaxIntrinsicWidth() + 2; x += 0.25f) {
        const int position = marked->getGlyphPositionAtCoordinate(x, 20).position;
        REPORTER_ASSERT(reporter, position != 2, "x %g lands inside b + U+0301", x);
    }
}

// HT2: carets fall on the UTF-16 index CTLineGetStringIndexForPosition gives on the same line,
// across pair kerning and LTR/RTL run boundaries in both paragraph directions
DEF_TEST(CoreTextContract_HT2_CaretIndexMatchesCTLine, reporter) {
    if (!has_font_families({"Arial"})) {
        return;
    }
    // Arial kerns AV and To and covers Hebrew, so neither line needs a fallback font
    const char* kText = "AVA To \u05D0\u05D1\u05D2 123";
    for (TextDirection direction : {TextDirection::kLtr, TextDirection::kRtl}) {
        ParagraphStyle paragraphStyle = contract_paragraph_style();
        paragraphStyle.setTextDirection(direction);
        paragraphStyle.setTextAlign(TextAlign::kLeft);
        auto paragraph = layout_text(reporter,
                                     system_collection(),
                                     paragraphStyle,
                                     contract_text_style("Arial", 20.0f),
                                     kText,
                                     1000.0f);
        SkUniqueCFRef<CTLineRef> line =
                native_line(native_font("Arial", 20).get(), kText, 0, direction);
        const SkScalar width = paragraph->getMaxIntrinsicWidth();
        for (SkScalar x = 0.25f; x < width; x += 0.5f) {
            const int position = paragraph->getGlyphPositionAtCoordinate(x, 10).position;
            const int native = SkToInt(CTLineGetStringIndexForPosition(line.get(), {x, 0}));
            REPORTER_ASSERT(reporter,
                            position == native,
                            "direction %d x %g: got %d, CTLine %d",
                            static_cast<int>(direction),
                            x,
                            position,
                            native);
        }
    }
}

#if !defined(SK_PARAGRAPH_DISABLE_HARFBUZZ)
// HT2: a tap left or right of a soft-wrapped line's glyphs gives the same caret and affinity
// as on kDefault (the nearest glyph edge), not CTLine's logical line start or end: Flutter's
// caret movement relies on it (user decision Nov 14 2026: Flutter API over native CTLine).
// Lines ending in '\n' keep HT3's line end upstream instead (user decision Nov 14 2026)
DEF_TEST(CoreTextContract_HT2_TapPastLineEdgeLikeDefaultEngine, reporter) {
    if (!has_font_families({"Arial"})) {
        return;
    }
    const char* kText = "AVA To \u05D0\u05D1\u05D2 123 \u05D0\u05D1 AV 12";
    const TextStyle style = contract_text_style("Arial", 20.0f);
    for (TextDirection direction : {TextDirection::kLtr, TextDirection::kRtl}) {
        ParagraphStyle paragraphStyle = contract_paragraph_style();
        paragraphStyle.setTextDirection(direction);
        paragraphStyle.setTextAlign(TextAlign::kCenter);
        auto paragraph =
                layout_text(reporter, system_collection(), paragraphStyle, style, kText, 180.0f);
        auto expected =
                default_engine_layout(system_collection(), paragraphStyle, style, kText, 180.0f);
        std::vector<LineMetrics> lines;
        paragraph->getLineMetrics(lines);
        REPORTER_ASSERT(reporter, lines.size() == 2, "got %zu lines", lines.size());
        for (const LineMetrics& line : lines) {
            const SkScalar y = static_cast<SkScalar>(line.fBaseline);
            const SkScalar left = static_cast<SkScalar>(line.fLeft);
            const SkScalar right = left + static_cast<SkScalar>(line.fWidth);
            for (SkScalar x : {-10.0f, left - 0.25f, right + 0.25f, 179.0f, 300.0f}) {
                const PositionWithAffinity got = paragraph->getGlyphPositionAtCoordinate(x, y);
                const PositionWithAffinity want = expected->getGlyphPositionAtCoordinate(x, y);
                skiatest::ReporterContext context(reporter,
                                                  SkStringPrintf("direction %d line %zu x %g",
                                                                 static_cast<int>(direction),
                                                                 line.fLineNumber,
                                                                 x));
                REPORTER_ASSERT(reporter,
                                got.position == want.position && got.affinity == want.affinity,
                                "got %d/%d, kDefault %d/%d",
                                got.position,
                                static_cast<int>(got.affinity),
                                want.position,
                                static_cast<int>(want.affinity));
            }
        }
    }
}
#endif  // !defined(SK_PARAGRAPH_DISABLE_HARFBUZZ)

// HT3: a tap right of the last glyph before '\n' gives the line end upstream and a tap on an
// empty line its start downstream, as Flutter caret movement needs; Ahem, as on HarfBuzz
DEF_TEST(CoreTextContract_HT3_HardBreakHitAffinity, reporter) {
    sk_sp<FontCollection> fontCollection = asset_collection({{"fonts/ahem.ttf", "Ahem"}});
    if (!fontCollection) {
        return;
    }
    constexpr std::array<SkScalar, 12> kXs = {0, 6, 7, 13, 14, 15, 27, 28, 29, 500, 986, 1000};
    // Lines "aa", "a", "aa" of 14px squares; in an RTL paragraph the LTR runs sit at the right.
    // Affinities: 'u' upstream, 'd' downstream, one per x
    const auto kRows =
            std::to_array<std::tuple<TextDirection, SkScalar, std::array<int, 12>, const char*>>({
                    {TextDirection::kLtr, 7, {0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2}, "dduuddUUUUUU"},
                    {TextDirection::kLtr, 21, {3, 3, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4}, "dduuuuuuuuuu"},
                    {TextDirection::kLtr, 35, {5, 5, 6, 6, 6, 6, 7, 7, 7, 7, 7, 7}, "dduuddUUUUUU"},
                    {TextDirection::kRtl, 7, {3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 1, 2}, "uuuuuuuuuudu"},
                    {TextDirection::kRtl, 21, {5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 4}, "uuuuuuuuuuuu"},
                    {TextDirection::kRtl, 35, {5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 6, 7}, "dddddddddddu"},
            });
    for (const auto& [direction, y, positions, affinities] : kRows) {
        ParagraphStyle paragraphStyle = contract_paragraph_style();
        paragraphStyle.setTextDirection(direction);
        auto paragraph = layout_text(reporter,
                                     fontCollection,
                                     paragraphStyle,
                                     contract_text_style("Ahem", 14.0f),
                                     "aa\na\naa",
                                     1000.0f);
        REPORTER_ASSERT(reporter, paragraph->lineNumber() == 3, "got %zu", paragraph->lineNumber());
        for (size_t i = 0; i < kXs.size(); ++i) {
            const PositionWithAffinity hit = paragraph->getGlyphPositionAtCoordinate(kXs[i], y);
            const Affinity affinity =
                    (affinities[i] == 'u' || affinities[i] == 'U') ? kUpstream : kDownstream;
            REPORTER_ASSERT(reporter,
                            hit.position == positions[i] && hit.affinity == affinity,
                            "direction %d (%g, %g): got %d/%d, want %d/%d",
                            static_cast<int>(direction),
                            kXs[i],
                            y,
                            hit.position,
                            hit.affinity,
                            positions[i],
                            affinity);
        }
    }

    auto emptyLine = layout_text(reporter,
                                 fontCollection,
                                 contract_paragraph_style(),
                                 contract_text_style("Ahem", 14.0f),
                                 "ab\n\ncd",
                                 1000.0f);
    const PositionWithAffinity lineEnd = emptyLine->getGlyphPositionAtCoordinate(28, 7);
    REPORTER_ASSERT(reporter,
                    lineEnd.position == 2 && lineEnd.affinity == kUpstream,
                    "got %d/%d",
                    lineEnd.position,
                    lineEnd.affinity);
    for (SkScalar x : {0.0f, 28.0f, 100.0f}) {
        const PositionWithAffinity empty = emptyLine->getGlyphPositionAtCoordinate(x, 21);
        REPORTER_ASSERT(reporter,
                        empty.position == 3 && empty.affinity == kDownstream,
                        "x %g: got %d/%d",
                        x,
                        empty.position,
                        empty.affinity);
    }
}

// HT4: boxes of a mixed line come back in visual order and a space between two RTL words is RTL,
// so Flutter paints one continuous selection
DEF_TEST(CoreTextContract_HT4_BidiBoxesVisualOrder, reporter) {
    if (!has_font_families({"Arial"})) {
        return;
    }
    // "abc " [0,4), first Hebrew word [4,7), space 7, second Hebrew word [8,11), " def" [11,15)
    auto paragraph = layout_text(reporter,
                                 system_collection(),
                                 contract_paragraph_style(),
                                 contract_text_style("Arial", 20.0f),
                                 "abc \u05D0\u05D1\u05D2 \u05D3\u05D4\u05D5 def",
                                 1000.0f);
    auto boxes =
            paragraph->getRectsForRange(0, 15, RectHeightStyle::kTight, RectWidthStyle::kTight);
    REPORTER_ASSERT(reporter, boxes.size() >= 3, "got %zu", boxes.size());
    for (size_t i = 1; i < boxes.size(); ++i) {
        REPORTER_ASSERT(reporter,
                        SkScalarNearlyEqual(boxes[i].rect.fLeft, boxes[i - 1].rect.fRight, 0.01f),
                        "box %zu starts at %g, box %zu ends at %g",
                        i,
                        boxes[i].rect.fLeft,
                        i - 1,
                        boxes[i - 1].rect.fRight);
    }
    const std::optional<TextBox> abc = single_tight_box(reporter, paragraph.get(), 0, 3);
    const std::optional<TextBox> firstHebrew = single_tight_box(reporter, paragraph.get(), 4, 7);
    const std::optional<TextBox> space = single_tight_box(reporter, paragraph.get(), 7, 8);
    const std::optional<TextBox> secondHebrew = single_tight_box(reporter, paragraph.get(), 8, 11);
    const std::optional<TextBox> def = single_tight_box(reporter, paragraph.get(), 12, 15);
    if (!abc || !firstHebrew || !space || !secondHebrew || !def) {
        return;
    }
    REPORTER_ASSERT(reporter, space->direction == TextDirection::kRtl);
    REPORTER_ASSERT(reporter, firstHebrew->direction == TextDirection::kRtl);
    REPORTER_ASSERT(reporter,
                    abc->rect.fRight <= secondHebrew->rect.fLeft,
                    "got %g, want <= %g",
                    abc->rect.fRight,
                    secondHebrew->rect.fLeft);
    REPORTER_ASSERT(reporter,
                    secondHebrew->rect.fRight <= space->rect.fLeft,
                    "got %g, want <= %g",
                    secondHebrew->rect.fRight,
                    space->rect.fLeft);
    REPORTER_ASSERT(reporter,
                    space->rect.fRight <= firstHebrew->rect.fLeft,
                    "got %g, want <= %g",
                    space->rect.fRight,
                    firstHebrew->rect.fLeft);
    REPORTER_ASSERT(reporter,
                    firstHebrew->rect.fRight <= def->rect.fLeft,
                    "got %g, want <= %g",
                    firstHebrew->rect.fRight,
                    def->rect.fLeft);
}

// HT5: spaces at the end of a right-aligned RTL line hang past its start and do not move its
// letters, so typing a space does not shift the text
DEF_TEST(CoreTextContract_HT5_RtlTrailingSpacesNoShift, reporter) {
    if (!has_font_families({"Arial"})) {
        return;
    }
    ParagraphStyle paragraphStyle = contract_paragraph_style();
    paragraphStyle.setTextDirection(TextDirection::kRtl);
    paragraphStyle.setTextAlign(TextAlign::kRight);
    const TextStyle arial = contract_text_style("Arial", 20.0f);
    auto layout = [&](const char* text) {
        return layout_text(reporter, system_collection(), paragraphStyle, arial, text, 300.0f);
    };
    auto plain = layout("\u05D0\u05D1\u05D2");
    auto spaced = layout("\u05D0\u05D1\u05D2   ");
    for (unsigned i = 0; i < 3; ++i) {
        skiatest::ReporterContext ctx(reporter, SkStringPrintf("letter %u", i));
        auto plainBoxes =
                plain->getRectsForRange(i, i + 1, RectHeightStyle::kTight, RectWidthStyle::kTight);
        auto spacedBoxes =
                spaced->getRectsForRange(i, i + 1, RectHeightStyle::kTight, RectWidthStyle::kTight);
        REPORTER_ASSERT(reporter,
                        plainBoxes.size() == 1 && spacedBoxes.size() == 1,
                        "got %zu and %zu boxes",
                        plainBoxes.size(),
                        spacedBoxes.size());
        if (plainBoxes.size() != 1 || spacedBoxes.size() != 1) {
            continue;
        }
        const SkRect& a = plainBoxes[0].rect;
        const SkRect& b = spacedBoxes[0].rect;
        REPORTER_ASSERT(reporter,
                        SkScalarNearlyEqual(a.fLeft, b.fLeft, 0.01f) &&
                                SkScalarNearlyEqual(a.fRight, b.fRight, 0.01f),
                        "got [%g, %g], want [%g, %g]",
                        b.fLeft,
                        b.fRight,
                        a.fLeft,
                        a.fRight);
    }
}

#if defined(SK_UNICODE_APPLE_IMPLEMENTATION)
// HT6: getWordBoundary gives the CFStringTokenizer word around each offset for every script,
// including punctuation runs and CJK, the one rule Apple text views share (D19)
DEF_TEST(CoreTextContract_HT6_WordBoundaryLikeCFStringTokenizer, reporter) {
    sk_sp<FontCollection> fontCollection = asset_collection({{"fonts/ahem.ttf", "Ahem"}});
    if (!fontCollection) {
        return;
    }
    const char* kFamily = "\U0001F468\u200D\U0001F469\u200D\U0001F466";
    const std::string twoFamilies = std::string(kFamily) + kFamily;
    for (const char* text : {"!?",
                             "A\u05D0",
                             "\u4F60\u597D\u4E16\u754C\u6211\u4EEC",
                             twoFamilies.c_str(),
                             "Hello, world! \u05E9\u05DC\u05D5\u05DD"}) {
        auto paragraph = layout_text(reporter,
                                     fontCollection,
                                     contract_paragraph_style(),
                                     contract_text_style("Ahem", 14.0f),
                                     text,
                                     1000.0f);
        SkUniqueCFRef<CFStringRef> string(
                CFStringCreateWithCString(nullptr, text, kCFStringEncodingUTF8));
        const CFIndex length = CFStringGetLength(string.get());
        SkUniqueCFRef<CFStringTokenizerRef> tokenizer(
                CFStringTokenizerCreate(nullptr,
                                        string.get(),
                                        CFRangeMake(0, length),
                                        kCFStringTokenizerUnitWordBoundary,
                                        nullptr));
        for (CFIndex offset = 0; offset < length; ++offset) {
            CFStringTokenizerGoToTokenAtIndex(tokenizer.get(), offset);
            const CFRange token = CFStringTokenizerGetCurrentTokenRange(tokenizer.get());
            const SkRange<size_t> word = paragraph->getWordBoundary(offset);
            REPORTER_ASSERT(reporter,
                            SkToS64(word.start) == token.location &&
                                    SkToS64(word.end) == token.location + token.length,
                            "%s offset %ld: got [%zu, %zu), tokenizer [%ld, %ld)",
                            text,
                            offset,
                            word.start,
                            word.end,
                            token.location,
                            token.location + token.length);
        }
    }

    // Double click in NSTextView picks "!" alone, the tokenizer (and Flutter) the whole run
    const auto kPinned = std::to_array<std::tuple<const char*, unsigned, size_t, size_t>>(
            {{"!?", 1, 0, 2},
             {"A\u05D0", 1, 0, 2},
             {twoFamilies.c_str(), 3, 0, 8},
             {twoFamilies.c_str(), 8, 8, 16}});
    for (const auto& [text, offset, start, end] : kPinned) {
        auto paragraph = layout_text(reporter,
                                     fontCollection,
                                     contract_paragraph_style(),
                                     contract_text_style("Ahem", 14.0f),
                                     text,
                                     1000.0f);
        const SkRange<size_t> word = paragraph->getWordBoundary(offset);
        REPORTER_ASSERT(reporter,
                        word.start == start && word.end == end,
                        "%s offset %u: got [%zu, %zu), want [%zu, %zu)",
                        text,
                        offset,
                        word.start,
                        word.end,
                        start,
                        end);
    }
}
#endif

// HT7: line queries agree with LineMetrics, selection boxes tile the lines without vertical
// gaps, and hit tests answer outside the paragraph and on an empty one
DEF_TEST(CoreTextContract_HT7_LineQueries, reporter) {
    sk_sp<FontCollection> fontCollection = asset_collection({{"fonts/ahem.ttf", "Ahem"}});
    if (!fontCollection) {
        return;
    }
    // Lines "aaa " (soft break), "aaa\n", "bb" of 14px squares
    auto paragraph = layout_text(reporter,
                                 fontCollection,
                                 contract_paragraph_style(),
                                 contract_text_style("Ahem", 14.0f),
                                 "aaa aaa\nbb",
                                 60.0f);
    std::vector<LineMetrics> metrics;
    paragraph->getLineMetrics(metrics);
    REPORTER_ASSERT(reporter, metrics.size() == 3, "got %zu", metrics.size());
    for (size_t offset = 0; offset < 10; ++offset) {
        const int line = paragraph->getLineNumberAtUTF16Offset(offset);
        REPORTER_ASSERT(reporter,
                        line >= 0 && line < SkToInt(metrics.size()) &&
                                metrics[line].fStartIndex <= offset &&
                                offset < metrics[line].fEndIncludingNewline,
                        "offset %zu: line %d",
                        offset,
                        line);
    }

    auto boxes = paragraph->getRectsForRange(0, 10, RectHeightStyle::kMax, RectWidthStyle::kTight);
    std::array<bool, 3> lineCovered = {};
    for (const TextBox& box : boxes) {
        const int line = SkScalarRoundToInt(box.rect.fTop / 14);
        REPORTER_ASSERT(reporter,
                        line >= 0 && line < 3 && box.rect.fTop == line * 14.0f &&
                                box.rect.fBottom == (line + 1) * 14.0f,
                        "box [%g, %g]",
                        box.rect.fTop,
                        box.rect.fBottom);
        if (line >= 0 && line < 3) {
            lineCovered[line] = true;
            REPORTER_ASSERT(
                    reporter, box.rect.fLeft == 0, "line %d starts at %g", line, box.rect.fLeft);
        }
    }
    REPORTER_ASSERT(reporter, lineCovered[0] && lineCovered[1] && lineCovered[2]);

    Paragraph::GlyphInfo info;
    REPORTER_ASSERT(reporter,
                    paragraph->getGlyphInfoAtUTF16Offset(5, &info) &&
                            info.fGraphemeClusterTextRange == skia::textlayout::TextRange(5, 6) &&
                            info.fGraphemeLayoutBounds.fTop == 14,
                    "got [%zu, %zu) at top %g",
                    info.fGraphemeClusterTextRange.start,
                    info.fGraphemeClusterTextRange.end,
                    info.fGraphemeLayoutBounds.fTop);
    REPORTER_ASSERT(reporter,
                    paragraph->getClosestUTF16GlyphInfoAt(20, 20, &info) &&
                            info.fGraphemeClusterTextRange == skia::textlayout::TextRange(5, 6),
                    "got [%zu, %zu)",
                    info.fGraphemeClusterTextRange.start,
                    info.fGraphemeClusterTextRange.end);

    const int before = paragraph->getGlyphPositionAtCoordinate(-10, -10).position;
    const int after = paragraph->getGlyphPositionAtCoordinate(1e4f, 1e4f).position;
    REPORTER_ASSERT(reporter, before == 0, "got %d", before);
    REPORTER_ASSERT(reporter, after == 10, "got %d", after);

    auto empty = layout_text(reporter,
                             fontCollection,
                             contract_paragraph_style(),
                             contract_text_style("Ahem", 14.0f),
                             "",
                             60.0f);
    for (SkPoint point : {SkPoint{0, 0}, SkPoint{100, 100}, SkPoint{-10, -10}}) {
        const int position = empty->getGlyphPositionAtCoordinate(point.fX, point.fY).position;
        REPORTER_ASSERT(reporter, position == 0, "(%g, %g): got %d", point.fX, point.fY, position);
    }
}

// HT8: every offset in the text has a non-empty cluster containing it and the text length none,
// or TextPainter asserts; strut caret boxes have height on spaces and zero-width characters
DEF_TEST(CoreTextContract_HT8_GlyphInfoEveryOffset, reporter) {
    if (!has_font_families({"Helvetica", "Apple Color Emoji"})) {
        return;
    }
    ParagraphStyle paragraphStyle = contract_paragraph_style();
    StrutStyle strut;
    strut.setFontFamilies({SkString("Helvetica")});
    strut.setFontSize(20.0f);
    strut.setStrutEnabled(true);
    paragraphStyle.setStrutStyle(strut);
    std::unique_ptr<ParagraphBuilderImpl> builder =
            contract_builder(system_collection(), paragraphStyle);
    builder->pushStyle(contract_text_style("Helvetica", 20.0f));
    // a, U+1F600 [1,3), family [3,11), space 11, ZWSP 12, b 13, LRM 14, placeholder 15, c 16
    builder->addText("a\U0001F600\U0001F468\u200D\U0001F469\u200D\U0001F466 \u200Bb\u200E");
    builder->addPlaceholder(PlaceholderStyle(
            10.0f, 10.0f, PlaceholderAlignment::kBaseline, TextBaseline::kAlphabetic, 10.0f));
    builder->addText("c");
    auto paragraph = layout_built(reporter, builder.get(), 1000.0f);
    constexpr size_t kLength = 17;
    Paragraph::GlyphInfo info;
    for (size_t offset = 0; offset < kLength; ++offset) {
        const bool found = paragraph->getGlyphInfoAtUTF16Offset(offset, &info);
        const skia::textlayout::TextRange range = info.fGraphemeClusterTextRange;
        REPORTER_ASSERT(reporter,
                        found && range.start <= offset && offset < range.end,
                        "offset %zu: found %d, [%zu, %zu)",
                        offset,
                        found,
                        found ? range.start : 0,
                        found ? range.end : 0);
    }
    REPORTER_ASSERT(reporter, !paragraph->getGlyphInfoAtUTF16Offset(kLength, &info));

    for (unsigned offset : {11u, 12u, 14u}) {
        auto boxes = paragraph->getRectsForRange(
                offset, offset + 1, RectHeightStyle::kStrut, RectWidthStyle::kTight);
        REPORTER_ASSERT(reporter,
                        boxes.size() == 1 && boxes[0].rect.height() > 0,
                        "offset %u: %zu boxes, height %g",
                        offset,
                        boxes.size(),
                        boxes.empty() ? 0 : boxes[0].rect.height());
    }
}

// HT8: on a soft break and on an LTR/RTL run boundary the same offset sits at two places, and the
// hit affinity tells them apart, so the caret stays where the user tapped
DEF_TEST(CoreTextContract_HT8_AffinityAtLineAndBidiEdges, reporter) {
    sk_sp<FontCollection> fontCollection = asset_collection({{"fonts/ahem.ttf", "Ahem"}});
    if (!fontCollection) {
        return;
    }
    // Lines "aaa " and "bbb" of 14px squares
    auto paragraph = layout_text(reporter,
                                 fontCollection,
                                 contract_paragraph_style(),
                                 contract_text_style("Ahem", 14.0f),
                                 "aaa bbb",
                                 50.0f);
    const PositionWithAffinity lineEnd = paragraph->getGlyphPositionAtCoordinate(49, 7);
    const PositionWithAffinity lineStart = paragraph->getGlyphPositionAtCoordinate(0, 21);
    REPORTER_ASSERT(reporter,
                    lineEnd.position == 4 && lineEnd.affinity == kUpstream,
                    "got %d/%d",
                    lineEnd.position,
                    lineEnd.affinity);
    REPORTER_ASSERT(reporter,
                    lineStart.position == 4 && lineStart.affinity == kDownstream,
                    "got %d/%d",
                    lineStart.position,
                    lineStart.affinity);

    if (!has_font_families({"Arial"})) {
        return;
    }
    // Offset 4 ends "abc " at the left of the Hebrew run and starts the Hebrew word at its right
    auto bidi = layout_text(reporter,
                            system_collection(),
                            contract_paragraph_style(),
                            contract_text_style("Arial", 20.0f),
                            "abc \u05D0\u05D1\u05D2",
                            1000.0f);
    const std::optional<TextBox> space = single_tight_box(reporter, bidi.get(), 3, 4);
    const std::optional<TextBox> alef = single_tight_box(reporter, bidi.get(), 4, 5);
    if (!space || !alef) {
        return;
    }
    const PositionWithAffinity afterSpace =
            bidi->getGlyphPositionAtCoordinate(space->rect.fRight - 0.5f, 10);
    const PositionWithAffinity beforeAlef =
            bidi->getGlyphPositionAtCoordinate(alef->rect.fRight - 0.5f, 10);
    REPORTER_ASSERT(reporter,
                    afterSpace.position == 4 && afterSpace.affinity == kUpstream,
                    "got %d/%d",
                    afterSpace.position,
                    afterSpace.affinity);
    REPORTER_ASSERT(reporter,
                    beforeAlef.position == 4 && beforeAlef.affinity == kDownstream,
                    "got %d/%d",
                    beforeAlef.position,
                    beforeAlef.affinity);
}

// HT9: every index is a UTF-16 index: a surrogate pair or a ZWJ sequence is one box and one caret
// stop, an offset inside it maps to the whole cluster, and U+FFFC boxes match the placeholder
DEF_TEST(CoreTextContract_HT9_Utf16Indices, reporter) {
    if (!has_font_families({"Helvetica", "Apple Color Emoji"})) {
        return;
    }
    std::unique_ptr<ParagraphBuilderImpl> builder =
            contract_builder(system_collection(), contract_paragraph_style());
    builder->pushStyle(contract_text_style("Helvetica", 20.0f));
    // a, U+1F600 [1,3), b, family [4,12), placeholder 12
    builder->addText("a\U0001F600b\U0001F468\u200D\U0001F469\u200D\U0001F466");
    builder->addPlaceholder(PlaceholderStyle(
            20.0f, 20.0f, PlaceholderAlignment::kBaseline, TextBaseline::kAlphabetic, 20.0f));
    auto paragraph = layout_built(reporter, builder.get(), 1000.0f);

    const auto emoji = tight_boxes(paragraph.get(), 1, 3);
    REPORTER_ASSERT(reporter,
                    emoji.size() == 1 && emoji[0].rect.width() > 0,
                    "got %zu boxes",
                    emoji.size());
    REPORTER_ASSERT(reporter,
                    tight_boxes(paragraph.get(), 1, 2).empty(),
                    "got %zu boxes",
                    tight_boxes(paragraph.get(), 1, 2).size());
    REPORTER_ASSERT(reporter,
                    tight_boxes(paragraph.get(), 4, 7).empty(),
                    "got %zu boxes",
                    tight_boxes(paragraph.get(), 4, 7).size());

    for (SkScalar x = 0; x < paragraph->getMaxIntrinsicWidth() + 2; x += 0.25f) {
        const int position = paragraph->getGlyphPositionAtCoordinate(x, 10).position;
        REPORTER_ASSERT(reporter,
                        position != 2 && (position <= 4 || position >= 12),
                        "x %g: got %d",
                        x,
                        position);
    }

    Paragraph::GlyphInfo info;
    for (auto [offset, start, end] : {std::array<size_t, 3>{2, 1, 3}, {7, 4, 12}}) {
        REPORTER_ASSERT(
                reporter,
                paragraph->getGlyphInfoAtUTF16Offset(offset, &info) &&
                        info.fGraphemeClusterTextRange == skia::textlayout::TextRange(start, end),
                "offset %zu: got [%zu, %zu), want [%zu, %zu)",
                offset,
                info.fGraphemeClusterTextRange.start,
                info.fGraphemeClusterTextRange.end,
                start,
                end);
    }

    const auto placeholders = paragraph->getRectsForPlaceholders();
    const auto replacement = tight_boxes(paragraph.get(), 12, 13);
    REPORTER_ASSERT(reporter,
                    placeholders.size() == 1 && replacement.size() == 1,
                    "got %zu placeholders, %zu boxes",
                    placeholders.size(),
                    replacement.size());
    if (placeholders.size() == 1 && replacement.size() == 1) {
        REPORTER_ASSERT(reporter, placeholders[0].rect == replacement[0].rect);
    }
}

// HT10: getWordBoundary always contains its offset, gives an empty range past the end, and keeps
// U+FFFC a word of its own; otherwise RenderEditable loops and SelectionArea misses WidgetSpans
DEF_TEST(CoreTextContract_HT10_WordBoundaryInvariants, reporter) {
    sk_sp<FontCollection> fontCollection = asset_collection({{"fonts/ahem.ttf", "Ahem"}});
    if (!fontCollection) {
        return;
    }
    std::unique_ptr<ParagraphBuilderImpl> builder =
            contract_builder(fontCollection, contract_paragraph_style());
    builder->pushStyle(contract_text_style("Ahem", 14.0f));
    // "ab cd" [0,5), placeholder 5, "ef" [6,8)
    builder->addText("ab cd");
    builder->addPlaceholder(PlaceholderStyle(
            14.0f, 14.0f, PlaceholderAlignment::kBaseline, TextBaseline::kAlphabetic, 14.0f));
    builder->addText("ef");
    auto paragraph = layout_built(reporter, builder.get(), 1000.0f);
    constexpr unsigned kLength = 8;
    for (unsigned offset = 0; offset < kLength; ++offset) {
        const SkRange<size_t> word = paragraph->getWordBoundary(offset);
        REPORTER_ASSERT(reporter,
                        word.start <= offset && offset < word.end,
                        "offset %u: got [%zu, %zu)",
                        offset,
                        word.start,
                        word.end);
    }
    const SkRange<size_t> past = paragraph->getWordBoundary(kLength);
    REPORTER_ASSERT(reporter,
                    past.start == past.end && past.end <= kLength,
                    "got [%zu, %zu)",
                    past.start,
                    past.end);
    const SkRange<size_t> placeholder = paragraph->getWordBoundary(5);
    REPORTER_ASSERT(reporter,
                    placeholder.start == 5 && placeholder.end == 6,
                    "got [%zu, %zu)",
                    placeholder.start,
                    placeholder.end);
}

// HT11: LineMetrics number and tile the lines, points above or below hit the first or last
// line at their dx, a point on a baseline hits that line; zero-descent lines are not covered
DEF_TEST(CoreTextContract_HT11_VerticalHits, reporter) {
    sk_sp<FontCollection> fontCollection = asset_collection({{"fonts/ahem.ttf", "Ahem"}});
    if (!fontCollection) {
        return;
    }
    for (const char* text : {"a\nbb\nccc", "aaaaa\n"}) {
        skiatest::ReporterContext ctx(reporter, SkStringPrintf("\"%s\"", text));
        auto paragraph = layout_text(reporter,
                                     fontCollection,
                                     contract_paragraph_style(),
                                     contract_text_style("Ahem", 14.0f),
                                     text,
                                     1000.0f);
        std::vector<LineMetrics> metrics;
        paragraph->getLineMetrics(metrics);
        REPORTER_ASSERT(reporter, metrics.size() >= 2, "got %zu", metrics.size());
        for (size_t i = 0; i < metrics.size(); ++i) {
            REPORTER_ASSERT(reporter,
                            metrics[i].fLineNumber == i,
                            "line %zu: got %zu",
                            i,
                            metrics[i].fLineNumber);
            // The empty line after a trailing '\n' starts on that '\n', so ranges may overlap
            if (i > 0) {
                REPORTER_ASSERT(
                        reporter,
                        metrics[i - 1].fStartIndex < metrics[i].fStartIndex &&
                                metrics[i].fStartIndex <= metrics[i - 1].fEndIncludingNewline,
                        "line %zu starts at %zu, previous [%zu, %zu)",
                        i,
                        metrics[i].fStartIndex,
                        metrics[i - 1].fStartIndex,
                        metrics[i - 1].fEndIncludingNewline);
            }
            const SkScalar baseline = static_cast<SkScalar>(metrics[i].fBaseline);
            const SkScalar middle =
                    baseline - static_cast<SkScalar>(metrics[i].fAscent - metrics[i].fHeight / 2);
            const int onBaseline = paragraph->getGlyphPositionAtCoordinate(1, baseline).position;
            const int inMiddle = paragraph->getGlyphPositionAtCoordinate(1, middle).position;
            REPORTER_ASSERT(reporter,
                            onBaseline == inMiddle,
                            "line %zu: got %d on the baseline, %d in the middle",
                            i,
                            onBaseline,
                            inMiddle);
        }
    }

    auto paragraph = layout_text(reporter,
                                 fontCollection,
                                 contract_paragraph_style(),
                                 contract_text_style("Ahem", 14.0f),
                                 "a\nbb\nccc",
                                 1000.0f);
    // x = 20 is past "a" on the first line and in the left half of the second "c" on the last
    const PositionWithAffinity above = paragraph->getGlyphPositionAtCoordinate(20, -50);
    const PositionWithAffinity below = paragraph->getGlyphPositionAtCoordinate(20, 1000);
    REPORTER_ASSERT(reporter, above.position == 1, "got %d", above.position);
    REPORTER_ASSERT(reporter, below.position == 6, "got %d", below.position);
}

// HT12: empty ranges have no box, '\n' adds no width and has a zero-width box and GlyphInfo,
// GlyphInfo takes the run's direction, closest-glyph boxes tile a justified line. Omits B6
DEF_TEST(CoreTextContract_HT12_BoxEdgeCases, reporter) {
    if (!has_font_families({"Helvetica"})) {
        return;
    }
    const ParagraphStyle paragraphStyle = contract_paragraph_style();
    const TextStyle helvetica = contract_text_style("Helvetica", 20.0f);
    auto layout = [&](const char* text) {
        return layout_text(reporter, system_collection(), paragraphStyle, helvetica, text, 1000);
    };
    auto newline = layout("ab\n");
    REPORTER_ASSERT(reporter, tight_boxes(newline.get(), 1, 1).empty());
    const auto withNewline = tight_boxes(newline.get(), 0, 3);
    const SkScalar lineBottom = newline->getHeight() / 2;
    REPORTER_ASSERT(reporter, !withNewline.empty());
    for (const TextBox& box : withNewline) {
        REPORTER_ASSERT(reporter,
                        box.rect.fBottom <= lineBottom,
                        "box %g..%g, want bottom <= %g",
                        box.rect.fTop,
                        box.rect.fBottom,
                        lineBottom);
    }
    const SkScalar withWidth = boxes_width(withNewline);
    const SkScalar withoutWidth = boxes_width(tight_boxes(newline.get(), 0, 2));
    REPORTER_ASSERT(reporter,
                    SkScalarNearlyEqual(withWidth, withoutWidth, 0.01f),
                    "got %g, want %g",
                    withWidth,
                    withoutWidth);
    Paragraph::GlyphInfo info;
    REPORTER_ASSERT(
            reporter,
            newline->getGlyphInfoAtUTF16Offset(2, &info) && info.fGraphemeLayoutBounds.width() == 0,
            "got width %g",
            info.fGraphemeLayoutBounds.width());

    auto onlyNewline = layout("\n");
    const auto newlineBoxes = tight_boxes(onlyNewline.get(), 0, 1);
    REPORTER_ASSERT(reporter,
                    newlineBoxes.size() == 1 && newlineBoxes[0].rect.width() == 0,
                    "got %zu boxes, width %g",
                    newlineBoxes.size(),
                    newlineBoxes.empty() ? 0 : newlineBoxes[0].rect.width());

    auto mixed = layout("ab \u05D0\u05D1");
    REPORTER_ASSERT(
            reporter,
            mixed->getGlyphInfoAtUTF16Offset(0, &info) && info.fDirection == TextDirection::kLtr);
    REPORTER_ASSERT(
            reporter,
            mixed->getGlyphInfoAtUTF16Offset(3, &info) && info.fDirection == TextDirection::kRtl);

    sk_sp<FontCollection> ahem = asset_collection({{"fonts/ahem.ttf", "Ahem"}});
    if (!ahem) {
        return;
    }
    ParagraphStyle justified = contract_paragraph_style();
    justified.setTextAlign(TextAlign::kJustify);
    TextStyle spaced = contract_text_style("Ahem", 14.0f);
    spaced.setLetterSpacing(2.0f);
    spaced.setWordSpacing(5.0f);
    // The first line "ab cd ef " is justified to 200; the last line is not
    auto paragraph = layout_text(reporter, ahem, justified, spaced, "ab cd ef gh ij kl mn", 200.0f);
    std::vector<SkRect> bounds;
    for (SkScalar x = 0; x < 200; x += 1) {
        REPORTER_ASSERT(reporter, paragraph->getClosestUTF16GlyphInfoAt(x, 7, &info));
        if (bounds.empty() || bounds.back() != info.fGraphemeLayoutBounds) {
            bounds.push_back(info.fGraphemeLayoutBounds);
        }
    }
    REPORTER_ASSERT(reporter, bounds.size() > 2, "got %zu", bounds.size());
    for (size_t i = 1; i < bounds.size(); ++i) {
        REPORTER_ASSERT(reporter,
                        SkScalarNearlyEqual(bounds[i].fLeft, bounds[i - 1].fRight, 0.01f),
                        "glyph %zu starts at %g, previous ends at %g",
                        i,
                        bounds[i].fLeft,
                        bounds[i - 1].fRight);
    }
}

// HT2: tapping past the right edge of an RTL run split by a soft wrap hits the wrapped line's
// rightmost grapheme start downstream, not the previous line's trailing grapheme
DEF_TEST(CoreTextContract_HT2_RtlWrappedRunRightEdgeHit, reporter) {
    if (!has_font_families({"Arial"})) {
        return;
    }
    const sk_sp<FontCollection> fonts = system_collection();
    const TextStyle style = contract_text_style("Arial", 20.0f);
    ParagraphStyle paragraphStyle = contract_paragraph_style();
    paragraphStyle.setTextDirection(TextDirection::kRtl);

    // UTF-16: [0,3) "אבג", 3 ' ', [4,6) "דָ" (dalet + qamats), 6 'ה', 7 'ו'
    const char* kFirstWord = "\u05D0\u05D1\u05D2";
    const char* kSecondWord = "\u05D3\u05B8\u05D4\u05D5";
    const char* kText = "\u05D0\u05D1\u05D2 \u05D3\u05B8\u05D4\u05D5";
    const SkScalar w1 = layout_text(reporter, fonts, paragraphStyle, style, kFirstWord, 1000.0f)
                                ->getMaxIntrinsicWidth();
    const SkScalar w2 = layout_text(reporter, fonts, paragraphStyle, style, kSecondWord, 1000.0f)
                                ->getMaxIntrinsicWidth();
    const SkScalar wrapWidth = std::max(w1, w2) + 2.0f;

    auto paragraph = layout_text(reporter, fonts, paragraphStyle, style, kText, wrapWidth);
    const std::vector<LineMetrics> lines = line_metrics(paragraph.get());
    REPORTER_ASSERT(reporter, lines.size() == 2, "got %zu lines", lines.size());
    if (lines.size() != 2) {
        return;
    }
    const SkScalar y = static_cast<SkScalar>(lines[1].fBaseline);
    const SkScalar right = static_cast<SkScalar>(lines[1].fLeft + lines[1].fWidth);
    for (SkScalar x : {right + 0.5f, right + 50.0f}) {
        skiatest::ReporterContext context(reporter, SkStringPrintf("x %g", x));
        const PositionWithAffinity got = paragraph->getGlyphPositionAtCoordinate(x, y);
        REPORTER_ASSERT(reporter,
                        got.position == 4 && got.affinity == Affinity::kDownstream,
                        "got %d/%d, want 4/downstream",
                        got.position,
                        static_cast<int>(got.affinity));
    }
}

// HT12: hit-testing a justified wrapped line uses justified cluster widths through the slice's
// last glyph rather than subtracting the next line's unjustified glyph position
DEF_TEST(CoreTextContract_HT12_JustifiedWrappedSliceHitTest, reporter) {
    sk_sp<FontCollection> ahem = asset_collection({{"fonts/ahem.ttf", "Ahem"}});
    if (!ahem) {
        return;
    }
    ParagraphStyle justified = contract_paragraph_style();
    justified.setTextAlign(TextAlign::kJustify);
    const TextStyle ahem14 = contract_text_style("Ahem", 14.0f);
    // Line 1 "ab cd-" (6 * 14 = 84) justifies to 108 (+24 on the space), placing '-' at [94, 108)
    auto ctLtr = layout_text(reporter, ahem, justified, ahem14, "ab cd-ef", 108.0f);
    const PositionWithAffinity leftHalfHyphen = ctLtr->getGlyphPositionAtCoordinate(96.0f, 7.0f);
    const PositionWithAffinity rightHalfHyphen = ctLtr->getGlyphPositionAtCoordinate(105.0f, 7.0f);
    REPORTER_ASSERT(
            reporter,
            leftHalfHyphen.position == 5 && leftHalfHyphen.affinity == Affinity::kDownstream,
            "got %d/%d, want 5/downstream",
            leftHalfHyphen.position,
            static_cast<int>(leftHalfHyphen.affinity));
    REPORTER_ASSERT(
            reporter,
            rightHalfHyphen.position == 6 && rightHalfHyphen.affinity == Affinity::kUpstream,
            "got %d/%d, want 6/upstream",
            rightHalfHyphen.position,
            static_cast<int>(rightHalfHyphen.affinity));

#if !defined(SK_PARAGRAPH_DISABLE_HARFBUZZ)
    ParagraphStyle hbJustified = justified;
    hbJustified.setTextShapingEngine(TextShapingEngine::kDefault);
    ParagraphBuilderImpl hbBuilder(hbJustified, ahem, contract_unicode());
    hbBuilder.pushStyle(ahem14);
    hbBuilder.addText("ab cd-ef");
    auto hbLtr = hbBuilder.Build();
    hbLtr->layout(108.0f);
    for (SkScalar x = 0.0f; x <= 108.0f; x += 2.0f) {
        skiatest::ReporterContext ctx(reporter, SkStringPrintf("ltr x %g", x));
        const PositionWithAffinity got = ctLtr->getGlyphPositionAtCoordinate(x, 7.0f);
        const PositionWithAffinity want = hbLtr->getGlyphPositionAtCoordinate(x, 7.0f);
        REPORTER_ASSERT(reporter,
                        got.position == want.position && got.affinity == want.affinity,
                        "got %d/%d, want %d/%d",
                        got.position,
                        static_cast<int>(got.affinity),
                        want.position,
                        static_cast<int>(want.affinity));
    }
#endif

    if (!has_font_families({"Arial"})) {
        return;
    }
    const sk_sp<FontCollection> sysFonts = system_collection();
    const TextStyle arial20 = contract_text_style("Arial", 20.0f);
    ParagraphStyle rtlJustified = justified;
    rtlJustified.setTextDirection(TextDirection::kRtl);
    const char* kRtlText = "\u05D0\u05D1 \u05D2\u05D3-\u05D4\u05D5";
    const char* kRtlPrefix = "\u05D0\u05D1 \u05D2\u05D3-";
    const SkScalar prefixWidth =
            layout_text(reporter, sysFonts, rtlJustified, arial20, kRtlPrefix, 1000.0f)
                    ->getMaxIntrinsicWidth();
    const SkScalar wrapWidth = prefixWidth + 24.0f;
    auto ctRtl = layout_text(reporter, sysFonts, rtlJustified, arial20, kRtlText, wrapWidth);
    const auto hyphenBoxes = tight_boxes(ctRtl.get(), 5, 6);
    REPORTER_ASSERT(reporter, hyphenBoxes.size() == 1, "got %zu boxes", hyphenBoxes.size());
    if (hyphenBoxes.size() == 1) {
        const SkScalar rightQuarter =
                hyphenBoxes[0].rect.fLeft + hyphenBoxes[0].rect.width() * 0.75f;
        const PositionWithAffinity got = ctRtl->getGlyphPositionAtCoordinate(rightQuarter, 10.0f);
        REPORTER_ASSERT(reporter,
                        got.position == 5 && got.affinity == Affinity::kDownstream,
                        "got %d/%d, want 5/downstream",
                        got.position,
                        static_cast<int>(got.affinity));
    }
}
