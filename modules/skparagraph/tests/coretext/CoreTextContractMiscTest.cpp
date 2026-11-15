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
#include "include/core/SkFont.h"
#include "include/core/SkFontMetrics.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkRefCnt.h"
#include "include/core/SkScalar.h"
#include "include/core/SkString.h"
#include "include/core/SkTypeface.h"
#include "include/core/SkTypes.h"
#include "include/ports/SkTypeface_mac.h"
#include "modules/skparagraph/include/DartTypes.h"
#include "modules/skparagraph/include/FontCollection.h"
#include "modules/skparagraph/include/Paragraph.h"
#include "modules/skparagraph/include/ParagraphPainter.h"
#include "modules/skparagraph/include/ParagraphStyle.h"
#include "modules/skparagraph/include/TextStyle.h"
#include "modules/skparagraph/src/ParagraphBuilderImpl.h"
#include "modules/skparagraph/tests/coretext/CoreTextTestUtils.h"
#include "tests/Test.h"
#include "tools/fonts/FontToolUtils.h"

#include <CoreText/CoreText.h>

#include <algorithm>
#include <array>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

using namespace skia::textlayout;
using namespace skia::textlayout::coretext_test;

namespace {
// What a layout exposes to TextPainter: drawn glyphs, line metrics and the paragraph size
struct SplitLayoutResult {
    std::vector<DrawnGlyph> glyphs;
    std::vector<LineMetrics> lines;
    SkScalar maxIntrinsic;
    SkScalar height;
};

// "Hello AV fi world again" in Roboto 20pt at width 120, with middle as the style of
// "AV fi wo", so kerning, a ligature and a line break cross the style boundary
SplitLayoutResult layout_with_middle(skiatest::Reporter* reporter,
                                     const sk_sp<FontCollection>& fontCollection,
                                     const TextStyle& middle) {
    std::unique_ptr<ParagraphBuilderImpl> builder =
            contract_builder(fontCollection, contract_paragraph_style());
    builder->pushStyle(contract_text_style("Roboto", 20.0f));
    builder->addText("Hello ");
    builder->pushStyle(middle);
    builder->addText("AV fi wo");
    builder->pop();
    builder->addText("rld again");
    auto paragraph = layout_built(reporter, builder.get(), 120.0f);
    return {drawn_glyphs(paragraph.get()),
            line_metrics(paragraph.get()),
            paragraph->getMaxIntrinsicWidth(),
            paragraph->getHeight()};
}

// Decoration rects in paragraph coordinates; solid underline and strikethrough reach the painter
// as drawFilledRect, every other draw is ignored
class DecorationRecorder final : public ParagraphPainter {
public:
    std::vector<SkRect> fFilledRects;

    void drawTextBlob(const sk_sp<SkTextBlob>&, SkScalar, SkScalar, const SkPaintOrID&) override {}
    void drawTextShadow(const sk_sp<SkTextBlob>&, SkScalar, SkScalar, SkColor, SkScalar) override {}
    void drawRect(const SkRect&, const SkPaintOrID&) override {}
    void drawFilledRect(const SkRect& rect, const DecorationStyle&) override {
        fFilledRects.push_back(rect.makeOffset(fOffset));
    }
    void drawPath(const SkPath&, const DecorationStyle&) override {}
    void drawLine(SkScalar, SkScalar, SkScalar, SkScalar, const DecorationStyle&) override {}
    void clipRect(const SkRect&) override {}
    void translate(SkScalar dx, SkScalar dy) override { fOffset += {dx, dy}; }
    void save() override { fSaved.push_back(fOffset); }
    void restore() override {
        fOffset = fSaved.back();
        fSaved.pop_back();
    }

private:
    SkVector fOffset = {0, 0};
    std::vector<SkVector> fSaved;
};
}  // namespace

