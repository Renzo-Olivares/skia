/*
 * Copyright 2026 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "include/core/SkFontMgr.h"
#include "include/core/SkFontStyle.h"
#include "include/core/SkRefCnt.h"
#include "include/core/SkScalar.h"
#include "include/core/SkString.h"
#include "include/core/SkTypeface.h"
#include "include/core/SkTypes.h"
#include "modules/skparagraph/include/DartTypes.h"
#include "modules/skparagraph/include/FontCollection.h"
#include "modules/skparagraph/include/ParagraphStyle.h"
#include "modules/skparagraph/include/TextStyle.h"
#include "modules/skparagraph/include/TypefaceFontProvider.h"
#include "modules/skparagraph/src/ParagraphBuilderImpl.h"
#include "modules/skparagraph/src/ParagraphImpl.h"
#include "modules/skparagraph/tests/coretext/CoreTextTestUtils.h"
#include "tests/Test.h"
#include "tools/Resources.h"
#include "tools/fonts/FontToolUtils.h"

#include <algorithm>
#include <vector>

using namespace skia::textlayout;
using namespace skia::textlayout::coretext_test;

// Tests assert the internal ParagraphImpl::isCoreTextShaped(): no public API reports the engine,
// and a silent HarfBuzz fallback would pass every other assert

// CoreText omits Default_Ignorable code points without emitting glyphs, so hit-testing a preceding
// LTR or RTL glyph must still flip at its midpoint and report that visible glyph on both halves
DEF_TEST(CoreTextPipeline_HitTestSkipsDefaultIgnorables, reporter) {
    if (!has_font_families({"Courier New", "Arial Hebrew"})) {
        return;
    }
    struct Case {
        const char* family;
        const char* text;
        TextDirection direction;
        SkScalar leadingFraction;
        SkScalar trailingFraction;
    };
    auto fontCollection = sk_make_sp<FontCollection>();
    fontCollection->setDefaultFontManager(ToolUtils::TestFontMgr());
    if (sk_sp<SkTypeface> abc = ToolUtils::CreateTypefaceFromResource("fonts/abc/abc.ttf")) {
        auto provider = sk_make_sp<TypefaceFontProvider>();
        provider->registerTypeface(std::move(abc), SkString("AbcFont"));
        fontCollection->setAssetFontManager(std::move(provider));
    }
    const Case kCases[] = {
            {"Courier New", "a\u200Bb", TextDirection::kLtr, 0.4f, 0.9f},
            {"Courier New", "a\u200Eb", TextDirection::kLtr, 0.4f, 0.9f},
            {"Courier New", "a\uFEFFb", TextDirection::kLtr, 0.4f, 0.9f},
            {"Arial Hebrew", "\u05D0\u200B\u05D1", TextDirection::kRtl, 0.6f, 0.1f},
            {"Arial Hebrew", "\u05D0\u200F\u05D1", TextDirection::kRtl, 0.6f, 0.1f},
            {"AbcFont", "a\u200Bb", TextDirection::kLtr, 0.4f, 0.9f},
            {"AbcFont", "a\u200Eb", TextDirection::kLtr, 0.4f, 0.9f},
            {"AbcFont", "a\uFEFFb", TextDirection::kLtr, 0.4f, 0.9f},
    };
    int ranCases = 0;
    for (const Case& c : kCases) {
        if (fontCollection->findTypefaces({SkString(c.family)}, SkFontStyle()).empty()) {
            continue;
        }
        ++ranCases;
        ParagraphStyle paragraphStyle;
        paragraphStyle.setTextShapingEngine(TextShapingEngine::kCoreText);
        paragraphStyle.setTextDirection(c.direction);
        TextStyle style;
        style.setFontFamilies({SkString(c.family)});
        style.setFontSize(20);
        ParagraphBuilderImpl builder(paragraphStyle, fontCollection, get_unicode(paragraphStyle));
        builder.pushStyle(style);
        builder.addText(c.text);
        auto paragraph = builder.Build();
        paragraph->layout(200);

        const std::vector<TextBox> boxes =
                paragraph->getRectsForRange(0, 1, RectHeightStyle::kTight, RectWidthStyle::kTight);
        REPORTER_ASSERT(reporter, boxes.size() == 1, "%s", c.family);
        if (boxes.size() != 1) {
            continue;
        }
        const SkRect first = boxes[0].rect;
        const SkScalar leadingX = first.fLeft + first.width() * c.leadingFraction;
        const SkScalar trailingX = first.fLeft + first.width() * c.trailingFraction;
        const PositionWithAffinity leading =
                paragraph->getGlyphPositionAtCoordinate(leadingX, first.centerY());
        const PositionWithAffinity trailing =
                paragraph->getGlyphPositionAtCoordinate(trailingX, first.centerY());
        REPORTER_ASSERT(reporter,
                        leading.position == 0 && leading.affinity == Affinity::kDownstream,
                        "%s '%s': leading half gives %d",
                        c.family,
                        c.text,
                        leading.position);
        REPORTER_ASSERT(reporter,
                        trailing.position == 1 && trailing.affinity == Affinity::kUpstream,
                        "%s '%s': trailing half gives %d",
                        c.family,
                        c.text,
                        trailing.position);
        Paragraph::GlyphInfo info;
        REPORTER_ASSERT(reporter,
                        paragraph->getClosestUTF16GlyphInfoAt(trailingX, first.centerY(), &info) &&
                                info.fGraphemeClusterTextRange == TextRange(0, 1) &&
                                info.fGraphemeLayoutBounds.width() > 0,
                        "%s '%s': trailing half closest glyph is [%zu,%zu) width %g",
                        c.family,
                        c.text,
                        info.fGraphemeClusterTextRange.start,
                        info.fGraphemeClusterTextRange.end,
                        info.fGraphemeLayoutBounds.width());
    }
    REPORTER_ASSERT(reporter, ranCases >= 5, "ran %d cases", ranCases);
}

// HT1: hit-testing keeps the caret on grapheme boundaries in visual order, including at exact
// cluster edges, inside LTR and RTL ligatures, and across combining marks with their own glyph
DEF_TEST(CoreTextPipeline_GlyphPositionSnapsToGraphemes, reporter) {
    if (!has_font_families({"Helvetica", "Geeza Pro"})) {
        return;
    }
    sk_sp<SkFontMgr> mgr = ToolUtils::TestFontMgr();
    auto provider = sk_make_sp<TypefaceFontProvider>();
    for (const char* res : {"extra_fonts/GoogleSans-Regular.ttf",
                            "extra_fonts/NotoNaskhArabic-Regular.ttf",
                            "extra_fonts/Katibeh-Regular.ttf"}) {
        if (auto stream = GetResourceAsStream(res, /*useFileStream=*/true)) {
            if (sk_sp<SkTypeface> tf = mgr->makeFromStream(std::move(stream), {})) {
                provider->registerTypeface(std::move(tf));
            }
        }
    }
    auto fontCollection = sk_make_sp<FontCollection>();
    fontCollection->setAssetFontManager(std::move(provider));
    fontCollection->setDefaultFontManager(mgr);
    // Fallback stays on: disableFontFallback() drops the default manager, and Helvetica and
    // Geeza Pro then resolve to the first asset face instead of the system fonts under test

    struct Case {
        const char* family;
        const char* text;
        TextDirection direction;
        std::vector<int> fVisualCarets;
    };
    // b + U+0301, i + U+0307, beh + kasra, and alef + maddah are two code points in one grapheme;
    // Helvetica/Google Sans' fi and Geeza Pro/Katibeh's lam-alef put two graphemes in one ligature
    const Case kCases[] = {
            {"Helvetica", "ab\u0301c", TextDirection::kLtr, {0, 1, 3, 4}},
            {"Helvetica", "fi\u0307x", TextDirection::kLtr, {0, 1, 3, 4}},
            {"Geeza Pro", "\u0628\u0650\u0633", TextDirection::kRtl, {3, 2, 0}},
            {"Geeza Pro", "\u0628\u0627\u0653", TextDirection::kRtl, {3, 1, 0}},
            {"Geeza Pro", "\u0628\u0644\u0627", TextDirection::kRtl, {3, 2, 1, 0}},
            {"Google Sans", "ab\u0301c", TextDirection::kLtr, {0, 1, 3, 4}},
            {"Google Sans", "fi\u0307x", TextDirection::kLtr, {0, 1, 3, 4}},
            {"Noto Naskh Arabic", "\u0628\u0650\u0633", TextDirection::kRtl, {3, 2, 0}},
            {"Noto Naskh Arabic", "\u0628\u0627\u0653", TextDirection::kRtl, {3, 1, 0}},
            {"Katibeh", "\u0628\u0644\u0627", TextDirection::kRtl, {3, 2, 1, 0}},
    };
    int ranCases = 0;
    for (const Case& c : kCases) {
        if (fontCollection->findTypefaces({SkString(c.family)}, SkFontStyle()).empty()) {
            continue;
        }
        ++ranCases;
        ParagraphStyle paragraphStyle;
        paragraphStyle.setTextShapingEngine(TextShapingEngine::kCoreText);
        paragraphStyle.setTextDirection(c.direction);
        paragraphStyle.setTextAlign(TextAlign::kLeft);
        TextStyle textStyle;
        textStyle.setFontFamilies({SkString(c.family)});
        textStyle.setFontSize(20);
        ParagraphBuilderImpl builder(paragraphStyle, fontCollection, get_unicode(paragraphStyle));
        builder.pushStyle(textStyle);
        builder.addText(c.text);
        auto paragraph = builder.Build();
        paragraph->layout(1000);
        REPORTER_ASSERT(reporter, paragraph->unresolvedGlyphs() == 0, "%s", c.family);

        const SkScalar width = paragraph->getMaxIntrinsicWidth();
        std::vector<SkScalar> xs;
        for (SkScalar x = -1.0f; x <= width + 2; x += 0.5f) {
            xs.push_back(x);
        }
        std::vector<int> sortedCarets = c.fVisualCarets;
        std::sort(sortedCarets.begin(), sortedCarets.end());
        REPORTER_ASSERT(reporter, sortedCarets.size() >= 2, "got %zu", sortedCarets.size());
        for (size_t i = 0; i + 1 < sortedCarets.size(); ++i) {
            const std::vector<TextBox> boxes = paragraph->getRectsForRange(sortedCarets[i],
                                                                           sortedCarets[i + 1],
                                                                           RectHeightStyle::kTight,
                                                                           RectWidthStyle::kTight);
            REPORTER_ASSERT(reporter, !boxes.empty(), "%s", c.family);
            for (const TextBox& box : boxes) {
                xs.push_back(box.rect.fLeft);
                xs.push_back(box.rect.fRight);
            }
        }
        std::sort(xs.begin(), xs.end());
        REPORTER_ASSERT(reporter, !xs.empty());

        std::vector<int> visited;
        for (SkScalar x : xs) {
            const int position = paragraph->getGlyphPositionAtCoordinate(x, 10).position;
            REPORTER_ASSERT(reporter,
                            std::find(c.fVisualCarets.begin(), c.fVisualCarets.end(), position) !=
                                    c.fVisualCarets.end(),
                            "%s '%s' x=%g: caret %d inside a grapheme",
                            c.family,
                            c.text,
                            x,
                            position);
            if (visited.empty() || visited.back() != position) {
                visited.push_back(position);
            }
        }
        REPORTER_ASSERT(reporter,
                        visited == c.fVisualCarets,
                        "%s '%s': visited %zu carets (first %d), expected %zu (first %d)",
                        c.family,
                        c.text,
                        visited.size(),
                        visited.empty() ? -1 : visited.front(),
                        c.fVisualCarets.size(),
                        c.fVisualCarets.front());
    }
    REPORTER_ASSERT(reporter, ranCases >= 5, "ran %d cases", ranCases);
}

