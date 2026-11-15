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

#include "include/core/SkFont.h"
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
#include "tests/Test.h"

#include <algorithm>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

using namespace skia::textlayout;
using namespace skia::textlayout::coretext_test;

namespace {
// Every getter a client reads after layout, printed exactly, so two layouts compare as strings
SkString layout_snapshot(Paragraph* paragraph) {
    SkString out =
            SkStringPrintf("h=%.9g longest=%.9g min=%.9g max=%.9g a=%.9g i=%.9g n=%zu e=%d\n",
                           paragraph->getHeight(),
                           paragraph->getLongestLine(),
                           paragraph->getMinIntrinsicWidth(),
                           paragraph->getMaxIntrinsicWidth(),
                           paragraph->getAlphabeticBaseline(),
                           paragraph->getIdeographicBaseline(),
                           paragraph->lineNumber(),
                           paragraph->didExceedMaxLines());
    for (const LineMetrics& line : line_metrics(paragraph)) {
        out.appendf("[%zu,%zu) %.9g %.9g %.9g %.9g %.9g %.9g %d\n",
                    line.fStartIndex,
                    line.fEndIndex,
                    line.fAscent,
                    line.fDescent,
                    line.fHeight,
                    line.fWidth,
                    line.fLeft,
                    line.fBaseline,
                    line.fHardBreak);
    }
    for (const DrawnGlyph& glyph : drawn_glyphs(paragraph)) {
        out.appendf("%d:%u@%.9g,%.9g ",
                    glyph.lineNumber,
                    glyph.glyph,
                    glyph.position.fX,
                    glyph.position.fY);
    }
    for (const TextBox& box :
         paragraph->getRectsForRange(0, 1000, RectHeightStyle::kTight, RectWidthStyle::kTight)) {
        out.appendf("\nbox %.9g %.9g %.9g %.9g",
                    box.rect.fLeft,
                    box.rect.fTop,
                    box.rect.fRight,
                    box.rect.fBottom);
    }
    const PositionWithAffinity hit = paragraph->getGlyphPositionAtCoordinate(50, 10);
    out.appendf("\nhit %d %d", hit.position, hit.affinity);
    return out;
}

// Drawn glyph ids without spaces, which a line break drops from the end of the line
std::vector<SkGlyphID> non_space_glyphs(Paragraph* paragraph) {
    std::vector<SkGlyphID> glyphs;
    for (const DrawnGlyph& glyph : drawn_glyphs(paragraph)) {
        if (glyph.glyph != glyph.font.unicharToGlyph(' ')) {
            glyphs.push_back(glyph.glyph);
        }
    }
    return glyphs;
}
}  // namespace

// LB1: Flutter lays text out again at its own maxIntrinsicWidth (shrink-wrap); fractional advances
// must not push the last word to a new line (Flutter #21965)
DEF_TEST(CoreTextContract_LB1_MaxIntrinsicKeepsLines, reporter) {
    sk_sp<FontCollection> fontCollection =
            asset_collection({{"fonts/Roboto-Regular.ttf", "Roboto"}});
    if (!fontCollection) {
        return;
    }
    auto paragraph = layout_text(reporter,
                                 fontCollection,
                                 contract_paragraph_style(),
                                 contract_text_style("Roboto", 17.0f),
                                 "Wrap me at my own width\nand keep two lines",
                                 SK_ScalarInfinity);
    const std::vector<LineMetrics> unbounded = line_metrics(paragraph.get());
    paragraph->layout(paragraph->getMaxIntrinsicWidth());
    const std::vector<LineMetrics> shrunk = line_metrics(paragraph.get());
    REPORTER_ASSERT(reporter, shrunk.size() == 2, "got %zu lines", shrunk.size());
    for (size_t i = 0; i < std::min(unbounded.size(), shrunk.size()); ++i) {
        skiatest::ReporterContext ctx(reporter, SkStringPrintf("line %zu", i));
        REPORTER_ASSERT(reporter,
                        shrunk[i].fEndIndex == unbounded[i].fEndIndex,
                        "got %zu, want %zu",
                        shrunk[i].fEndIndex,
                        unbounded[i].fEndIndex);
    }
}