// PH1: a placeholder takes exactly its width between the text around it, getRectsForPlaceholders
// returns boxes in insertion order even when BiDi reverses them, and the U+FFFC box is the same
DEF_TEST(CoreTextContract_PH1_PlaceholderBoxes, reporter) {
    sk_sp<FontCollection> fontCollection = asset_collection({{"fonts/ahem.ttf", "Ahem"}});
    if (!fontCollection) {
        return;
    }
    const PlaceholderStyle wide(
            30.0f, 20.0f, PlaceholderAlignment::kBaseline, TextBaseline::kAlphabetic, 0.0f);
    {
        std::unique_ptr<ParagraphBuilderImpl> builder =
                contract_builder(fontCollection, contract_paragraph_style());
        builder->pushStyle(contract_text_style("Ahem", 14.0f));
        // "Prefix " [0,7), placeholder 7, " Suffix" [8,15)
        builder->addText("Prefix ");
        builder->addPlaceholder(wide);
        builder->addText(" Suffix");
        auto paragraph = layout_built(reporter, builder.get(), 1000.0f);
        REPORTER_ASSERT(reporter,
                        paragraph->unresolvedGlyphs() == 0,
                        "got %d",
                        paragraph->unresolvedGlyphs());
        auto placeholders = paragraph->getRectsForPlaceholders();
        auto prefix =
                paragraph->getRectsForRange(0, 7, RectHeightStyle::kTight, RectWidthStyle::kTight);
        auto replacement =
                paragraph->getRectsForRange(7, 8, RectHeightStyle::kTight, RectWidthStyle::kTight);
        auto suffix =
                paragraph->getRectsForRange(8, 9, RectHeightStyle::kTight, RectWidthStyle::kTight);
        REPORTER_ASSERT(reporter,
                        placeholders.size() == 1 && prefix.size() == 1 && replacement.size() == 1 &&
                                suffix.size() == 1,
                        "got %zu placeholders, %zu/%zu/%zu boxes",
                        placeholders.size(),
                        prefix.size(),
                        replacement.size(),
                        suffix.size());
        if (placeholders.size() == 1 && prefix.size() == 1 && replacement.size() == 1 &&
            suffix.size() == 1) {
            const SkRect& box = placeholders[0].rect;
            REPORTER_ASSERT(reporter,
                            box.width() == 30 && box.height() == 20,
                            "got %g x %g",
                            box.width(),
                            box.height());
            REPORTER_ASSERT(
                    reporter,
                    box.fLeft == prefix[0].rect.fRight && suffix[0].rect.fLeft == box.fRight,
                    "placeholder [%g, %g] between %g and %g",
                    box.fLeft,
                    box.fRight,
                    prefix[0].rect.fRight,
                    suffix[0].rect.fLeft);
            REPORTER_ASSERT(reporter, replacement[0].rect == box);
        }
    }

    ParagraphStyle rtl = contract_paragraph_style();
    rtl.setTextDirection(TextDirection::kRtl);
    std::unique_ptr<ParagraphBuilderImpl> builder = contract_builder(fontCollection, rtl);
    builder->pushStyle(contract_text_style("Ahem", 14.0f));
    // Neutral spaces and U+FFFC in an RTL paragraph: the first placeholder (1) is drawn right of
    // the second (3)
    builder->addText(" ");
    builder->addPlaceholder(wide);
    builder->addText(" ");
    builder->addPlaceholder(PlaceholderStyle(
            40.0f, 20.0f, PlaceholderAlignment::kBaseline, TextBaseline::kAlphabetic, 0.0f));
    builder->addText(" ");
    auto paragraph = layout_built(reporter, builder.get(), 1000.0f);
    auto placeholders = paragraph->getRectsForPlaceholders();
    REPORTER_ASSERT(reporter, placeholders.size() == 2, "got %zu", placeholders.size());
    if (placeholders.size() != 2) {
        return;
    }
    const SkRect& first = placeholders[0].rect;
    const SkRect& second = placeholders[1].rect;
    REPORTER_ASSERT(reporter,
                    first.width() == 30 && second.width() == 40,
                    "got widths %g, %g",
                    first.width(),
                    second.width());
    REPORTER_ASSERT(reporter,
                    second.fRight <= first.fLeft,
                    "first at %g, second ends at %g",
                    first.fLeft,
                    second.fRight);
    for (auto [offset, index] : {std::pair<unsigned, size_t>{1, 0}, {3, 1}}) {
        auto replacement = paragraph->getRectsForRange(
                offset, offset + 1, RectHeightStyle::kTight, RectWidthStyle::kTight);
        REPORTER_ASSERT(reporter,
                        replacement.size() == 1 && replacement[0].rect == placeholders[index].rect,
                        "offset %u: %zu boxes",
                        offset,
                        replacement.size());
    }
}

