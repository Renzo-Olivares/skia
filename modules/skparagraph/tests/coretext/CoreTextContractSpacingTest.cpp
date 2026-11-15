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

#include "include/core/SkColor.h"
#include "include/core/SkRefCnt.h"
#include "include/core/SkScalar.h"
#include "include/core/SkString.h"
#include "include/core/SkTypes.h"
#include "modules/skparagraph/include/DartTypes.h"
#include "modules/skparagraph/include/FontCollection.h"
#include "modules/skparagraph/include/Paragraph.h"
#include "modules/skparagraph/include/ParagraphStyle.h"
#include "modules/skparagraph/include/TextStyle.h"
#include "modules/skparagraph/tests/coretext/CoreTextTestUtils.h"
#include "src/utils/mac/SkUniqueCFRef.h"
#include "tests/Test.h"

#include <CoreText/CoreText.h>

#include <algorithm>
#include <array>
#include <string_view>
#include <vector>

using namespace skia::textlayout;
using namespace skia::textlayout::coretext_test;

namespace {
// Drawn glyphs without repeats: SkParagraph paints a cluster that a style boundary splits once
// per style, clipped, so a split ligature comes twice in a row at the same position
std::vector<DrawnGlyph> distinct_drawn_glyphs(Paragraph* paragraph) {
    std::vector<DrawnGlyph> glyphs = drawn_glyphs(paragraph);
    auto repeats =
            std::unique(glyphs.begin(), glyphs.end(), [](const DrawnGlyph& a, const DrawnGlyph& b) {
                return a.glyph == b.glyph && a.position == b.position;
            });
    glyphs.erase(repeats, glyphs.end());
    return glyphs;
}
}  // namespace

// SP1: letterSpacing goes once after each grapheme, so a base with marks and an Indic syllable
// with a pre-base matra are not split and the line grows by spacing times the grapheme count
DEF_TEST(CoreTextContract_SP1_SpacingPerGrapheme, reporter) {
    struct Case {
        sk_sp<FontCollection> fontCollection;
        const char* family;
        std::string_view text;
        int graphemes;
    };
    if (!has_font_families({"Helvetica", ".AppleSystemUIFont"})) {
        return;
    }
    sk_sp<FontCollection> ahem = asset_collection({{"fonts/ahem.ttf", "Ahem"}});
    if (!ahem) {
        return;
    }
    const std::array<Case, 3> cases = {{
            {ahem, "Ahem", "Cancel", 6},
            {system_collection(), "Helvetica", "ae\u0301\u0323b", 3},
            // hi + anusvara, then dii: two graphemes
            {system_collection(), ".AppleSystemUIFont", "\u0939\u093F\u0902\u0926\u0940", 2},
    }};
    for (const Case& c : cases) {
        for (SkScalar letterSpacing : {-0.41f, 3.0f}) {
            skiatest::ReporterContext ctx(
                    reporter,
                    SkStringPrintf("%s '%s' ls %g", c.family, c.text.data(), letterSpacing));
            auto unspaced = layout_text(reporter,
                                        c.fontCollection,
                                        contract_paragraph_style(),
                                        spaced_text_style(c.family, 20, 0),
                                        c.text,
                                        1000);
            auto spaced = layout_text(reporter,
                                      c.fontCollection,
                                      contract_paragraph_style(),
                                      spaced_text_style(c.family, 20, letterSpacing),
                                      c.text,
                                      1000);
            const SkScalar growth =
                    first_line_width(spaced.get()) - first_line_width(unspaced.get());
            REPORTER_ASSERT(reporter,
                            SkScalarNearlyEqual(growth, letterSpacing * c.graphemes, 1e-3f),
                            "got %g, want %g",
                            growth,
                            letterSpacing * c.graphemes);
        }
    }
}

