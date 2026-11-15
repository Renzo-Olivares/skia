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
#include "include/core/SkTypes.h"
#include "modules/skparagraph/include/DartTypes.h"
#include "modules/skparagraph/include/FontCollection.h"
#include "modules/skparagraph/include/ParagraphStyle.h"
#include "modules/skparagraph/include/TextStyle.h"
#include "modules/skparagraph/src/ParagraphBuilderImpl.h"
#include "modules/skparagraph/tests/coretext/CoreTextTestUtils.h"
#include "tests/Test.h"

#include <string.h>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace skia::textlayout;
using namespace skia::textlayout::coretext_test;

namespace {
enum class StyleSplit { kNone, kColor, kLetterSpacing, kSize };

// Drawn glyph ids of four Arabic beh in Arial 20pt, the last two in a second span that differs
// from the first by split
std::vector<SkGlyphID> split_glyph_ids(skiatest::Reporter* reporter, StyleSplit split) {
    std::unique_ptr<ParagraphBuilderImpl> builder =
            contract_builder(system_collection(), contract_paragraph_style());
    TextStyle style = contract_text_style("Arial", 20.0f);
    builder->pushStyle(style);
    builder->addText("\u0628\u0628");
    switch (split) {
        case StyleSplit::kNone:
            break;
        case StyleSplit::kColor:
            style.setColor(SK_ColorRED);
            break;
        case StyleSplit::kLetterSpacing:
            style.setLetterSpacing(3.0f);
            break;
        case StyleSplit::kSize:
            style.setFontSize(30.0f);
            break;
    }
    builder->pushStyle(style);
    builder->addText("\u0628\u0628");
    return drawn_glyph_ids(layout_built(reporter, builder.get(), 1000.0f).get());
}
}  // namespace

// BD3: in RTL letterSpacing goes after every letter as in LTR: each Ahem box is one spacing
// wider than its glyph and boxes tile the line from the right edge, or from ls / 2 (SP5) in LTR
DEF_TEST(CoreTextContract_BD3_RtlSpacingPerLetter, reporter) {
    sk_sp<FontCollection> ahem = asset_collection({{"fonts/ahem.ttf", "Ahem"}});
    if (!ahem) {
        return;
    }
    constexpr SkScalar kWidth = 300;
    struct Case {
        std::string_view text;
        int letters;
        bool rtlScript;
    };
    constexpr auto kCases =
            std::to_array<Case>({{"Cancel", 6, false}, {"\u05E9\u05DC\u05D5\u05DD", 4, true}});
    for (const Case& c : kCases) {
        for (TextDirection direction : {TextDirection::kLtr, TextDirection::kRtl}) {
            for (SkScalar letterSpacing : {-0.41f, 1.5f}) {
                ParagraphStyle paragraphStyle = contract_paragraph_style();
                paragraphStyle.setTextDirection(direction);
                auto paragraph = layout_text(reporter,
                                             ahem,
                                             paragraphStyle,
                                             spaced_text_style("Ahem", kAhemSize, letterSpacing),
                                             c.text,
                                             kWidth);
                const SkScalar letter = kAhemSize + letterSpacing;
                const SkScalar lineLeft = direction == TextDirection::kRtl
                                                  ? kWidth - c.letters * letter
                                                  : letterSpacing / 2;
                for (int i = 0; i < c.letters; ++i) {
                    skiatest::ReporterContext ctx(
                            reporter,
                            SkStringPrintf("'%s' %s ls %g letter %d",
                                           c.text.data(),
                                           direction == TextDirection::kRtl ? "RTL" : "LTR",
                                           letterSpacing,
                                           i));
                    const int slot = c.rtlScript ? c.letters - 1 - i : i;
                    const SkScalar left = lineLeft + slot * letter;
                    const std::vector<TextBox> boxes = paragraph->getRectsForRange(
                            i, i + 1, RectHeightStyle::kTight, RectWidthStyle::kTight);
                    REPORTER_ASSERT(reporter, boxes.size() == 1, "got %zu boxes", boxes.size());
                    if (boxes.size() != 1) {
                        continue;
                    }
                    const SkRect& box = boxes[0].rect;
                    REPORTER_ASSERT(reporter,
                                    SkScalarNearlyEqual(box.fLeft, left, 1e-3f) &&
                                            SkScalarNearlyEqual(box.fRight, left + letter, 1e-3f),
                                    "got %g..%g, want %g..%g",
                                    box.fLeft,
                                    box.fRight,
                                    left,
                                    left + letter);
                }
            }
        }
    }
}