// LB2: a grapheme cluster wider than what is left after a break opportunity moves to the next
// line whole; a ZWJ family emoji is one cluster of seven code points
DEF_TEST(CoreTextContract_LB2_WideClusterWraps, reporter) {
    if (!has_font_families({"Helvetica", "Apple Color Emoji"})) {
        return;
    }
    const TextStyle style = contract_text_style("Helvetica", 20.0f);
    const char* family =
            "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7"
            "\xE2\x80\x8D\xF0\x9F\x91\xA6";
    auto emoji = layout_text(reporter,
                             system_collection(),
                             contract_paragraph_style(),
                             style,
                             family,
                             SK_ScalarInfinity);
    auto prefix = layout_text(
            reporter, system_collection(), contract_paragraph_style(), style, "a ", 1000);
    const SkScalar width = prefix->getMaxIntrinsicWidth() + emoji->getMaxIntrinsicWidth() / 2;
    auto paragraph = layout_text(reporter,
                                 system_collection(),
                                 contract_paragraph_style(),
                                 style,
                                 std::string("a ") + family,
                                 width);
    const std::vector<LineMetrics> lines = line_metrics(paragraph.get());
    REPORTER_ASSERT(reporter, lines.size() == 2, "got %zu lines", lines.size());
    if (lines.size() != 2) {
        return;
    }
    REPORTER_ASSERT(reporter, lines[1].fStartIndex == 2, "got %zu", lines[1].fStartIndex);
    int emojiGlyphs = 0;
    for (const DrawnGlyph& glyph : drawn_glyphs(paragraph.get())) {
        emojiGlyphs += glyph.lineNumber == 1;
    }
    REPORTER_ASSERT(reporter, emojiGlyphs == 1, "got %d", emojiGlyphs);
}

// LB3: '\n' ends its line with no width; an empty line takes the metrics of its '\n' or the
// forced strut. Omits B4 (style after a trailing '\n') and B8 / Flutter #126258
DEF_TEST(CoreTextContract_LB3_NewlineEndsLine, reporter) {
    sk_sp<FontCollection> fontCollection = asset_collection({{"fonts/ahem.ttf", "Ahem"}});
    if (!fontCollection) {
        return;
    }
    const TextStyle small = contract_text_style("Ahem", 10.0f);
    auto trailing =
            layout_text(reporter, fontCollection, contract_paragraph_style(), small, "a\n", 100.0f);
    std::vector<LineMetrics> lines = line_metrics(trailing.get());
    REPORTER_ASSERT(reporter, lines.size() == 2, "got %zu lines", lines.size());
    REPORTER_ASSERT(reporter, lines[0].fWidth == 10, "got %g", lines[0].fWidth);

    std::unique_ptr<ParagraphBuilderImpl> builder =
            contract_builder(fontCollection, contract_paragraph_style());
    builder->pushStyle(small);
    builder->addText("a\n");
    builder->pushStyle(contract_text_style("Ahem", 20.0f));
    builder->addText("\n");
    builder->pushStyle(small);
    builder->addText("b");
    auto mixed = layout_built(reporter, builder.get(), 100.0f);
    lines = line_metrics(mixed.get());
    REPORTER_ASSERT(reporter, lines.size() == 3, "got %zu lines", lines.size());
    REPORTER_ASSERT(reporter, lines[1].fHeight == 20, "got %g", lines[1].fHeight);
    REPORTER_ASSERT(reporter, lines[2].fHeight == 10, "got %g", lines[2].fHeight);

    ParagraphStyle strutStyle = contract_paragraph_style();
    StrutStyle strut;
    strut.setStrutEnabled(true);
    strut.setForceStrutHeight(true);
    strut.setFontFamilies({SkString("Ahem")});
    strut.setFontSize(20.0f);
    strutStyle.setStrutStyle(strut);
    auto strutted = layout_text(reporter, fontCollection, strutStyle, small, "a\n\nb", 100.0f);
    lines = line_metrics(strutted.get());
    REPORTER_ASSERT(reporter, lines.size() == 3, "got %zu lines", lines.size());
    for (const LineMetrics& line : lines) {
        skiatest::ReporterContext ctx(reporter, SkStringPrintf("line %zu", line.fLineNumber));
        REPORTER_ASSERT(reporter, line.fHeight == 20, "got %g", line.fHeight);
    }
}