// SP2: letterSpacing adds to the font's trak as native tracking does: the line keeps its trak
// and grows by exactly letterSpacing per grapheme, for the system font, fallbacks and RTL
DEF_TEST(CoreTextContract_SP2_SpacingAddsToTrak, reporter) {
    if (!has_font_families({".AppleSystemUIFont"})) {
        return;
    }
    struct Case {
        const char* name;
        std::string_view text;
        TextDirection direction;
        int graphemes;
        // CoreText draws part of the text in fonts other than the system font
        bool fallback;
    };
    constexpr auto kCases = std::to_array<Case>({
            {"Latin",
             "The quick brown fox jumps over the lazy dog",
             TextDirection::kLtr,
             43,
             false},
            {"PingFang", "Ab \u6C34\u6C34\u6C34", TextDirection::kLtr, 6, true},
            {"Hebrew", "Ab \u05E9\u05DC\u05D5", TextDirection::kLtr, 6, true},
            {"mixed",
             "Hi \u6C34 \U0001F600 \u05E9\u05DC\u05D5\u05DD ok",
             TextDirection::kLtr,
             14,
             true},
            {"combining", "ae\u0301\u0323x o\u0308", TextDirection::kLtr, 5, false},
            {"Hebrew RTL",
             "\u05E9\u05DC\u05D5\u05DD \u05E2\u05D5\u05DC\u05DD",
             TextDirection::kRtl,
             9,
             true},
            {"Latin first RTL", "ok \u05E9\u05DC\u05D5\u05DD", TextDirection::kRtl, 7, true},
    });
    for (const Case& c : kCases) {
        ParagraphStyle paragraphStyle = contract_paragraph_style();
        paragraphStyle.setTextDirection(c.direction);
        for (SkScalar size : {11.0f, 17.0f, 28.0f, 48.0f}) {
            skiatest::ReporterContext ctx(reporter, SkStringPrintf("%s %gpt", c.name, size));
            auto unspaced = layout_text(reporter,
                                        system_collection(),
                                        paragraphStyle,
                                        spaced_text_style(".AppleSystemUIFont", size, 0),
                                        c.text,
                                        10000);
            const SkScalar unspacedWidth = first_line_width(unspaced.get());
            const size_t fonts = unspaced->getFonts().size();
            REPORTER_ASSERT(reporter, (fonts > 1) == c.fallback, "got %zu fonts", fonts);
            SkScalar advances = 0;
            for (const DrawnGlyph& glyph : drawn_glyphs(unspaced.get())) {
                advances += font_advance(glyph);
            }
            REPORTER_ASSERT(reporter,
                            !SkScalarNearlyEqual(unspacedWidth, advances, 1e-3f),
                            "got %g, the fonts' own advances",
                            unspacedWidth);
            for (SkScalar letterSpacing : {-0.41f, 1.0f}) {
                auto spaced =
                        layout_text(reporter,
                                    system_collection(),
                                    paragraphStyle,
                                    spaced_text_style(".AppleSystemUIFont", size, letterSpacing),
                                    c.text,
                                    10000);
                const SkScalar growth = first_line_width(spaced.get()) - unspacedWidth;
                REPORTER_ASSERT(reporter,
                                SkScalarNearlyEqual(growth, letterSpacing * c.graphemes, 1e-3f),
                                "ls %g: got %g, want %g",
                                letterSpacing,
                                growth,
                                letterSpacing * c.graphemes);
            }
        }
    }
}

// SP3: any nonzero letterSpacing, negative too, turns ligatures off as native tracking does, so
// Helvetica fi, ffl and Roboto fi, fl draw one glyph per letter
DEF_TEST(CoreTextContract_SP3_SpacingDisablesLigatures, reporter) {
    struct Case {
        sk_sp<FontCollection> fontCollection;
        const char* family;
        std::string_view text;
    };
    if (!has_font_families({"Helvetica"})) {
        return;
    }
    sk_sp<FontCollection> roboto = asset_collection({{"fonts/Roboto-Regular.ttf", "Roboto"}});
    if (!roboto) {
        return;
    }
    const std::array<Case, 2> cases = {{
            {roboto, "Roboto", "fifl"},
            {system_collection(), "Helvetica", "office fluffy"},
    }};
    for (const Case& c : cases) {
        for (SkScalar letterSpacing : {0.0f, -0.5f, 0.5f, 2.0f}) {
            skiatest::ReporterContext ctx(reporter,
                                          SkStringPrintf("%s ls %g", c.family, letterSpacing));
            auto paragraph = layout_text(reporter,
                                         c.fontCollection,
                                         contract_paragraph_style(),
                                         spaced_text_style(c.family, 24, letterSpacing),
                                         c.text,
                                         1000);
            const size_t glyphs = drawn_glyphs(paragraph.get()).size();
            if (letterSpacing == 0) {
                REPORTER_ASSERT(reporter, glyphs < c.text.size(), "got %zu glyphs", glyphs);
            } else {
                REPORTER_ASSERT(reporter,
                                glyphs == c.text.size(),
                                "got %zu glyphs, want %zu",
                                glyphs,
                                c.text.size());
            }
        }
    }
}