// Left of a soft-wrapped RTL line the caret lands at that line's logical end, even when the
// leftmost run continues onto the next line and runs to its right end earlier in the text
DEF_TEST(CoreTextPipeline_RtlWrappedLineLeftEdgeHitsLineEnd, reporter) {
    if (!has_font_families({"Arial Hebrew"})) {
        return;
    }
    ParagraphStyle paragraphStyle;
    paragraphStyle.setTextShapingEngine(TextShapingEngine::kCoreText);
    paragraphStyle.setTextDirection(TextDirection::kRtl);
    ParagraphBuilderImpl builder(
            paragraphStyle, test_font_collection(), get_unicode(paragraphStyle));
    TextStyle style;
    style.setFontFamilies({SkString("Arial Hebrew")});
    style.setFontSize(20);
    builder.pushStyle(style);
    // Two sizes give two runs: "shalom " stays on line 0, "olam yafe meod" wraps after "olam "
    builder.addText("\u05E9\u05DC\u05D5\u05DD ");
    style.setFontSize(24);
    builder.pushStyle(style);
    builder.addText("\u05E2\u05D5\u05DC\u05DD \u05D9\u05E4\u05D4 \u05DE\u05D0\u05D5\u05D3");
    auto paragraph = builder.Build();
    paragraph->layout(1000);
    SkScalar firstLineWidth = 0;
    for (const TextBox& box :
         paragraph->getRectsForRange(0, 9, RectHeightStyle::kTight, RectWidthStyle::kTight)) {
        firstLineWidth += box.rect.width();
    }
    paragraph->layout(firstLineWidth + 1);

    std::vector<LineMetrics> lines;
    paragraph->getLineMetrics(lines);
    REPORTER_ASSERT(reporter, lines.size() >= 2, "%zu lines", lines.size());
    if (lines.size() < 2) {
        return;
    }
    REPORTER_ASSERT(reporter,
                    lines[0].fEndExcludingWhitespaces == 9,
                    "line 0 ends at %zu",
                    lines[0].fEndExcludingWhitespaces);
    const SkScalar y = lines[0].fHeight / 2;
    const int position = paragraph->getGlyphPositionAtCoordinate(-100, y).position;
    REPORTER_ASSERT(reporter,
                    SkToSizeT(position) >= lines[0].fEndExcludingWhitespaces &&
                            SkToSizeT(position) <= lines[0].fEndIndex,
                    "left of line 0 gives %d, line ends at [%zu, %zu]",
                    position,
                    lines[0].fEndExcludingWhitespaces,
                    lines[0].fEndIndex);
}