// PH1 / R130: appendPlaceholderRun honors explicit TextStyle::setTypeface without colliding in
// ParagraphCache, and falls back to the paragraph's RTL base direction when fBidiRegions is empty
DEF_TEST(CoreTextContract_PH1_ExplicitTypefaceBaseline, reporter) {
    sk_sp<FontCollection> fontCollection =
            asset_collection({{"fonts/Roboto-Regular.ttf", "Roboto"}});
    sk_sp<SkTypeface> ahem = ToolUtils::CreateTypefaceFromResource("fonts/ahem.ttf");
    if (!fontCollection || !ahem) {
        return;
    }
    const PlaceholderStyle phStyle(
            15.0f, 10.0f, PlaceholderAlignment::kMiddle, TextBaseline::kAlphabetic, 0.0f);
    TextStyle noExplicit;
    noExplicit.setFontSize(30.0f);
    noExplicit.setFontFamilies({});
    {
        std::unique_ptr<ParagraphBuilderImpl> warmBuilder =
                contract_builder(fontCollection, contract_paragraph_style());
        warmBuilder->pushStyle(noExplicit);
        warmBuilder->addText("a");
        warmBuilder->addPlaceholder(phStyle);
        (void)layout_built(reporter, warmBuilder.get(), 1000.0f);
    }

    TextStyle explicitFace = noExplicit;
    explicitFace.setTypeface(ahem);
    std::unique_ptr<ParagraphBuilderImpl> builder =
            contract_builder(fontCollection, contract_paragraph_style());
    builder->pushStyle(explicitFace);
    builder->addText("a");
    builder->addPlaceholder(phStyle);
    auto paragraph = layout_built(reporter, builder.get(), 1000.0f);

    // Text runs carry a CTFont round-trip of the face (new uniqueID), so offset 0 checks the
    // family; the placeholder (offset 1) returns the TextStyle typeface itself
    SkString textFamily;
    const SkFont textFont = paragraph->getFontAtUTF16Offset(0);
    if (textFont.getTypeface()) {
        textFont.getTypeface()->getFamilyName(&textFamily);
    }
    REPORTER_ASSERT(reporter, textFamily.equals("Ahem"), "got family '%s'", textFamily.c_str());
    const SkFont placeholderFont = paragraph->getFontAtUTF16Offset(1);
    REPORTER_ASSERT(reporter,
                    placeholderFont.getTypeface() == ahem.get(),
                    "got typeface %u, want %u",
                    placeholderFont.getTypeface() ? placeholderFont.getTypeface()->uniqueID() : 0,
                    ahem->uniqueID());

    ParagraphStyle rtl = contract_paragraph_style();
    rtl.setTextDirection(TextDirection::kRtl);
    std::unique_ptr<ParagraphBuilderImpl> rtlBuilder = contract_builder(fontCollection, rtl);
    rtlBuilder->pushStyle(explicitFace);
    rtlBuilder->addText(" ");
    rtlBuilder->addPlaceholder(PlaceholderStyle(
            30.0f, 20.0f, PlaceholderAlignment::kBaseline, TextBaseline::kAlphabetic, 0.0f));
    rtlBuilder->addPlaceholder(PlaceholderStyle(
            40.0f, 20.0f, PlaceholderAlignment::kBaseline, TextBaseline::kAlphabetic, 0.0f));
    rtlBuilder->addText("\xFF");
    auto rtlParagraph = layout_built(reporter, rtlBuilder.get(), 1000.0f);
    const std::vector<TextBox> placeholders = rtlParagraph->getRectsForPlaceholders();
    REPORTER_ASSERT(reporter, placeholders.size() == 2, "got %zu", placeholders.size());
    if (placeholders.size() == 2) {
        REPORTER_ASSERT(reporter, placeholders[0].direction == TextDirection::kRtl);
        REPORTER_ASSERT(reporter,
                        placeholders[1].rect.fRight <= placeholders[0].rect.fLeft,
                        "first at %g, second ends at %g",
                        placeholders[0].rect.fLeft,
                        placeholders[1].rect.fRight);
    }
}