// LB4: layout is a pure function of width: layout(w1), layout(w2), layout(w1) returns what the
// first layout(w1) did, and rewrapping draws the same glyphs apart from spaces at the breaks
DEF_TEST(CoreTextContract_LB4_RelayoutIdempotent, reporter) {
    sk_sp<FontCollection> fontCollection =
            asset_collection({{"fonts/Roboto-Regular.ttf", "Roboto"}});
    if (!fontCollection) {
        return;
    }
    auto paragraph = layout_text(reporter,
                                 fontCollection,
                                 contract_paragraph_style(),
                                 contract_text_style("Roboto", 20.0f),
                                 "The quick brown fox jumps over the lazy dog",
                                 200.0f);
    const SkString first = layout_snapshot(paragraph.get());
    const std::vector<SkGlyphID> wideGlyphs = non_space_glyphs(paragraph.get());
    paragraph->layout(80.0f);
    const std::vector<SkGlyphID> narrowGlyphs = non_space_glyphs(paragraph.get());
    REPORTER_ASSERT(reporter, paragraph->lineNumber() > 2, "got %zu", paragraph->lineNumber());
    REPORTER_ASSERT(reporter, narrowGlyphs == wideGlyphs);
    paragraph->layout(200.0f);
    const SkString again = layout_snapshot(paragraph.get());
    REPORTER_ASSERT(reporter, again == first, "got\n%s\nwant\n%s", again.c_str(), first.c_str());
}

// LB5: a word that fits nowhere breaks mid-word and a character wider than the line overflows
// on its own line. Omits R126 (minIntrinsicWidth depends on the previous layout) and B8
DEF_TEST(CoreTextContract_LB5_LongWordBreaks, reporter) {
    sk_sp<FontCollection> fontCollection = asset_collection({{"fonts/ahem.ttf", "Ahem"}});
    if (!fontCollection) {
        return;
    }
    const TextStyle style = contract_text_style("Ahem", 10.0f);
    auto narrow = layout_text(
            reporter, fontCollection, contract_paragraph_style(), style, "aaaaaaaaaa", 45.0f);
    std::vector<LineMetrics> lines = line_metrics(narrow.get());
    REPORTER_ASSERT(reporter, lines.size() == 3, "got %zu lines", lines.size());
    for (const LineMetrics& line : lines) {
        skiatest::ReporterContext ctx(reporter, SkStringPrintf("line %zu", line.fLineNumber));
        REPORTER_ASSERT(reporter, line.fWidth <= 45, "got %g", line.fWidth);
    }
    REPORTER_ASSERT(reporter,
                    narrow->getMinIntrinsicWidth() <= narrow->getMaxIntrinsicWidth(),
                    "got min %g, max %g",
                    narrow->getMinIntrinsicWidth(),
                    narrow->getMaxIntrinsicWidth());

    auto tiny = layout_text(
            reporter, fontCollection, contract_paragraph_style(), style, "aaaaaaaaaa", 5.0f);
    lines = line_metrics(tiny.get());
    REPORTER_ASSERT(reporter, lines.size() == 10, "got %zu lines", lines.size());
    for (const LineMetrics& line : lines) {
        skiatest::ReporterContext ctx(reporter, SkStringPrintf("line %zu", line.fLineNumber));
        REPORTER_ASSERT(reporter, line.fWidth == 10, "got %g", line.fWidth);
    }
}