// SP4: cursive scripts get no letterSpacing and keep their ligatures: an Arabic word draws the
// same glyphs at the same width at letterSpacing 0 and 5 (Arial lam-lam-heh, SF Arabic)
DEF_TEST(CoreTextContract_SP4_CursiveUnspaced, reporter) {
    struct Case {
        const char* family;
        std::string_view text;
    };
    constexpr auto kCases =
            std::to_array<Case>({{"Arial", "\u0627\u0644\u0644\u0647"},
                                 {".AppleSystemUIFont", "\u0633\u0644\u0627\u0645"}});
    ParagraphStyle paragraphStyle = contract_paragraph_style();
    paragraphStyle.setTextDirection(TextDirection::kRtl);
    for (const Case& c : kCases) {
        if (!has_font_families({c.family})) {
            continue;
        }
        skiatest::ReporterContext ctx(reporter, SkStringPrintf("%s", c.family));
        auto unspaced = layout_text(reporter,
                                    system_collection(),
                                    paragraphStyle,
                                    spaced_text_style(c.family, 20, 0),
                                    c.text,
                                    1000);
        auto spaced = layout_text(reporter,
                                  system_collection(),
                                  paragraphStyle,
                                  spaced_text_style(c.family, 20, 5),
                                  c.text,
                                  1000);
        const SkScalar unspacedWidth = first_line_width(unspaced.get());
        const SkScalar spacedWidth = first_line_width(spaced.get());
        REPORTER_ASSERT(reporter,
                        SkScalarNearlyEqual(spacedWidth, unspacedWidth, 1e-3f),
                        "got %g, want %g",
                        spacedWidth,
                        unspacedWidth);
        const std::vector<DrawnGlyph> unspacedGlyphs = drawn_glyphs(unspaced.get());
        const std::vector<DrawnGlyph> spacedGlyphs = drawn_glyphs(spaced.get());
        REPORTER_ASSERT(reporter,
                        spacedGlyphs.size() == unspacedGlyphs.size(),
                        "got %zu glyphs, want %zu",
                        spacedGlyphs.size(),
                        unspacedGlyphs.size());
        for (size_t k = 0; k < std::min(spacedGlyphs.size(), unspacedGlyphs.size()); ++k) {
            REPORTER_ASSERT(reporter,
                            spacedGlyphs[k].glyph == unspacedGlyphs[k].glyph,
                            "glyph %zu: got %u, want %u",
                            k,
                            spacedGlyphs[k].glyph,
                            unspacedGlyphs[k].glyph);
        }
    }
}

// SP5: a letter-spaced line starts half a letterSpacing to the right under left and justify
// alignment (Flutter #103705, Skia CL 541978); right and center place the line without that shift
DEF_TEST(CoreTextContract_SP5_HalfSpacingShift, reporter) {
    sk_sp<FontCollection> ahem = asset_collection({{"fonts/ahem.ttf", "Ahem"}});
    if (!ahem) {
        return;
    }
    constexpr SkScalar kSpacing = 4;
    constexpr SkScalar kWidth = 200;
    constexpr SkScalar kLine = 3 * (kAhemSize + kSpacing);
    struct Case {
        TextAlign align;
        SkScalar firstGlyphX;
    };
    constexpr auto kCases = std::to_array<Case>({{TextAlign::kLeft, kSpacing / 2},
                                                 {TextAlign::kJustify, kSpacing / 2},
                                                 {TextAlign::kRight, kWidth - kLine},
                                                 {TextAlign::kCenter, (kWidth - kLine) / 2}});
    for (const Case& c : kCases) {
        skiatest::ReporterContext ctx(reporter,
                                      SkStringPrintf("align %d", static_cast<int>(c.align)));
        ParagraphStyle paragraphStyle = contract_paragraph_style();
        paragraphStyle.setTextAlign(c.align);
        auto paragraph = layout_text(reporter,
                                     ahem,
                                     paragraphStyle,
                                     spaced_text_style("Ahem", kAhemSize, kSpacing),
                                     "abc",
                                     kWidth);
        const std::vector<DrawnGlyph> glyphs = drawn_glyphs(paragraph.get());
        REPORTER_ASSERT(reporter, glyphs.size() == 3, "got %zu", glyphs.size());
        if (glyphs.empty()) {
            continue;
        }
        REPORTER_ASSERT(reporter,
                        glyphs[0].position.fX == c.firstGlyphX,
                        "got %g, want %g",
                        glyphs[0].position.fX,
                        c.firstGlyphX);
    }
}