// A cluster wider than the line after a break opportunity starts the next line, as a longer
// word does; CoreText shapes a ZWJ emoji sequence into one such cluster even when unresolved
DEF_TEST(CoreTextPipeline_LineBreak_WideClusterStartsNextLine, reporter) {
    auto fontCollection = make_ahem_collection();
    if (!fontCollection) {
        return;
    }
    const char* kFamilyEmoji =
            "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D"
            "\xF0\x9F\x91\xA6";
    const char* kHan = "\xE6\xBC\xA2";
    for (const char* wide : {kFamilyEmoji, kHan}) {
        ParagraphStyle paragraphStyle;
        paragraphStyle.setTextShapingEngine(TextShapingEngine::kCoreText);
        paragraphStyle.setApplyRoundingHack(false);
        TextStyle style;
        style.setFontFamilies({SkString("Ahem")});
        style.setFontSize(14.0f);
        ParagraphBuilderImpl builder(paragraphStyle, fontCollection, get_unicode(paragraphStyle));
        builder.pushStyle(style);
        builder.addText("1");
        style.setFontSize(wide == kHan ? 30.0f : 14.0f);
        builder.pushStyle(style);
        builder.addText(wide);
        auto paragraph = builder.Build();
        paragraph->layout(20);
        std::vector<LineMetrics> lines;
        paragraph->getLineMetrics(lines);
        REPORTER_ASSERT(reporter,
                        lines.size() >= 2 && lines[0].fEndIndex == 1 && lines[1].fStartIndex == 1,
                        "%s: %zu lines, first [%zu,%zu)",
                        wide == kHan ? "Han" : "emoji",
                        lines.size(),
                        lines.empty() ? 0 : lines[0].fStartIndex,
                        lines.empty() ? 0 : lines[0].fEndIndex);
    }
}