// PH2: every PlaceholderAlignment puts the placeholder where Flutter defines it against the line
// metrics, and a relayout at another width recomputes it from the new line
DEF_TEST(CoreTextContract_PH2_AlignmentsAndRelayout, reporter) {
    sk_sp<FontCollection> fontCollection = asset_collection({{"fonts/ahem.ttf", "Ahem"}});
    if (!fontCollection) {
        return;
    }
    const PlaceholderAlignment kAlignments[] = {PlaceholderAlignment::kBaseline,
                                                PlaceholderAlignment::kAboveBaseline,
                                                PlaceholderAlignment::kBelowBaseline,
                                                PlaceholderAlignment::kTop,
                                                PlaceholderAlignment::kBottom,
                                                PlaceholderAlignment::kMiddle};
    constexpr SkScalar kHeight = 30;
    constexpr SkScalar kBaselineOffset = 25;
    for (PlaceholderAlignment alignment : kAlignments) {
        skiatest::ReporterContext ctx(reporter,
                                      SkStringPrintf("alignment %d", static_cast<int>(alignment)));
        std::unique_ptr<ParagraphBuilderImpl> builder =
                contract_builder(fontCollection, contract_paragraph_style());
        builder->pushStyle(contract_text_style("Ahem", 14.0f));
        builder->addText("a");
        builder->addPlaceholder(PlaceholderStyle(
                20.0f, kHeight, alignment, TextBaseline::kAlphabetic, kBaselineOffset));
        builder->addText("a");
        auto paragraph = layout_built(reporter, builder.get(), 1000.0f);
        LineMetrics line;
        auto placeholders = paragraph->getRectsForPlaceholders();
        REPORTER_ASSERT(reporter,
                        paragraph->getLineMetricsAt(0, &line) && placeholders.size() == 1,
                        "got %zu placeholders",
                        placeholders.size());
        if (placeholders.size() != 1) {
            continue;
        }
        const SkRect& box = placeholders[0].rect;
        const SkScalar baseline = line.fBaseline;
        const SkScalar top = baseline - line.fAscent;
        const SkScalar bottom = baseline + line.fDescent;
        SkScalar want = 0;
        switch (alignment) {
            case PlaceholderAlignment::kBaseline:
                want = baseline - kBaselineOffset;
                break;
            case PlaceholderAlignment::kAboveBaseline:
                want = baseline - kHeight;
                break;
            case PlaceholderAlignment::kBelowBaseline:
                want = baseline;
                break;
            case PlaceholderAlignment::kTop:
                want = top;
                break;
            case PlaceholderAlignment::kBottom:
                want = bottom - kHeight;
                break;
            case PlaceholderAlignment::kMiddle:
                want = (top + bottom - kHeight) / 2;
                break;
        }
        REPORTER_ASSERT(reporter,
                        SkScalarNearlyEqual(box.fTop, want, 0.01f) && box.height() == kHeight,
                        "got top %g height %g, want top %g (line %g..%g, baseline %g)",
                        box.fTop,
                        box.height(),
                        want,
                        top,
                        bottom,
                        baseline);
    }

    // kMiddle centers on the 14px text around the line's baseline. At 1000 the 40px "A" shares
    // the placeholder's line and pushes the baseline down; at 60 it wraps and the baseline rises
    std::unique_ptr<ParagraphBuilderImpl> builder =
            contract_builder(fontCollection, contract_paragraph_style());
    builder->pushStyle(contract_text_style("Ahem", 14.0f));
    builder->addText("a");
    builder->addPlaceholder(PlaceholderStyle(
            20.0f, 6.0f, PlaceholderAlignment::kMiddle, TextBaseline::kAlphabetic, 0.0f));
    builder->addText(" ");
    builder->pushStyle(contract_text_style("Ahem", 40.0f));
    builder->addText("A");
    auto paragraph = layout_built(reporter, builder.get(), 1000.0f);
    SkFontMetrics text;
    paragraph->getFontAt(0).getMetrics(&text);
    std::array<SkScalar, 2> tops = {};
    for (size_t i = 0; i < 2; ++i) {
        if (i == 1) {
            paragraph->layout(60.0f);
        }
        LineMetrics line;
        auto placeholders = paragraph->getRectsForPlaceholders();
        REPORTER_ASSERT(reporter,
                        paragraph->getLineMetricsAt(0, &line) && placeholders.size() == 1,
                        "layout %zu: got %zu placeholders",
                        i,
                        placeholders.size());
        if (placeholders.size() != 1) {
            return;
        }
        const SkScalar middle =
                static_cast<SkScalar>(line.fBaseline) + (text.fAscent + text.fDescent) / 2;
        tops[i] = placeholders[0].rect.fTop;
        REPORTER_ASSERT(reporter,
                        SkScalarNearlyEqual(placeholders[0].rect.centerY(), middle, 0.01f),
                        "layout %zu: center %g, text middle %g",
                        i,
                        placeholders[0].rect.centerY(),
                        middle);
    }
    REPORTER_ASSERT(reporter, tops[0] != tops[1], "top stays %g", tops[0]);
}