// SP6: wordSpacing goes once onto each run of spaces, however long, and letterSpacing once per
// grapheme on top of it, also when two styles on the line carry different letterSpacing
DEF_TEST(CoreTextContract_SP6_WordSpacingPerSpaceRun, reporter) {
    sk_sp<FontCollection> ahem = asset_collection({{"fonts/ahem.ttf", "Ahem"}});
    if (!ahem) {
        return;
    }
    struct Case {
        std::string_view text;
        SkScalar letterSpacing;
        SkScalar wordSpacing;
        SkScalar growth;
    };
    constexpr auto kCases = std::to_array<Case>({
            {"a b", 0, 10, 10},
            {"a   b", 0, 10, 10},
            {"Hello World", 5, 0, 55},
            {"Hello World", 0, 10, 10},
            {"Hello World", 5, 10, 65},
    });
    for (const Case& c : kCases) {
        skiatest::ReporterContext ctx(
                reporter,
                SkStringPrintf("'%s' ls %g ws %g", c.text.data(), c.letterSpacing, c.wordSpacing));
        TextStyle style = spaced_text_style("Ahem", kAhemSize, c.letterSpacing);
        style.setWordSpacing(c.wordSpacing);
        auto paragraph =
                layout_text(reporter, ahem, contract_paragraph_style(), style, c.text, 1000);
        const SkScalar width = first_line_width(paragraph.get());
        const SkScalar expected = c.text.size() * kAhemSize + c.growth;
        REPORTER_ASSERT(reporter, width == expected, "got %g, want %g", width, expected);
    }

    auto builder = contract_builder(ahem, contract_paragraph_style());
    builder->pushStyle(spaced_text_style("Ahem", kAhemSize, 4));
    builder->addText("Hello ");
    builder->pushStyle(spaced_text_style("Ahem", kAhemSize, 2));
    builder->addText("World");
    auto twoStyles = layout_built(reporter, builder.get(), 1000);
    // "Hello " takes 6 x 4 and "World" 5 x 2
    const SkScalar width = first_line_width(twoStyles.get());
    REPORTER_ASSERT(reporter, width == 11 * kAhemSize + 34, "got %g", width);
}

