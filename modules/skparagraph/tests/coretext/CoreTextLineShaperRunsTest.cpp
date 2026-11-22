/*
 * Copyright 2026 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "include/core/SkFont.h"
#include "include/core/SkFontStyle.h"
#include "include/core/SkRefCnt.h"
#include "include/core/SkScalar.h"
#include "include/core/SkString.h"
#include "include/core/SkTypeface.h"
#include "include/core/SkTypes.h"
#include "modules/skparagraph/include/DartTypes.h"
#include "modules/skparagraph/include/FontCollection.h"
#include "modules/skparagraph/include/ParagraphCache.h"
#include "modules/skparagraph/include/ParagraphStyle.h"
#include "modules/skparagraph/include/TextStyle.h"
#include "modules/skparagraph/include/TypefaceFontProvider.h"
#include "modules/skparagraph/src/CoreTextFontRegistry.h"
#include "modules/skparagraph/src/ParagraphBuilderImpl.h"
#include "modules/skparagraph/src/ParagraphImpl.h"
#include "modules/skparagraph/tests/coretext/CoreTextTestUtils.h"
#include "tests/Test.h"
#include "tools/fonts/FontToolUtils.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

using namespace skia::textlayout;
using namespace skia::textlayout::coretext_test;

// Tests assert the internal ParagraphImpl::isCoreTextShaped(): no public API reports the engine,
// and a silent HarfBuzz fallback would pass every other assert

// Keeping the two engines apart in the cache needs both engines, and kDefault needs HarfBuzz
#if !defined(SK_PARAGRAPH_DISABLE_HARFBUZZ) && defined(SK_UNICODE_ICU_IMPLEMENTATION)
// ParagraphCache keys include the shaping engine, so a HarfBuzz layout never replays CoreText
// runs of the same text and style, or the other way round
DEF_TEST(CoreTextPipeline_CacheKeyEngineSeparation, reporter) {
    auto fontCollection = sk_make_sp<FontCollection>();
    fontCollection->setDefaultFontManager(ToolUtils::TestFontMgr());

    ParagraphCache cache;
    cache.turnOn(true);

    TextStyle textStyle;
    textStyle.setFontFamilies({SkString("Roboto")});
    textStyle.setFontSize(16.0f);

    auto makeParagraph = [&](TextShapingEngine engine) {
        ParagraphStyle paragraphStyle;
        paragraphStyle.setTextShapingEngine(engine);
        ParagraphBuilderImpl builder(paragraphStyle, fontCollection, get_unicode(paragraphStyle));
        builder.pushStyle(textStyle);
        builder.addText("Paragraph Cache Shaping Engine Test");
        builder.pop();
        auto paragraph = builder.Build();
        paragraph->layout(500.0f);
        return paragraph;
    };

    auto pCT1 = makeParagraph(TextShapingEngine::kCoreText);
    auto implCT1 = static_cast<ParagraphImpl*>(pCT1.get());
    REPORTER_ASSERT(reporter, implCT1->isCoreTextShaped());
    REPORTER_ASSERT(reporter, cache.updateParagraph(implCT1));
    REPORTER_ASSERT(reporter, cache.count() == 1, "got %d", cache.count());

    auto pHB1 = makeParagraph(TextShapingEngine::kDefault);
    auto implHB1 = static_cast<ParagraphImpl*>(pHB1.get());
    REPORTER_ASSERT(reporter, !implHB1->isCoreTextShaped());
    REPORTER_ASSERT(reporter, !cache.findParagraph(implHB1));

    REPORTER_ASSERT(reporter, cache.updateParagraph(implHB1));
    REPORTER_ASSERT(reporter, cache.count() == 2, "got %d", cache.count());

    ParagraphStyle ctQueryStyle;
    ctQueryStyle.setTextShapingEngine(TextShapingEngine::kCoreText);
    ParagraphBuilderImpl bCT(ctQueryStyle, fontCollection, get_unicode(ctQueryStyle));
    bCT.pushStyle(textStyle);
    bCT.addText("Paragraph Cache Shaping Engine Test");
    bCT.pop();
    auto pCTQuery = bCT.Build();
    auto implCTQuery = static_cast<ParagraphImpl*>(pCTQuery.get());
    REPORTER_ASSERT(reporter, !implCTQuery->isCoreTextShaped());
    REPORTER_ASSERT(reporter, cache.findParagraph(implCTQuery));
    REPORTER_ASSERT(reporter, implCTQuery->isCoreTextShaped());

    ParagraphStyle hbQueryStyle;
    hbQueryStyle.setTextShapingEngine(TextShapingEngine::kDefault);
    ParagraphBuilderImpl bHB(hbQueryStyle, fontCollection, get_unicode(hbQueryStyle));
    bHB.pushStyle(textStyle);
    bHB.addText("Paragraph Cache Shaping Engine Test");
    bHB.pop();
    auto pHBQuery = bHB.Build();
    auto implHBQuery = static_cast<ParagraphImpl*>(pHBQuery.get());
    REPORTER_ASSERT(reporter, !implHBQuery->isCoreTextShaped());
    REPORTER_ASSERT(reporter, cache.findParagraph(implHBQuery));
    REPORTER_ASSERT(reporter, !implHBQuery->isCoreTextShaped());
}
#endif  // !defined(SK_PARAGRAPH_DISABLE_HARFBUZZ) && defined(SK_UNICODE_ICU_IMPLEMENTATION)

// CoreText places trailing whitespace in an RTL CTLine at negative X coordinates
// [-CTLineGetTrailingWhitespaceWidth, 0), so visualOriginX must start there to keep the last
// glyph's fPositions[k] consistent with the fPositions[glyphCount] advance sentinel
DEF_TEST(CoreTextPipeline_RtlTrailingWhitespaceClusterWidthAndUserOnlyFontCollection, reporter) {
    if (!has_font_families({"Helvetica", "Apple Color Emoji"})) {
        return;
    }
    auto fc = test_font_collection();
    ParagraphStyle ps;
    ps.setTextShapingEngine(TextShapingEngine::kCoreText);
    ps.setTextDirection(TextDirection::kRtl);

    // "אבג" vs "אבג    " (3 Hebrew letters + 4 trailing spaces)
    const char* rtlNoSpaces = "\xD7\x90\xD7\x91\xD7\x92";
    const char* rtlTrailingSpaces = "\xD7\x90\xD7\x91\xD7\x92    ";
    TextStyle ts;
    ts.setFontFamilies({SkString("Helvetica")});
    ts.setFontSize(14.0f);

    ParagraphBuilderImpl baseBuilder(ps, fc, get_unicode(ps));
    baseBuilder.pushStyle(ts);
    baseBuilder.addText(rtlNoSpaces);
    baseBuilder.pop();
    auto baseP = baseBuilder.Build();
    baseP->layout(500.0f);

    ParagraphBuilderImpl builder(ps, fc, get_unicode(ps));
    builder.pushStyle(ts);
    builder.addText(rtlTrailingSpaces);
    builder.pop();
    auto p = builder.Build();
    p->layout(500.0f);

    auto impl = static_cast<ParagraphImpl*>(p.get());
    REPORTER_ASSERT(reporter, impl->isCoreTextShaped());
    Paragraph::GlyphClusterInfo baseFirstCluster;
    Paragraph::GlyphClusterInfo firstCluster;
    REPORTER_ASSERT(reporter, baseP->getGlyphClusterAt(0, &baseFirstCluster));
    REPORTER_ASSERT(reporter, p->getGlyphClusterAt(0, &firstCluster));
    REPORTER_ASSERT(reporter,
                    SkScalarNearlyEqual(
                            firstCluster.fBounds.width(), baseFirstCluster.fBounds.width(), 0.01f),
                    "RTL first cluster width %f != no-trailing-spaces width %f",
                    firstCluster.fBounds.width(),
                    baseFirstCluster.fBounds.width());

    // When a client registers user fonts (AssetFontManager) without setting a defaultFontManager,
    // matchFamily must not resolve system families ahead of registered user families
    sk_sp<SkTypeface> emojiFace =
            ToolUtils::TestFontMgr()->matchFamilyStyle("Apple Color Emoji", SkFontStyle());
    REPORTER_ASSERT(reporter, emojiFace != nullptr);
    if (emojiFace) {
        auto assetProvider = sk_make_sp<TypefaceFontProvider>();
        assetProvider->registerTypeface(emojiFace, SkString("Apple Color Emoji"));
        auto userOnlyFc = sk_make_sp<FontCollection>();
        userOnlyFc->setAssetFontManager(assetProvider);

        auto matched = CoreTextFontRegistry::Instance().findTypefaces(
                *userOnlyFc,
                {SkString(".AppleSystemUIFont"), SkString("Apple Color Emoji")},
                SkFontStyle::Normal(),
                std::nullopt);
        REPORTER_ASSERT(reporter, matched.size() == 1, "matched=%zu", matched.size());
        if (matched.size() == 1) {
            SkString family;
            matched[0]->getFamilyName(&family);
            REPORTER_ASSERT(reporter,
                            family.equals("Apple Color Emoji"),
                            "expected Apple Color Emoji, got '%s'",
                            family.c_str());
        }
    }
}

namespace {
enum class ArabicSplit { kNone, kEdgingOnly, kShiftOnly, kBoth };

// "مرحبا" in 24pt Geeza Pro on an RTL line; when split is not kNone, the first three letters
// ("مرح", 6 UTF-8 bytes) and the last two ("با") carry distinct edging, baselineShift, or both
std::unique_ptr<Paragraph> layout_arabic_word(ArabicSplit split) {
    ParagraphStyle ps;
    ps.setTextShapingEngine(TextShapingEngine::kCoreText);
    ps.setTextDirection(TextDirection::kRtl);
    TextStyle baseStyle;
    baseStyle.setFontFamilies({SkString("Geeza Pro")});
    baseStyle.setFontSize(24.0f);
    baseStyle.setFontEdging(SkFont::Edging::kAntiAlias);
    baseStyle.setBaselineShift(0.0f);
    ParagraphBuilderImpl builder(ps, test_font_collection(), get_unicode(ps));
    if (split == ArabicSplit::kNone) {
        builder.pushStyle(baseStyle);
        builder.addText("\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7");
    } else {
        TextStyle style1 = baseStyle;
        if (split == ArabicSplit::kEdgingOnly || split == ArabicSplit::kBoth) {
            style1.setFontEdging(SkFont::Edging::kSubpixelAntiAlias);
        }
        if (split == ArabicSplit::kShiftOnly || split == ArabicSplit::kBoth) {
            style1.setBaselineShift(-4.0f);
        }
        builder.pushStyle(baseStyle);
        builder.addText("\xD9\x85\xD8\xB1\xD8\xAD");
        builder.pop();
        builder.pushStyle(style1);
        builder.addText("\xD8\xA8\xD8\xA7");
    }
    builder.pop();
    auto paragraph = builder.Build();
    paragraph->layout(1000.0f);
    return paragraph;
}

// Glyph IDs of the paragraph in the order they are painted from left to right, whatever the run
// order or the number of blobs
std::vector<SkGlyphID> glyphs_left_to_right(Paragraph* paragraph) {
    std::vector<std::pair<SkScalar, SkGlyphID>> drawn;
    paragraph->visit([&](int, const Paragraph::VisitorInfo* info) {
        for (int i = 0; info && i < info->count; ++i) {
            drawn.push_back({info->origin.fX + info->positions[i].fX, info->glyphs[i]});
        }
    });
    std::sort(drawn.begin(), drawn.end());
    std::vector<SkGlyphID> glyphs;
    for (const auto& [x, glyph] : drawn) {
        glyphs.push_back(glyph);
    }
    return glyphs;
}
}  // namespace

// A style boundary inside an Arabic word keeps the joining forms of the unsplit word, which holds
// only if both styles are shaped in one CTLine instead of one CTLine per style
DEF_TEST(CoreTextPipeline_AdjacentBlocks_ArabicJoinsAcrossStyles, reporter) {
    if (!has_font_families({"Geeza Pro"})) {
        return;
    }
    auto single = layout_arabic_word(ArabicSplit::kNone);
    auto split = layout_arabic_word(ArabicSplit::kBoth);
    const std::vector<SkGlyphID> singleGlyphs = glyphs_left_to_right(single.get());
    const std::vector<SkGlyphID> splitGlyphs = glyphs_left_to_right(split.get());
    REPORTER_ASSERT(reporter,
                    !singleGlyphs.empty() && splitGlyphs.size() == singleGlyphs.size(),
                    "got %zu glyphs, want %zu",
                    splitGlyphs.size(),
                    singleGlyphs.size());
    for (size_t i = 0; i < std::min(splitGlyphs.size(), singleGlyphs.size()); ++i) {
        REPORTER_ASSERT(reporter,
                        splitGlyphs[i] == singleGlyphs[i],
                        "glyph %zu: got %u, want %u",
                        i,
                        splitGlyphs[i],
                        singleGlyphs[i]);
    }
    REPORTER_ASSERT(reporter,
                    SkScalarNearlyEqual(
                            split->getMaxIntrinsicWidth(), single->getMaxIntrinsicWidth(), 0.05f),
                    "got width %g, want %g",
                    split->getMaxIntrinsicWidth(),
                    single->getMaxIntrinsicWidth());
}

// Edging and baselineShift are run properties: a style boundary that changes them splits the run
// even inside one CTLine, or one half would paint with the other half's edging or baseline
DEF_TEST(CoreTextPipeline_AdjacentBlocks_EdgingAndShiftSplitRuns, reporter) {
    if (!has_font_families({"Geeza Pro"})) {
        return;
    }
    auto single = layout_arabic_word(ArabicSplit::kNone);
    const size_t singleRuns = single->getFonts().size();
    REPORTER_ASSERT(reporter, singleRuns == 1, "got %zu runs without a style split", singleRuns);

    // Changing only fontEdging at baselineShift=0 must split runs and paint each with its edging
    {
        skiatest::ReporterContext ctx(reporter, SkString("edging only"));
        auto edgingSplit = layout_arabic_word(ArabicSplit::kEdgingOnly);
        const std::vector<Paragraph::FontInfo> fonts = edgingSplit->getFonts();
        REPORTER_ASSERT(reporter, fonts.size() == 2, "got %zu runs, want 2", fonts.size());
        if (fonts.size() == 2) {
            REPORTER_ASSERT(reporter,
                            fonts[0].fTextRange == TextRange(0, 6),
                            "got run 0 [%zu, %zu), want [0, 6)",
                            fonts[0].fTextRange.start,
                            fonts[0].fTextRange.end);
            REPORTER_ASSERT(reporter,
                            fonts[0].fFont.getEdging() == SkFont::Edging::kAntiAlias,
                            "got edging %d",
                            static_cast<int>(fonts[0].fFont.getEdging()));
            REPORTER_ASSERT(reporter,
                            fonts[1].fFont.getEdging() == SkFont::Edging::kSubpixelAntiAlias,
                            "got edging %d",
                            static_cast<int>(fonts[1].fFont.getEdging()));
            int aaRuns = 0;
            int subpixelRuns = 0;
            edgingSplit->visit([&](int, const Paragraph::VisitorInfo* info) {
                if (!info || info->count == 0) {
                    return;
                }
                if (info->utf8Starts[0] < 6) {
                    aaRuns += info->font.getEdging() == SkFont::Edging::kAntiAlias;
                } else {
                    subpixelRuns += info->font.getEdging() == SkFont::Edging::kSubpixelAntiAlias;
                }
            });
            REPORTER_ASSERT(reporter,
                            aaRuns == 1 && subpixelRuns == 1,
                            "got %d AA and %d subpixel painted runs, want 1 and 1",
                            aaRuns,
                            subpixelRuns);
        }
    }

    // Changing only baselineShift at equal fontEdging and fontSize must split runs and shift y
    {
        skiatest::ReporterContext ctx(reporter, SkString("baselineShift only"));
        auto shiftSplit = layout_arabic_word(ArabicSplit::kShiftOnly);
        const std::vector<Paragraph::FontInfo> fonts = shiftSplit->getFonts();
        REPORTER_ASSERT(reporter, fonts.size() == 2, "got %zu runs, want 2", fonts.size());
        if (fonts.size() == 2) {
            REPORTER_ASSERT(reporter,
                            fonts[0].fTextRange == TextRange(0, 6),
                            "got run 0 [%zu, %zu), want [0, 6)",
                            fonts[0].fTextRange.start,
                            fonts[0].fTextRange.end);
            // VisitorInfo::origin.fY is the painted baseline, baselineShift included
            SkScalar plainY = SK_ScalarNaN;
            SkScalar shiftedY = SK_ScalarNaN;
            shiftSplit->visit([&](int, const Paragraph::VisitorInfo* info) {
                if (info && info->count > 0) {
                    (info->utf8Starts[0] < 6 ? plainY : shiftedY) = info->origin.fY;
                }
            });
            REPORTER_ASSERT(reporter,
                            shiftedY - plainY == -4.0f,
                            "got shift %g, want -4",
                            shiftedY - plainY);
        }
    }
}

// A space that ends a style block in the middle of an RTL line is not line-trailing whitespace:
// demoting it to level 0 would swap the visual order of the two Hebrew words around it
DEF_TEST(CoreTextPipeline_AdjacentBlocks_RtlMidLineSpaceKeepsWordOrder, reporter) {
    if (!has_font_families({".AppleSystemUIFont"})) {
        return;
    }
    ParagraphStyle ps;
    ps.setTextShapingEngine(TextShapingEngine::kCoreText);
    ps.setTextDirection(TextDirection::kRtl);
    TextStyle style0;
    style0.setFontFamilies({SkString(".AppleSystemUIFont")});
    style0.setFontSize(20.0f);
    style0.setFontEdging(SkFont::Edging::kAlias);
    TextStyle style1 = style0;
    style1.setFontEdging(SkFont::Edging::kAntiAlias);

    // "שלום " (first word, visually right) + "עולם" (second word, visually left)
    ParagraphBuilderImpl builder(ps, test_font_collection(), get_unicode(ps));
    builder.pushStyle(style0);
    builder.addText("\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D ");
    builder.pop();
    builder.pushStyle(style1);
    builder.addText("\xD7\xA2\xD7\x95\xD7\x9C\xD7\x9D");
    builder.pop();
    auto paragraph = builder.Build();
    paragraph->layout(1000.0f);

    auto boxesWord0 =
            paragraph->getRectsForRange(0, 4, RectHeightStyle::kTight, RectWidthStyle::kTight);
    auto boxesWord1 =
            paragraph->getRectsForRange(5, 9, RectHeightStyle::kTight, RectWidthStyle::kTight);
    REPORTER_ASSERT(reporter,
                    !boxesWord0.empty() && !boxesWord1.empty(),
                    "got %zu and %zu boxes",
                    boxesWord0.size(),
                    boxesWord1.size());
    if (boxesWord0.empty() || boxesWord1.empty()) {
        return;
    }
    REPORTER_ASSERT(reporter,
                    boxesWord0[0].rect.fLeft > boxesWord1[0].rect.fLeft,
                    "got word 0 left %g, word 1 left %g",
                    boxesWord0[0].rect.fLeft,
                    boxesWord1[0].rect.fLeft);
}

// Blocks that differ only in color share one run: paint attributes never reach CoreText, so a
// split there would only cost shaping context and extra blobs
DEF_TEST(CoreTextPipeline_AdjacentBlocks_PaintOnlyStylesShareRun, reporter) {
    if (!has_font_families({"Helvetica"})) {
        return;
    }
    ParagraphStyle ps;
    ps.setTextShapingEngine(TextShapingEngine::kCoreText);
    TextStyle redStyle;
    redStyle.setFontFamilies({SkString("Helvetica")});
    redStyle.setFontSize(20.0f);
    redStyle.setColor(SK_ColorRED);
    TextStyle blueStyle = redStyle;
    blueStyle.setColor(SK_ColorBLUE);

    ParagraphBuilderImpl builder(ps, test_font_collection(), get_unicode(ps));
    builder.pushStyle(redStyle);
    builder.addText("Hello ");
    builder.pop();
    builder.pushStyle(blueStyle);
    builder.addText("World");
    builder.pop();
    auto paragraph = builder.Build();
    paragraph->layout(1000.0f);
    const size_t runs = paragraph->getFonts().size();
    REPORTER_ASSERT(reporter, runs == 1, "got %zu runs", runs);
}

// Two user faces that both miss a code point keep one run each with fallback disabled, even
// though both fall back to LastResort: the run keeps the face the style asked for
DEF_TEST(CoreTextPipeline_AdjacentBlocks_DisabledFallbackFamiliesStayApart, reporter) {
    sk_sp<SkTypeface> roboto = ToolUtils::CreateTypefaceFromResource("fonts/Roboto-Regular.ttf");
    sk_sp<SkTypeface> dejaVu = ToolUtils::CreateTypefaceFromResource("fonts/DejaVuSans.subset.ttf");
    if (!roboto || !dejaVu) {
        return;
    }
    auto fc = test_font_collection();
    auto provider = sk_make_sp<TypefaceFontProvider>();
    provider->registerTypeface(roboto, SkString("FaceA"));
    provider->registerTypeface(dejaVu, SkString("FaceB"));
    fc->setDynamicFontManager(provider);
    // With fallback disabled installed families are hidden, so both faces are user faces
    fc->disableFontFallback();
    SkString famA;
    SkString famB;
    roboto->getFamilyName(&famA);
    dejaVu->getFamilyName(&famB);

    ParagraphStyle ps;
    ps.setTextShapingEngine(TextShapingEngine::kCoreText);
    TextStyle styleA;
    styleA.setFontFamilies({SkString("FaceA")});
    styleA.setFontSize(20.0f);
    TextStyle styleB = styleA;
    styleB.setFontFamilies({SkString("FaceB")});
    ParagraphBuilderImpl builder(ps, fc, get_unicode(ps));
    builder.pushStyle(styleA);
    builder.addText("\xF4\x8F\xBF\xBF");
    builder.pop();
    builder.pushStyle(styleB);
    builder.addText("\xF4\x8F\xBF\xBF");
    builder.pop();
    auto paragraph = builder.Build();
    paragraph->layout(1000.0f);

    REPORTER_ASSERT(reporter,
                    paragraph->unresolvedGlyphs() == 2,
                    "got %d unresolved glyphs",
                    paragraph->unresolvedGlyphs());
    const std::vector<Paragraph::FontInfo> fonts = paragraph->getFonts();
    REPORTER_ASSERT(reporter, fonts.size() == 2, "got %zu runs", fonts.size());
    if (fonts.size() != 2) {
        return;
    }
    SkString fam0;
    SkString fam1;
    fonts[0].fFont.getTypeface()->getFamilyName(&fam0);
    fonts[1].fFont.getTypeface()->getFamilyName(&fam1);
    REPORTER_ASSERT(
            reporter, fam0 == famA, "run 0: got '%s', want '%s'", fam0.c_str(), famA.c_str());
    REPORTER_ASSERT(
            reporter, fam1 == famB, "run 1: got '%s', want '%s'", fam1.c_str(), famB.c_str());
}