// PH3: a placeholder cut off by maxLines or by the ellipsis is left out of
// getRectsForPlaceholders, not returned as an empty box that Flutter would lay a widget into
DEF_TEST(CoreTextContract_PH3_TruncatedPlaceholderOmitted, reporter) {
    sk_sp<FontCollection> fontCollection = asset_collection({{"fonts/ahem.ttf", "Ahem"}});
    if (!fontCollection) {
        return;
    }
    struct Case {
        const char* before;
        const char* after;
        bool ellipsis;
    };
    // 70px holds five 14px squares: the placeholder goes to line 2 after the soft break, or
    // past the ellipsis that ends "aaaa" on line 1
    const Case kCases[] = {{"aaaa ", "", false}, {"aaaa", "aa", true}};
    for (const Case& c : kCases) {
        for (bool truncated : {false, true}) {
            skiatest::ReporterContext ctx(
                    reporter, SkStringPrintf("ellipsis %d truncated %d", c.ellipsis, truncated));
            ParagraphStyle paragraphStyle = contract_paragraph_style();
            if (truncated) {
                paragraphStyle.setMaxLines(1);
                if (c.ellipsis) {
                    paragraphStyle.setEllipsis(SkString("."));
                }
            }
            std::unique_ptr<ParagraphBuilderImpl> builder =
                    contract_builder(fontCollection, paragraphStyle);
            builder->pushStyle(contract_text_style("Ahem", 14.0f));
            builder->addText(c.before);
            builder->addPlaceholder(PlaceholderStyle(20.0f,
                                                     10.0f,
                                                     PlaceholderAlignment::kBaseline,
                                                     TextBaseline::kAlphabetic,
                                                     10.0f));
            builder->addText(c.after);
            auto paragraph = layout_built(reporter, builder.get(), 70.0f);
            const size_t count = paragraph->getRectsForPlaceholders().size();
            REPORTER_ASSERT(reporter, count == (truncated ? 0u : 1u), "got %zu", count);
        }
    }
}

// DC1: underline uses the font's underline position and thickness, while strikethrough matches
// native Apple TextKit: centered at CTFont xHeight / 2 above baseline with thickness
// 0.044 * (ascent + descent + leading), scaled by decorationThickness
DEF_TEST(CoreTextContract_DC1_DecorationFromFontMetrics, reporter) {
    sk_sp<FontCollection> fontCollection =
            asset_collection({{"fonts/Roboto-Regular.ttf", "Roboto"}});
    if (!fontCollection) {
        return;
    }
    for (TextDecoration decoration : {TextDecoration::kUnderline, TextDecoration::kLineThrough}) {
        for (SkScalar multiplier : {1.0f, 2.0f}) {
            skiatest::ReporterContext ctx(
                    reporter,
                    SkStringPrintf(
                            "decoration %d mult %g", static_cast<int>(decoration), multiplier));
            TextStyle style = contract_text_style("Roboto", 20.0f);
            style.setDecoration(decoration);
            style.setDecorationStyle(TextDecorationStyle::kSolid);
            style.setDecorationColor(SK_ColorBLACK);
            style.setDecorationThicknessMultiplier(multiplier);
            auto paragraph = layout_text(reporter,
                                         fontCollection,
                                         contract_paragraph_style(),
                                         style,
                                         "Hamburgefons",
                                         1000);
            const SkFont& font = paragraph->getFontAt(0);
            SkScalar position = 0;
            SkScalar thickness = 0;
            if (decoration == TextDecoration::kUnderline) {
                SkFontMetrics metrics;
                font.getMetrics(&metrics);
                // Without valid fields the decoration falls back to estimates and checks nothing
                REPORTER_ASSERT(reporter,
                                metrics.hasUnderlinePosition(&position) &&
                                        metrics.hasUnderlineThickness(&thickness));
            } else {
                CTFontRef ct = SkTypeface_GetCTFontRef(font.getTypeface());
                REPORTER_ASSERT(reporter, ct != nullptr);
                if (!ct) {
                    continue;
                }
                const SkScalar scale = font.getSize() / static_cast<SkScalar>(CTFontGetSize(ct));
                position = -static_cast<SkScalar>(CTFontGetXHeight(ct)) * 0.5f * scale;
                thickness = 0.044f *
                            static_cast<SkScalar>(CTFontGetAscent(ct) + CTFontGetDescent(ct) +
                                                  CTFontGetLeading(ct)) *
                            scale;
            }
            thickness *= multiplier;
            DecorationRecorder recorder;
            paragraph->paint(&recorder, 0, 0);
            REPORTER_ASSERT(reporter,
                            recorder.fFilledRects.size() == 1,
                            "got %zu",
                            recorder.fFilledRects.size());
            if (recorder.fFilledRects.size() != 1) {
                continue;
            }
            const SkRect& line = recorder.fFilledRects[0];
            LineMetrics lineMetrics;
            REPORTER_ASSERT(reporter, paragraph->getLineMetricsAt(0, &lineMetrics));
            const SkScalar want = static_cast<SkScalar>(lineMetrics.fBaseline) + position;
            REPORTER_ASSERT(reporter,
                            SkScalarNearlyEqual(line.centerY(), want, 0.01f) &&
                                    SkScalarNearlyEqual(line.height(), thickness, 0.01f),
                            "got y %g thickness %g, want y %g thickness %g",
                            line.centerY(),
                            line.height(),
                            want,
                            thickness);
        }
    }
}