// SP7: trailing spaces get letterSpacing but stay out of fWidth and longestLine (a spaces-only
// line keeps them in longestLine); in RTL they follow the logical end, left of the text
DEF_TEST(CoreTextContract_SP7_TrailingSpaces, reporter) {
    sk_sp<FontCollection> ahem = asset_collection({{"fonts/ahem.ttf", "Ahem"}});
    if (!ahem) {
        return;
    }
    for (SkScalar letterSpacing : {0.0f, 1.0f}) {
        skiatest::ReporterContext ctx(reporter, SkStringPrintf("ls %g", letterSpacing));
        const SkScalar letter = kAhemSize + letterSpacing;
        const TextStyle style = spaced_text_style("Ahem", kAhemSize, letterSpacing);
        auto paragraph =
                layout_text(reporter, ahem, contract_paragraph_style(), style, "ab   ", 1000);
        const SkScalar width = first_line_width(paragraph.get());
        REPORTER_ASSERT(reporter, width == 2 * letter, "got %g, want %g", width, 2 * letter);
        REPORTER_ASSERT(reporter,
                        paragraph->getLongestLine() == 2 * letter,
                        "got %g, want %g",
                        paragraph->getLongestLine(),
                        2 * letter);
        REPORTER_ASSERT(reporter,
                        paragraph->getMaxIntrinsicWidth() == 5 * letter,
                        "got %g, want %g",
                        paragraph->getMaxIntrinsicWidth(),
                        5 * letter);
        const std::vector<TextBox> spaces =
                paragraph->getRectsForRange(2, 5, RectHeightStyle::kTight, RectWidthStyle::kTight);
        REPORTER_ASSERT(reporter, spaces.size() == 1, "got %zu boxes", spaces.size());
        if (spaces.size() == 1) {
            REPORTER_ASSERT(reporter,
                            spaces[0].rect.width() == 3 * letter,
                            "got %g, want %g",
                            spaces[0].rect.width(),
                            3 * letter);
        }

        auto onlySpaces =
                layout_text(reporter, ahem, contract_paragraph_style(), style, "   ", 1000);
        const SkScalar spacesWidth = first_line_width(onlySpaces.get());
        REPORTER_ASSERT(reporter, spacesWidth == 0, "got %g", spacesWidth);
        REPORTER_ASSERT(reporter,
                        onlySpaces->getLongestLine() == 3 * letter,
                        "got %g, want %g",
                        onlySpaces->getLongestLine(),
                        3 * letter);
    }

    ParagraphStyle rtl = contract_paragraph_style();
    rtl.setTextDirection(TextDirection::kRtl);
    auto hebrew = layout_text(
            reporter, ahem, rtl, contract_text_style("Ahem", kAhemSize), "\u05D0\u05D1   ", 1000);
    const std::vector<TextBox> letters =
            hebrew->getRectsForRange(0, 2, RectHeightStyle::kTight, RectWidthStyle::kTight);
    const std::vector<TextBox> spaces =
            hebrew->getRectsForRange(2, 5, RectHeightStyle::kTight, RectWidthStyle::kTight);
    REPORTER_ASSERT(reporter,
                    letters.size() == 1 && spaces.size() == 1,
                    "got %zu and %zu boxes",
                    letters.size(),
                    spaces.size());
    if (letters.size() == 1 && spaces.size() == 1) {
        REPORTER_ASSERT(reporter,
                        spaces[0].rect.fRight <= letters[0].rect.fLeft,
                        "spaces %g..%g, letters %g..%g",
                        spaces[0].rect.fLeft,
                        spaces[0].rect.fRight,
                        letters[0].rect.fLeft,
                        letters[0].rect.fRight);
    }
}

// SP7 with native trak: in the system font the spaced line without its trailing spaces is as
// wide as the native tracked CTLine of the text without them (a line metric, not glyph x)
DEF_TEST(CoreTextContract_SP7_TrailingSpacesNativeTracking, reporter) {
    if (!has_font_families({".AppleSystemUIFont"})) {
        return;
    }
    SkUniqueCFRef<CTFontRef> system(
            CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, 17, nullptr));
    for (SkScalar letterSpacing : {-0.41f, 1.0f}) {
        skiatest::ReporterContext ctx(reporter, SkStringPrintf("ls %g", letterSpacing));
        auto paragraph = layout_text(reporter,
                                     system_collection(),
                                     contract_paragraph_style(),
                                     spaced_text_style(".AppleSystemUIFont", 17, letterSpacing),
                                     "ok  ",
                                     1000);
        SkUniqueCFRef<CTLineRef> line = native_line(system.get(), "ok", letterSpacing);
        const SkScalar width = first_line_width(paragraph.get());
        const SkScalar nativeWidth = native_line_width(line.get());
        REPORTER_ASSERT(reporter,
                        SkScalarNearlyEqual(width, nativeWidth, kNativeWidthTolerance),
                        "got %.5f, want %.5f",
                        width,
                        nativeWidth);
    }
}