// BD1: run directions follow the Unicode BiDi algorithm on the base direction: strong runs
// keep theirs, LTR inside RTL stays LTR, and neutrals at a run edge take the base direction
DEF_TEST(CoreTextContract_BD1_BidiOnBaseDirection, reporter) {
    if (!has_font_families({"Arial"})) {
        return;
    }
    struct Segment {
        unsigned start;
        unsigned end;
        TextDirection direction;
    };
    struct Case {
        TextDirection base;
        const char* text;
        std::array<Segment, 3> visualOrder;  // left to right
        // Two code points of one segment, the first logically earlier
        std::array<unsigned, 2> inner;
        bool innerLeftToRight;
    };
    // "Hello " [0,6), Arabic [6,10), " World" [10,16); RTL: Arabic [0,4), "Hello" [5,10),
    // Arabic [11,15)
    const Case kCases[] = {
            {TextDirection::kLtr,
             "Hello \u0633\u0644\u0627\u0645 World",
             {{{0, 5, TextDirection::kLtr},
               {6, 10, TextDirection::kRtl},
               {11, 16, TextDirection::kLtr}}},
             {6, 9},
             false},
            {TextDirection::kRtl,
             "\u0633\u0644\u0627\u0645 Hello \u0633\u0644\u0627\u0645",
             {{{11, 15, TextDirection::kRtl},
               {5, 10, TextDirection::kLtr},
               {0, 4, TextDirection::kRtl}}},
             {5, 9},
             true},
    };
    for (const Case& c : kCases) {
        skiatest::ReporterContext ctx(
                reporter, SkStringPrintf("base %s", c.base == TextDirection::kRtl ? "rtl" : "ltr"));
        ParagraphStyle paragraphStyle = contract_paragraph_style();
        paragraphStyle.setTextDirection(c.base);
        auto paragraph = layout_text(reporter,
                                     system_collection(),
                                     paragraphStyle,
                                     contract_text_style("Arial", 14.0f),
                                     c.text,
                                     1000.0f);
        SkScalar previousRight = -1;
        for (const Segment& segment : c.visualOrder) {
            const std::optional<TextBox> segmentBox =
                    single_tight_box(reporter, paragraph.get(), segment.start, segment.end);
            if (!segmentBox) {
                continue;
            }
            REPORTER_ASSERT(reporter,
                            segmentBox->direction == segment.direction &&
                                    segmentBox->rect.fLeft >= previousRight,
                            "[%u, %u): %s at %g, previous ends at %g",
                            segment.start,
                            segment.end,
                            segmentBox->direction == TextDirection::kRtl ? "rtl" : "ltr",
                            segmentBox->rect.fLeft,
                            previousRight);
            previousRight = segmentBox->rect.fRight;
        }
        const std::optional<TextBox> first =
                single_tight_box(reporter, paragraph.get(), c.inner[0], c.inner[0] + 1);
        const std::optional<TextBox> second =
                single_tight_box(reporter, paragraph.get(), c.inner[1], c.inner[1] + 1);
        if (!first || !second) {
            continue;
        }
        REPORTER_ASSERT(reporter,
                        (first->rect.fLeft < second->rect.fLeft) == c.innerLeftToRight,
                        "code points at %g and %g",
                        first->rect.fLeft,
                        second->rect.fLeft);
    }

    sk_sp<FontCollection> ahem = asset_collection({{"fonts/ahem.ttf", "Ahem"}});
    if (!ahem) {
        return;
    }
    ParagraphStyle rtl = contract_paragraph_style();
    rtl.setTextDirection(TextDirection::kRtl);
    auto neutral =
            layout_text(reporter, ahem, rtl, contract_text_style("Ahem", 14.0f), "abc!", 1000.0f);
    const std::optional<TextBox> bang = single_tight_box(reporter, neutral.get(), 3, 4);
    const std::optional<TextBox> a = single_tight_box(reporter, neutral.get(), 0, 1);
    if (bang && a) {
        REPORTER_ASSERT(
                reporter,
                bang->rect.fRight <= a->rect.fLeft && bang->direction == TextDirection::kRtl,
                "'!' right %g dir %d, 'a' left %g",
                bang->rect.fRight,
                static_cast<int>(bang->direction),
                a->rect.fLeft);
    }
}

// BD2: Arabic letters keep their joining forms across a style boundary inside one font and size
// (color, letterSpacing), as native CoreText does, and break it when the size changes
DEF_TEST(CoreTextContract_BD2_ArabicJoinsAcrossStyles, reporter) {
    if (!has_font_families({"Arial"})) {
        return;
    }
    const std::vector<SkGlyphID> joined = split_glyph_ids(reporter, StyleSplit::kNone);
    REPORTER_ASSERT(reporter, joined.size() == 4, "got %zu", joined.size());
    REPORTER_ASSERT(reporter, split_glyph_ids(reporter, StyleSplit::kColor) == joined);
    REPORTER_ASSERT(reporter, split_glyph_ids(reporter, StyleSplit::kLetterSpacing) == joined);
    REPORTER_ASSERT(reporter, split_glyph_ids(reporter, StyleSplit::kSize) != joined);
}