// LB6: LF, CRLF (one break), FF, U+2028 and U+2029 end a line with hardBreak, as does the last
// line but not one cut by maxLines. Omits lone CR, U+0085 (B5) and VT, pending with Flutter
DEF_TEST(CoreTextContract_LB6_HardBreaks, reporter) {
    sk_sp<FontCollection> fontCollection = asset_collection({{"fonts/ahem.ttf", "Ahem"}});
    if (!fontCollection) {
        return;
    }
    struct BreakCase {
        const char* separator;
        size_t secondLineStart;  // UTF-16, the LineMetrics index domain
    };
    constexpr BreakCase kCases[] = {
            {"\n", 2}, {"\r\n", 3}, {"\f", 2}, {"\u2028", 2}, {"\u2029", 2}};
    const TextStyle style = contract_text_style("Ahem", 10.0f);
    for (const BreakCase& c : kCases) {
        skiatest::ReporterContext ctx(reporter, SkStringPrintf("separator %s", c.separator));
        auto paragraph = layout_text(reporter,
                                     fontCollection,
                                     contract_paragraph_style(),
                                     style,
                                     std::string("a") + c.separator + "b",
                                     100.0f);
        const std::vector<LineMetrics> lines = line_metrics(paragraph.get());
        REPORTER_ASSERT(reporter, lines.size() == 2, "got %zu lines", lines.size());
        if (lines.size() != 2) {
            continue;
        }
        REPORTER_ASSERT(reporter, lines[0].fHardBreak);
        REPORTER_ASSERT(reporter, lines[1].fHardBreak);
        REPORTER_ASSERT(reporter,
                        lines[1].fStartIndex == c.secondLineStart,
                        "got %zu, want %zu",
                        lines[1].fStartIndex,
                        c.secondLineStart);
    }

    ParagraphStyle cut = contract_paragraph_style();
    cut.setMaxLines(1);
    auto paragraph = layout_text(reporter, fontCollection, cut, style, "ab cd", 25.0f);
    const std::vector<LineMetrics> lines = line_metrics(paragraph.get());
    REPORTER_ASSERT(reporter, lines.size() == 1, "got %zu lines", lines.size());
    REPORTER_ASSERT(reporter, lines.size() == 1 && !lines[0].fHardBreak);
}

// LB7: U+00AD takes no width inside a line; a line broken at it ends with the soft hyphen glyph
// only when hyphens are rendered (Flutter Hyphens.manual vs hidden)
DEF_TEST(CoreTextContract_LB7_SoftHyphen, reporter) {
    sk_sp<FontCollection> fontCollection =
            asset_collection({{"fonts/Roboto-Regular.ttf", "Roboto"}});
    if (!fontCollection) {
        return;
    }
    const TextStyle style = contract_text_style("Roboto", 20.0f);
    auto plain = layout_text(
            reporter, fontCollection, contract_paragraph_style(), style, "abcd", 1000.0f);
    auto soft = layout_text(
            reporter, fontCollection, contract_paragraph_style(), style, "ab\u00ADcd", 1000.0f);
    REPORTER_ASSERT(reporter,
                    soft->getMaxIntrinsicWidth() == plain->getMaxIntrinsicWidth(),
                    "got %g, want %g",
                    soft->getMaxIntrinsicWidth(),
                    plain->getMaxIntrinsicWidth());
    const size_t inlineGlyphs = drawn_glyphs(soft.get()).size();
    REPORTER_ASSERT(reporter, inlineGlyphs == 4, "got %zu", inlineGlyphs);

    for (bool render : {false, true}) {
        skiatest::ReporterContext ctx(reporter, SkStringPrintf("render %d", render));
        ParagraphStyle paragraphStyle = contract_paragraph_style();
        paragraphStyle.setRenderSoftHyphens(render);
        auto broken =
                layout_text(reporter, fontCollection, paragraphStyle, style, "ab\u00ADcd", 30.0f);
        REPORTER_ASSERT(reporter, broken->lineNumber() == 2, "got %zu", broken->lineNumber());
        std::vector<SkGlyphID> firstLine;
        SkFont font;
        for (const DrawnGlyph& glyph : drawn_glyphs(broken.get())) {
            if (glyph.lineNumber == 0) {
                firstLine.push_back(glyph.glyph);
                font = glyph.font;
            }
        }
        const size_t want = render ? 3 : 2;
        REPORTER_ASSERT(
                reporter, firstLine.size() == want, "got %zu, want %zu", firstLine.size(), want);
        if (render && !firstLine.empty()) {
            const SkGlyphID hyphen = font.unicharToGlyph('-');
            REPORTER_ASSERT(reporter,
                            firstLine.back() == hyphen,
                            "got %u, want %u",
                            firstLine.back(),
                            hyphen);
        }
    }
}