// SP8: the ellipsis takes no letterSpacing but keeps its fallback font's trak: a spaced line's
// "\u6C34\u6C34" ellipsis draws at an unspaced paragraph's pitch, not the font's advance
DEF_TEST(CoreTextContract_SP8_EllipsisIgnoresSpacing, reporter) {
    if (!has_font_families({".AppleSystemUIFont"})) {
        return;
    }
    constexpr const char* kEllipsis = "\u6C34\u6C34";
    for (SkScalar size : {9.0f, 17.0f, 40.0f}) {
        skiatest::ReporterContext ctx(reporter, SkStringPrintf("%gpt", size));
        ParagraphStyle paragraphStyle = contract_paragraph_style();
        paragraphStyle.setMaxLines(1);
        paragraphStyle.setEllipsis(SkString(kEllipsis));
        auto truncated = layout_text(reporter,
                                     system_collection(),
                                     paragraphStyle,
                                     spaced_text_style(".AppleSystemUIFont", size, 5),
                                     "The quick brown fox jumps over the lazy dog again and again",
                                     10 * size);
        auto standalone = layout_text(reporter,
                                      system_collection(),
                                      contract_paragraph_style(),
                                      contract_text_style(".AppleSystemUIFont", size),
                                      kEllipsis,
                                      1000);
        const std::vector<DrawnGlyph> line = drawn_glyphs(truncated.get());
        const std::vector<DrawnGlyph> expected = drawn_glyphs(standalone.get());
        REPORTER_ASSERT(reporter,
                        line.size() > 2 && expected.size() == 2,
                        "got %zu and %zu glyphs",
                        line.size(),
                        expected.size());
        if (line.size() <= 2 || expected.size() != 2) {
            continue;
        }
        // The ellipsis run is drawn last on a left-to-right line
        const DrawnGlyph& first = line[line.size() - 2];
        const DrawnGlyph& second = line[line.size() - 1];
        REPORTER_ASSERT(reporter,
                        first.glyph == expected[0].glyph,
                        "got %u, want %u",
                        first.glyph,
                        expected[0].glyph);
        const SkScalar pitch = second.position.fX - first.position.fX;
        const SkScalar expectedPitch = expected[1].position.fX - expected[0].position.fX;
        REPORTER_ASSERT(reporter,
                        SkScalarNearlyEqual(pitch, expectedPitch, 1e-3f),
                        "got %g, want %g",
                        pitch,
                        expectedPitch);
        REPORTER_ASSERT(reporter,
                        !SkScalarNearlyEqual(pitch, font_advance(first), 1e-3f),
                        "got %g, the font's own advance in %s",
                        pitch,
                        family_name(first.font).c_str());
    }
}

// SP9: a style change by color alone moves no glyph: three color spans draw the same glyphs at
// the same positions as one style, across Latin kerning pairs and joined Arabic letters
DEF_TEST(CoreTextContract_SP9_ColorSplitKeepsPositions, reporter) {
    struct Case {
        const char* family;
        std::string_view text;
        TextDirection direction;
    };
    constexpr auto kCases = std::to_array<Case>(
            {{"Helvetica", "AVATAR Tower", TextDirection::kLtr},
             {"Arial",
              "\u0627\u0644\u0633\u0644\u0627\u0645 \u0639\u0644\u064A\u0643\u0645",
              TextDirection::kRtl}});
    for (const Case& c : kCases) {
        if (!has_font_families({c.family})) {
            continue;
        }
        skiatest::ReporterContext ctx(reporter, SkStringPrintf("%s", c.family));
        ParagraphStyle paragraphStyle = contract_paragraph_style();
        paragraphStyle.setTextDirection(c.direction);
        TextStyle style = contract_text_style(c.family, 20);
        auto single =
                layout_text(reporter, system_collection(), paragraphStyle, style, c.text, 1000);

        auto builder = contract_builder(system_collection(), paragraphStyle);
        // Split points fall inside the "TA" and "To" kerning pairs and inside the Arabic word,
        // the second between lam and alef of the lam-alef ligature
        const size_t splits[] = {0, 4, 8, c.text.size()};
        constexpr SkColor kColors[] = {SK_ColorBLACK, SK_ColorRED, SK_ColorBLUE};
        for (int i = 0; i < 3; ++i) {
            style.setColor(kColors[i]);
            builder->pushStyle(style);
            builder->addText(c.text.data() + splits[i], splits[i + 1] - splits[i]);
        }
        auto colored = layout_built(reporter, builder.get(), 1000);

        const std::vector<DrawnGlyph> expected = drawn_glyphs(single.get());
        const std::vector<DrawnGlyph> glyphs = distinct_drawn_glyphs(colored.get());
        REPORTER_ASSERT(reporter,
                        glyphs.size() == expected.size(),
                        "got %zu glyphs, want %zu",
                        glyphs.size(),
                        expected.size());
        for (size_t k = 0; k < std::min(glyphs.size(), expected.size()); ++k) {
            REPORTER_ASSERT(reporter,
                            glyphs[k].glyph == expected[k].glyph &&
                                    SkScalarNearlyEqual(
                                            glyphs[k].position.fX, expected[k].position.fX, 1e-4f),
                            "glyph %zu: got %u at %g, want %u at %g",
                            k,
                            glyphs[k].glyph,
                            glyphs[k].position.fX,
                            expected[k].glyph,
                            expected[k].position.fX);
        }
        const SkScalar width = first_line_width(colored.get());
        const SkScalar expectedWidth = first_line_width(single.get());
        REPORTER_ASSERT(reporter, width == expectedWidth, "got %g, want %g", width, expectedWidth);
    }
}