// DC2: paint-only fields (color, paints, decoration, its color, style and thickness) keep
// glyphs, widths and line metrics, so TextPainter can swap them in paint() without layout
DEF_TEST(CoreTextContract_DC2_PaintOnlyKeepsLayout, reporter) {
    sk_sp<FontCollection> fontCollection =
            asset_collection({{"fonts/Roboto-Regular.ttf", "Roboto"}});
    if (!fontCollection) {
        return;
    }
    const TextStyle roboto = contract_text_style("Roboto", 20.0f);
    const SplitLayoutResult base = layout_with_middle(reporter, fontCollection, roboto);
    REPORTER_ASSERT(reporter, base.lines.size() >= 2, "got %zu", base.lines.size());

    // Reserved so the style references below stay valid while variants grows
    std::vector<std::pair<const char*, TextStyle>> variants;
    variants.reserve(5);
    auto add = [&](const char* name) -> TextStyle& {
        return variants.emplace_back(name, roboto).second;
    };
    add("color").setColor(SK_ColorRED);
    SkPaint stroke;
    stroke.setColor(SK_ColorBLUE);
    stroke.setStyle(SkPaint::kStroke_Style);
    stroke.setStrokeWidth(3);
    add("foreground").setForegroundPaint(stroke);
    SkPaint fill;
    fill.setColor(SK_ColorYELLOW);
    add("background").setBackgroundPaint(fill);
    TextStyle& wavy = add("wavy decoration");
    wavy.setDecoration(
            static_cast<TextDecoration>(TextDecoration::kUnderline | TextDecoration::kLineThrough));
    wavy.setDecorationStyle(TextDecorationStyle::kWavy);
    wavy.setDecorationColor(SK_ColorGREEN);
    wavy.setDecorationThicknessMultiplier(3);
    TextStyle& doubled = add("double overline");
    doubled.setDecoration(TextDecoration::kOverline);
    doubled.setDecorationStyle(TextDecorationStyle::kDouble);

    for (const auto& [name, style] : variants) {
        skiatest::ReporterContext ctx(reporter, SkString(name));
        const SplitLayoutResult changed = layout_with_middle(reporter, fontCollection, style);
        REPORTER_ASSERT(reporter,
                        changed.maxIntrinsic == base.maxIntrinsic && changed.height == base.height,
                        "got %g x %g, want %g x %g",
                        changed.maxIntrinsic,
                        changed.height,
                        base.maxIntrinsic,
                        base.height);
        REPORTER_ASSERT(reporter,
                        changed.glyphs.size() == base.glyphs.size(),
                        "got %zu glyphs, want %zu",
                        changed.glyphs.size(),
                        base.glyphs.size());
        for (size_t i = 0; i < std::min(changed.glyphs.size(), base.glyphs.size()); ++i) {
            REPORTER_ASSERT(reporter,
                            changed.glyphs[i].glyph == base.glyphs[i].glyph &&
                                    changed.glyphs[i].position == base.glyphs[i].position,
                            "glyph %zu: %d at (%g, %g), want %d at (%g, %g)",
                            i,
                            changed.glyphs[i].glyph,
                            changed.glyphs[i].position.fX,
                            changed.glyphs[i].position.fY,
                            base.glyphs[i].glyph,
                            base.glyphs[i].position.fX,
                            base.glyphs[i].position.fY);
        }
        REPORTER_ASSERT(reporter,
                        changed.lines.size() == base.lines.size(),
                        "got %zu lines",
                        changed.lines.size());
        for (size_t i = 0; i < std::min(changed.lines.size(), base.lines.size()); ++i) {
            REPORTER_ASSERT(reporter,
                            changed.lines[i].fBaseline == base.lines[i].fBaseline &&
                                    changed.lines[i].fWidth == base.lines[i].fWidth &&
                                    changed.lines[i].fHeight == base.lines[i].fHeight &&
                                    changed.lines[i].fEndIndex == base.lines[i].fEndIndex,
                            "line %zu: baseline %g width %g height %g end %zu, want %g %g %g %zu",
                            i,
                            changed.lines[i].fBaseline,
                            changed.lines[i].fWidth,
                            changed.lines[i].fHeight,
                            changed.lines[i].fEndIndex,
                            base.lines[i].fBaseline,
                            base.lines[i].fWidth,
                            base.lines[i].fHeight,
                            base.lines[i].fEndIndex);
        }
    }
}