// LB8: maxIntrinsicWidth counts trailing spaces, and every TextAlign places the line by its
// width without them, kStart / kEnd following the paragraph direction
DEF_TEST(CoreTextContract_LB8_AlignmentsAndTrailingSpaces, reporter) {
    sk_sp<FontCollection> fontCollection = asset_collection({{"fonts/ahem.ttf", "Ahem"}});
    if (!fontCollection) {
        return;
    }
    auto spaces = layout_text(reporter,
                              fontCollection,
                              contract_paragraph_style(),
                              contract_text_style("Ahem", 14.0f),
                              "ab  ",
                              1000.0f);
    REPORTER_ASSERT(reporter,
                    spaces->getMaxIntrinsicWidth() == 56,
                    "got %g",
                    spaces->getMaxIntrinsicWidth());

    constexpr auto kCases = std::to_array<std::tuple<TextAlign, TextDirection, SkScalar>>({
            {TextAlign::kLeft, TextDirection::kLtr, 0},
            {TextAlign::kRight, TextDirection::kLtr, 80},
            {TextAlign::kCenter, TextDirection::kLtr, 40},
            {TextAlign::kJustify, TextDirection::kLtr, 0},
            {TextAlign::kStart, TextDirection::kLtr, 0},
            {TextAlign::kEnd, TextDirection::kLtr, 80},
            {TextAlign::kLeft, TextDirection::kRtl, 0},
            {TextAlign::kRight, TextDirection::kRtl, 80},
            {TextAlign::kCenter, TextDirection::kRtl, 40},
            {TextAlign::kJustify, TextDirection::kRtl, 80},
            {TextAlign::kStart, TextDirection::kRtl, 80},
            {TextAlign::kEnd, TextDirection::kRtl, 0},
    });
    const TextStyle style = contract_text_style("Ahem", 10.0f);
    for (const auto& [align, direction, left] : kCases) {
        skiatest::ReporterContext ctx(reporter,
                                      SkStringPrintf("align %d, direction %d",
                                                     static_cast<int>(align),
                                                     static_cast<int>(direction)));
        ParagraphStyle paragraphStyle = contract_paragraph_style();
        paragraphStyle.setTextAlign(align);
        paragraphStyle.setTextDirection(direction);
        auto paragraph = layout_text(reporter, fontCollection, paragraphStyle, style, "ab  ", 100);
        const std::vector<LineMetrics> lines = line_metrics(paragraph.get());
        REPORTER_ASSERT(reporter,
                        lines.size() == 1 && lines[0].fLeft == left,
                        "got %g, want %g",
                        lines.empty() ? -1.0 : lines[0].fLeft,
                        left);
    }

    ParagraphStyle justified = contract_paragraph_style();
    justified.setTextAlign(TextAlign::kJustify);
    auto paragraph = layout_text(reporter, fontCollection, justified, style, "ab cd ef", 55.0f);
    SkScalar lastRight = 0;
    for (const DrawnGlyph& glyph : drawn_glyphs(paragraph.get())) {
        if (glyph.lineNumber == 0) {
            lastRight = glyph.position.fX + 10;
        }
    }
    REPORTER_ASSERT(reporter, lastRight == 55, "got %g", lastRight);
}