// SP9: a non-zero sub-epsilon letterSpacing span disables ligatures on itself without coalescing
// into an adjacent zero-spacing span, while two NaN-spacing spans still coalesce across color
DEF_TEST(CoreTextContract_SP9_TinyAndNanLetterSpacingCoalescing, reporter) {
    sk_sp<FontCollection> roboto = asset_collection({{"fonts/Roboto-Regular.ttf", "Roboto"}});
    if (!roboto) {
        return;
    }
    // ParagraphCache relaxes sub-epsilon letterSpacing, which would alias standalone "fi" runs
    roboto->getParagraphCache()->turnOn(false);
    const TextStyle zeroStyle = spaced_text_style("Roboto", 20.0f, 0.0f);
    const TextStyle tinyStyle = spaced_text_style("Roboto", 20.0f, 1e-4f);
    const size_t zeroGlyphs =
            drawn_glyph_ids(
                    layout_text(reporter, roboto, contract_paragraph_style(), zeroStyle, "fi", 1000)
                            .get())
                    .size();
    const size_t tinyGlyphs =
            drawn_glyph_ids(
                    layout_text(reporter, roboto, contract_paragraph_style(), tinyStyle, "fi", 1000)
                            .get())
                    .size();
    REPORTER_ASSERT(reporter, zeroGlyphs == 1, "got %zu glyphs", zeroGlyphs);
    REPORTER_ASSERT(reporter, tinyGlyphs == 2, "got %zu glyphs", tinyGlyphs);

    auto builder = contract_builder(roboto, contract_paragraph_style());
    builder->pushStyle(tinyStyle);
    builder->addText("fi");
    builder->pop();
    builder->pushStyle(zeroStyle);
    builder->addText("fi");
    builder->pop();
    auto combined = layout_built(reporter, builder.get(), 1000.0f);
    const size_t gotGlyphs = drawn_glyph_ids(combined.get()).size();
    REPORTER_ASSERT(reporter,
                    gotGlyphs == tinyGlyphs + zeroGlyphs,
                    "got %zu glyphs, want %zu",
                    gotGlyphs,
                    tinyGlyphs + zeroGlyphs);

    if (!has_font_families({"Arial"})) {
        return;
    }
    ParagraphStyle rtl = contract_paragraph_style();
    rtl.setTextDirection(TextDirection::kRtl);
    TextStyle nanArabic = spaced_text_style("Arial", 20.0f, SK_ScalarNaN);
    auto singleArabic =
            layout_text(reporter, system_collection(), rtl, nanArabic, "\u0644\u0627", 1000.0f);
    auto nanBuilder = contract_builder(system_collection(), rtl);
    nanArabic.setColor(SK_ColorRED);
    nanBuilder->pushStyle(nanArabic);
    nanBuilder->addText("\u0644");
    nanBuilder->pop();
    nanArabic.setColor(SK_ColorBLUE);
    nanBuilder->pushStyle(nanArabic);
    nanBuilder->addText("\u0627");
    nanBuilder->pop();
    auto splitArabic = layout_built(reporter, nanBuilder.get(), 1000.0f);
    const size_t singleCount = drawn_glyph_ids(singleArabic.get()).size();
    const size_t splitCount = distinct_drawn_glyphs(splitArabic.get()).size();
    REPORTER_ASSERT(reporter,
                    splitCount == singleCount,
                    "got %zu glyphs, want %zu",
                    splitCount,
                    singleCount);
}