// Flutter lays text out again at getMaxIntrinsicWidth() and expects the same lines; spaced
// clusters (14.1 wide) make the float sum depend on the order the line breaker adds them
DEF_TEST(CoreTextPipeline_LineBreak_MaxIntrinsicWidthKeepsOneLine, reporter) {
    auto fontCollection = make_ahem_collection();
    if (!fontCollection) {
        return;
    }
    for (TextDirection direction : {TextDirection::kLtr, TextDirection::kRtl}) {
        for (SkScalar letterSpacing : {0.1f, 1.1f}) {
            auto paragraph =
                    layout_ahem(fontCollection, "View licenses", direction, letterSpacing, 1000);
            SkScalar maxIntrinsicWidth = paragraph->getMaxIntrinsicWidth();
            paragraph->layout(maxIntrinsicWidth);
            REPORTER_ASSERT(reporter,
                            paragraph->lineNumber() == 1,
                            "dir=%d ls=%g: %zu lines at width %.9g",
                            static_cast<int>(direction),
                            letterSpacing,
                            paragraph->lineNumber(),
                            maxIntrinsicWidth);
        }
    }
}

// SH4: pair kerning splits across adjacent glyphs so the caret at index 1 sits at kern/2 inside
// the kerned gap while keeping the second glyph's drawn x unchanged
DEF_TEST(CoreTextPipeline_PairKerning_CaretAtMiddleOfKernedGap, reporter) {
    auto fontCollection = test_font_collection();
    for (const char* family :
         {"Helvetica", "Avenir", ".AppleSystemUIFont", "Times New Roman", "Arial", "Futura"}) {
        if (!has_font_families({family})) {
            continue;
        }
        for (const char* text : {"AV", "To", "Wa"}) {
            for (SkScalar fontSize : {12.0f, 17.3f, 24.0f}) {
                auto paragraph = layout_single_style(fontCollection, family, text, fontSize, true);
                REPORTER_ASSERT(reporter,
                                static_cast<ParagraphImpl*>(paragraph.get())->isCoreTextShaped());
                const std::vector<PaintedGlyph> glyphs = painted_glyphs(paragraph.get());
                auto boxes = paragraph->getRectsForRange(
                        0, 1, RectHeightStyle::kTight, RectWidthStyle::kTight);
                REPORTER_ASSERT(reporter,
                                glyphs.size() == 2 && boxes.size() == 1,
                                "%s '%s' size=%g: %zu glyphs, %zu boxes",
                                family,
                                text,
                                fontSize,
                                glyphs.size(),
                                boxes.size());
                if (glyphs.size() != 2 || boxes.size() != 1) {
                    continue;
                }
                // On a two-glyph pair, line width minus nominal sum isolates the pair's kerning
                // independently of drawnX so we verify both drawnX preservation and caret split
                const SkScalar pen = glyphs[0].drawnX + glyphs[0].nominal;
                const SkScalar kern =
                        paragraph->getLongestLine() - (glyphs[0].nominal + glyphs[1].nominal);
                const SkScalar caret = boxes[0].rect.fRight;
                REPORTER_ASSERT(reporter,
                                kern < 0,
                                "%s '%s' size=%g: kerning %g",
                                family,
                                text,
                                fontSize,
                                kern);
                REPORTER_ASSERT(reporter,
                                SkScalarNearlyEqual(glyphs[1].drawnX, pen + kern, 1e-3f),
                                "%s '%s' size=%g: drawnX %.6f, expected %.6f",
                                family,
                                text,
                                fontSize,
                                glyphs[1].drawnX,
                                pen + kern);
                REPORTER_ASSERT(reporter,
                                SkScalarNearlyEqual(caret, pen + kern / 2, 1e-3f) &&
                                        pen + kern < caret && caret < pen,
                                "%s '%s' size=%g: caret %.6f, pen %.6f, kerning %.6f",
                                family,
                                text,
                                fontSize,
                                caret,
                                pen,
                                kern);
            }
        }
    }
}