// BD1 / R130: an LTR CTRun inside BiDi level 1 (via RLO) promotes to level 2 instead of 0 so
// surrounding RTL runs reverse right-to-left; an RTL base direction alone keeps the same order
DEF_TEST(CoreTextContract_BD1_RtlBaseLevelAndEmbeddedLtrOrder, reporter) {
    if (!has_font_families({"Arial"})) {
        return;
    }
    ParagraphStyle paragraphStyle = contract_paragraph_style();
    paragraphStyle.setTextDirection(TextDirection::kRtl);
    const TextStyle style = contract_text_style("Arial", 20.0f);
    std::unique_ptr<ParagraphBuilderImpl> builder =
            contract_builder(system_collection(), paragraphStyle);
    builder->pushStyle(style);
    builder->addText("\xE2\x80\xAE");
    builder->addPlaceholder(PlaceholderStyle(
            10.0f, 20.0f, PlaceholderAlignment::kBaseline, TextBaseline::kAlphabetic, 0.0f));
    // UTF-16 offsets: 0 RLO, 1 U+FFFC, [2,4) "אב", [4,7) "abc", [7,9) "גד"
    builder->addText(
            "\xD7\x90\xD7\x91"
            "abc"
            "\xD7\x92\xD7\x93");
    auto paragraph = layout_built(reporter, builder.get(), 1000.0f);

    const std::optional<TextBox> firstRtl = single_tight_box(reporter, paragraph.get(), 2, 4);
    const std::optional<TextBox> embeddedLtr = single_tight_box(reporter, paragraph.get(), 4, 7);
    const std::optional<TextBox> secondRtl = single_tight_box(reporter, paragraph.get(), 7, 9);
    if (!firstRtl || !embeddedLtr || !secondRtl) {
        return;
    }
    REPORTER_ASSERT(reporter,
                    secondRtl->rect.fRight <= embeddedLtr->rect.fLeft + 0.5f &&
                            embeddedLtr->rect.fRight <= firstRtl->rect.fLeft + 0.5f,
                    "got second [%g,%g], ltr [%g,%g], first [%g,%g]",
                    secondRtl->rect.fLeft,
                    secondRtl->rect.fRight,
                    embeddedLtr->rect.fLeft,
                    embeddedLtr->rect.fRight,
                    firstRtl->rect.fLeft,
                    firstRtl->rect.fRight);

    auto rtlBase = layout_text(reporter,
                               system_collection(),
                               paragraphStyle,
                               style,
                               "!\xD7\x90\xD7\x91"
                               "abc"
                               "\xD7\x92\xD7\x93\n",
                               1000.0f);
    const std::vector<DrawnGlyph> rtlBaseGlyphs = drawn_glyphs(rtlBase.get());
    REPORTER_ASSERT(reporter, !rtlBaseGlyphs.empty());
    SkScalar firstRtlX = -1, ltrX = -1, secondRtlX = -1;
    for (const DrawnGlyph& g : rtlBaseGlyphs) {
        if (g.utf8Start >= 1 && g.utf8Start < 5 && firstRtlX < 0) {
            firstRtlX = g.position.fX;
        } else if (g.utf8Start >= 5 && g.utf8Start < 8 && ltrX < 0) {
            ltrX = g.position.fX;
        } else if (g.utf8Start >= 8 && g.utf8Start < 12 && secondRtlX < 0) {
            secondRtlX = g.position.fX;
        }
    }
    REPORTER_ASSERT(reporter,
                    secondRtlX >= 0 && secondRtlX < ltrX && ltrX < firstRtlX,
                    "got second %g, ltr %g, first %g",
                    secondRtlX,
                    ltrX,
                    firstRtlX);
}