// IN4: SkUnicode rejects malformed UTF-8, so the paragraph gets no BiDi regions or code unit
// flags; kCoreText then lays out no text, like kDefault, instead of lines without flags
DEF_TEST(CoreTextContract_IN4_MalformedUtf8DropsTextLikeDefault, reporter) {
    sk_sp<FontCollection> fontCollection =
            asset_collection({{"fonts/Roboto-Regular.ttf", "Roboto"}});
    if (!fontCollection) {
        return;
    }
    struct Case {
        const char* name;
        std::string_view text;
        bool rtl;
        SkScalar letterSpacing;
        bool strut;
        bool placeholder;
    };
    // RTL + letterSpacing + strut must not reach extendedVisit with an empty trimmed line;
    // overlong and above-U+10FFFF sequences are malformed UTF-8 too
    constexpr std::string_view kBadBytes =
            "abc\xFF\xFE"
            "def g";
    constexpr std::array<Case, 5> kCases = {{
            {"ltr", kBadBytes, false, 0, false, false},
            {"rtl ls strut", kBadBytes, true, 1, true, false},
            {"rtl placeholder", kBadBytes, true, 0, false, true},
            {"overlong", "a\xC0\xAF!", false, 0, false, false},
            {"too big", "a\xF4\x90\x80\x80!", false, 0, false, false},
    }};
    for (const Case& c : kCases) {
        skiatest::ReporterContext context(reporter, SkString(c.name));
        ParagraphStyle paragraphStyle = contract_paragraph_style();
        paragraphStyle.setTextDirection(c.rtl ? TextDirection::kRtl : TextDirection::kLtr);
        if (c.strut) {
            StrutStyle strut;
            strut.setStrutEnabled(true);
            strut.setFontFamilies({SkString("Roboto")});
            strut.setFontSize(12);
            paragraphStyle.setStrutStyle(strut);
        }
        TextStyle style = contract_text_style("Roboto", 12.0f);
        style.setLetterSpacing(c.letterSpacing);
        auto layout = [&](TextShapingEngine engine) {
            paragraphStyle.setTextShapingEngine(engine);
            ParagraphBuilderImpl builder(paragraphStyle, fontCollection, contract_unicode());
            builder.pushStyle(style);
            builder.addText(c.text.data(), c.text.size());
            if (c.placeholder) {
                builder.addPlaceholder(PlaceholderStyle(30.0f,
                                                        20.0f,
                                                        PlaceholderAlignment::kBaseline,
                                                        TextBaseline::kAlphabetic,
                                                        0.0f));
            }
            std::unique_ptr<Paragraph> paragraph = builder.Build();
            paragraph->layout(1000.0f);
            return paragraph;
        };
        std::unique_ptr<Paragraph> coreText = layout(TextShapingEngine::kCoreText);
        auto visits = [](Paragraph* paragraph) {
            int count = 0;
            paragraph->visit([&](int, const Paragraph::VisitorInfo*) { ++count; });
            paragraph->extendedVisit([&](int, const Paragraph::ExtendedVisitorInfo*) { ++count; });
            return count;
        };
        const std::vector<TextBox> textBoxes =
                coreText->getRectsForRange(0, 3, RectHeightStyle::kTight, RectWidthStyle::kTight);
        REPORTER_ASSERT(reporter, textBoxes.empty(), "got %zu", textBoxes.size());
        // A placeholder needs no text indexing, so it keeps its line like on kDefault
        if (!c.placeholder) {
            REPORTER_ASSERT(
                    reporter, coreText->lineNumber() == 0, "got %zu", coreText->lineNumber());
            REPORTER_ASSERT(reporter, visits(coreText.get()) == 0);
        }
#if !defined(SK_PARAGRAPH_DISABLE_HARFBUZZ)
        std::unique_ptr<Paragraph> harfBuzz = layout(TextShapingEngine::kDefault);
        REPORTER_ASSERT(reporter,
                        coreText->lineNumber() == harfBuzz->lineNumber(),
                        "got %zu, kDefault %zu",
                        coreText->lineNumber(),
                        harfBuzz->lineNumber());
        REPORTER_ASSERT(reporter,
                        visits(coreText.get()) == visits(harfBuzz.get()),
                        "got %d, kDefault %d",
                        visits(coreText.get()),
                        visits(harfBuzz.get()));
        const std::vector<TextBox> ctPlaceholders = coreText->getRectsForPlaceholders();
        const std::vector<TextBox> hbPlaceholders = harfBuzz->getRectsForPlaceholders();
        REPORTER_ASSERT(reporter,
                        ctPlaceholders.size() == hbPlaceholders.size(),
                        "got %zu, kDefault %zu",
                        ctPlaceholders.size(),
                        hbPlaceholders.size());
        for (size_t k = 0; k < std::min(ctPlaceholders.size(), hbPlaceholders.size()); ++k) {
            REPORTER_ASSERT(reporter,
                            ctPlaceholders[k].rect.fLeft == hbPlaceholders[k].rect.fLeft &&
                                    ctPlaceholders[k].rect.fRight == hbPlaceholders[k].rect.fRight,
                            "got [%g,%g], kDefault [%g,%g]",
                            ctPlaceholders[k].rect.fLeft,
                            ctPlaceholders[k].rect.fRight,
                            hbPlaceholders[k].rect.fLeft,
                            hbPlaceholders[k].rect.fRight);
        }
        REPORTER_ASSERT(reporter,
                        coreText->getHeight() == harfBuzz->getHeight(),
                        "got %g, kDefault %g",
                        coreText->getHeight(),
                        harfBuzz->getHeight());
        REPORTER_ASSERT(reporter,
                        coreText->getAlphabeticBaseline() == harfBuzz->getAlphabeticBaseline(),
                        "got %g, kDefault %g",
                        coreText->getAlphabeticBaseline(),
                        harfBuzz->getAlphabeticBaseline());
        REPORTER_ASSERT(reporter,
                        coreText->getLongestLine() == harfBuzz->getLongestLine(),
                        "got %g, kDefault %g",
                        coreText->getLongestLine(),
                        harfBuzz->getLongestLine());
        REPORTER_ASSERT(reporter,
                        coreText->getMaxIntrinsicWidth() == harfBuzz->getMaxIntrinsicWidth(),
                        "got %g, kDefault %g",
                        coreText->getMaxIntrinsicWidth(),
                        harfBuzz->getMaxIntrinsicWidth());
        // A placeholder's UTF-16 offset past malformed UTF-8 comes from the shared UTF-8 -> UTF-16
        // mapping, which the engines read differently, so only placeholder-free cases hit-test
        if (c.placeholder) {
            continue;
        }
        const PositionWithAffinity ctPos = coreText->getGlyphPositionAtCoordinate(5, 5);
        const PositionWithAffinity hbPos = harfBuzz->getGlyphPositionAtCoordinate(5, 5);
        REPORTER_ASSERT(reporter,
                        ctPos.position == hbPos.position && ctPos.affinity == hbPos.affinity,
                        "got %d/%d, kDefault %d/%d",
                        ctPos.position,
                        (int)ctPos.affinity,
                        hbPos.position,
                        (int)hbPos.affinity);
#endif
    }
}