// BD1: inline placeholders stay in the segment's CTLine as U+FFFC so neutrals and adjacent
// placeholders resolve their BiDi level against text on both sides of the placeholder
DEF_TEST(CoreTextContract_BD1_PlaceholderPreservesBidiContext, reporter) {
    if (!has_font_families({"Arial"})) {
        return;
    }
    const sk_sp<FontCollection> fonts = system_collection();
    const TextStyle style = contract_text_style("Arial", 20.0f);
    const PlaceholderStyle phStyle(
            10.0f, 20.0f, PlaceholderAlignment::kBaseline, TextBaseline::kAlphabetic, 0.0f);

    // LTR paragraph with RTL runs on both sides of the placeholder: UAX #9 N1 resolves both the
    // placeholder and trailing neutrals ("!?" and "(") to RTL
    for (const char* neutral : {"!?", "("}) {
        skiatest::ReporterContext context(reporter, SkStringPrintf("LTR neutral '%s'", neutral));
        std::unique_ptr<ParagraphBuilderImpl> builder =
                contract_builder(fonts, contract_paragraph_style());
        builder->pushStyle(style);
        builder->addText("\xD7\x90\xD7\x91");
        builder->addPlaceholder(phStyle);
        const std::string after = std::string(neutral) + "\xD7\x92\xD7\x93";
        builder->addText(after.c_str(), after.size());
        auto paragraph = layout_built(reporter, builder.get(), 1000.0f);

        const size_t nLen = strlen(neutral);
        const std::optional<TextBox> firstRtl = single_tight_box(reporter, paragraph.get(), 0, 2);
        const std::optional<TextBox> neutralBox =
                single_tight_box(reporter, paragraph.get(), 3, 3 + nLen);
        const std::optional<TextBox> secondRtl =
                single_tight_box(reporter, paragraph.get(), 3 + nLen, 5 + nLen);
        const std::vector<TextBox> phs = paragraph->getRectsForPlaceholders();
        REPORTER_ASSERT(reporter, phs.size() == 1, "got %zu placeholders", phs.size());
        if (!firstRtl || !neutralBox || !secondRtl || phs.size() != 1) {
            continue;
        }
        REPORTER_ASSERT(reporter,
                        neutralBox->direction == TextDirection::kRtl &&
                                phs[0].direction == TextDirection::kRtl,
                        "neutral dir %d, placeholder dir %d",
                        static_cast<int>(neutralBox->direction),
                        static_cast<int>(phs[0].direction));
        REPORTER_ASSERT(reporter,
                        secondRtl->rect.fRight <= neutralBox->rect.fLeft + 0.5f &&
                                neutralBox->rect.fRight <= phs[0].rect.fLeft + 0.5f &&
                                phs[0].rect.fRight <= firstRtl->rect.fLeft + 0.5f,
                        "got second [%g,%g], neutral [%g,%g], ph [%g,%g], first [%g,%g]",
                        secondRtl->rect.fLeft,
                        secondRtl->rect.fRight,
                        neutralBox->rect.fLeft,
                        neutralBox->rect.fRight,
                        phs[0].rect.fLeft,
                        phs[0].rect.fRight,
                        firstRtl->rect.fLeft,
                        firstRtl->rect.fRight);
    }

    // RTL paragraph with LTR runs on both sides of two adjacent placeholders: both placeholders
    // and "!?" resolve to embedded LTR (left-to-right visual order) without coalescing U+FFFC
    ParagraphStyle rtlStyle = contract_paragraph_style();
    rtlStyle.setTextDirection(TextDirection::kRtl);
    std::unique_ptr<ParagraphBuilderImpl> builder = contract_builder(fonts, rtlStyle);
    builder->pushStyle(style);
    builder->addText("abc");
    builder->addPlaceholder(phStyle);
    builder->addPlaceholder(phStyle);
    builder->addText("!?def");
    auto rtlParagraph = layout_built(reporter, builder.get(), 1000.0f);

    const std::optional<TextBox> firstLtr = single_tight_box(reporter, rtlParagraph.get(), 0, 3);
    const std::optional<TextBox> bangBox = single_tight_box(reporter, rtlParagraph.get(), 5, 7);
    const std::optional<TextBox> secondLtr = single_tight_box(reporter, rtlParagraph.get(), 7, 10);
    const std::vector<TextBox> phs = rtlParagraph->getRectsForPlaceholders();
    REPORTER_ASSERT(reporter, phs.size() == 2, "got %zu placeholders", phs.size());
    if (firstLtr && bangBox && secondLtr && phs.size() == 2) {
        REPORTER_ASSERT(reporter,
                        bangBox->direction == TextDirection::kLtr &&
                                phs[0].direction == TextDirection::kLtr &&
                                phs[1].direction == TextDirection::kLtr,
                        "bang dir %d, ph dirs %d/%d",
                        static_cast<int>(bangBox->direction),
                        static_cast<int>(phs[0].direction),
                        static_cast<int>(phs[1].direction));
        REPORTER_ASSERT(reporter,
                        firstLtr->rect.fRight <= phs[0].rect.fLeft + 0.5f &&
                                phs[0].rect.fRight <= phs[1].rect.fLeft + 0.5f &&
                                phs[1].rect.fRight <= bangBox->rect.fLeft + 0.5f &&
                                bangBox->rect.fRight <= secondLtr->rect.fLeft + 0.5f,
                        "got first [%g,%g], ph0 [%g,%g], ph1 [%g,%g], bang [%g,%g], second [%g,%g]",
                        firstLtr->rect.fLeft,
                        firstLtr->rect.fRight,
                        phs[0].rect.fLeft,
                        phs[0].rect.fRight,
                        phs[1].rect.fLeft,
                        phs[1].rect.fRight,
                        bangBox->rect.fLeft,
                        bangBox->rect.fRight,
                        secondLtr->rect.fLeft,
                        secondLtr->rect.fRight);
    }
}
