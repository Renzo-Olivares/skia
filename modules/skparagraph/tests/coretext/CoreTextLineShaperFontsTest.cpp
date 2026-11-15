/*
 * Copyright 2026 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontArguments.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkFontStyle.h"
#include "include/core/SkRefCnt.h"
#include "include/core/SkScalar.h"
#include "include/core/SkSpan.h"
#include "include/core/SkStream.h"
#include "include/core/SkString.h"
#include "include/core/SkTypeface.h"
#include "include/core/SkTypes.h"
#include "modules/skparagraph/include/DartTypes.h"
#include "modules/skparagraph/include/FontCollection.h"
#include "modules/skparagraph/include/ParagraphStyle.h"
#include "modules/skparagraph/include/TextStyle.h"
#include "modules/skparagraph/include/TypefaceFontProvider.h"
#include "modules/skparagraph/src/CoreTextFontRegistry.h"
#include "modules/skparagraph/src/ParagraphBuilderImpl.h"
#include "modules/skparagraph/src/ParagraphImpl.h"
#include "modules/skparagraph/tests/coretext/CoreTextTestUtils.h"
#include "src/core/SkUTF.h"
#include "tests/Test.h"
#include "tools/fonts/FontToolUtils.h"

#include <algorithm>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace skia::textlayout;
using namespace skia::textlayout::coretext_test;

// Tests assert the internal ParagraphImpl::isCoreTextShaped(): no public API reports the engine,
// and a silent HarfBuzz fallback would pass every other assert

// An empty family or a system alias binds the system UI font with nothing unresolved, and bold
// or italic reach it as traits; the default font of every Flutter app on Apple platforms
DEF_TEST(CoreTextPipeline_AppleSystemFonts_OpticalAndTraits, reporter) {
    auto fontCollection = sk_make_sp<FontCollection>();

    {
        ParagraphStyle paragraphStyle;
        paragraphStyle.setTextShapingEngine(TextShapingEngine::kCoreText);
        ParagraphBuilderImpl builder(paragraphStyle, fontCollection, get_unicode(paragraphStyle));
        TextStyle style;
        style.setFontSize(16.0f);
        builder.pushStyle(style);
        builder.addText("System Font Regular");
        builder.pop();

        auto paragraph = builder.Build();
        paragraph->layout(500.0f);

        REPORTER_ASSERT(reporter,
                        paragraph->unresolvedGlyphs() == 0,
                        "got %d",
                        paragraph->unresolvedGlyphs());
        REPORTER_ASSERT(reporter,
                        paragraph->getMaxIntrinsicWidth() > 0.0f,
                        "got %g",
                        paragraph->getMaxIntrinsicWidth());

        const std::vector<Paragraph::FontInfo> fonts = paragraph->getFonts();
        REPORTER_ASSERT(reporter, fonts.size() == 1, "got %zu runs", fonts.size());
        if (!fonts.empty()) {
            REPORTER_ASSERT(reporter,
                            CoreTextFontRegistry::isSystemUiFace(fonts[0].fFont.getTypeface()));
        }
    }

    {
        ParagraphStyle paragraphStyle;
        paragraphStyle.setTextShapingEngine(TextShapingEngine::kCoreText);
        ParagraphBuilderImpl builder(paragraphStyle, fontCollection, get_unicode(paragraphStyle));
        TextStyle boldStyle;
        boldStyle.setFontSize(16.0f);
        boldStyle.setFontStyle(SkFontStyle::Bold());
        builder.pushStyle(boldStyle);
        builder.addText("Bold");
        builder.pop();

        TextStyle italicStyle;
        italicStyle.setFontSize(16.0f);
        italicStyle.setFontStyle(SkFontStyle::Italic());
        builder.pushStyle(italicStyle);
        builder.addText(" Italic");
        builder.pop();

        auto paragraph = builder.Build();
        paragraph->layout(500.0f);

        REPORTER_ASSERT(reporter,
                        paragraph->unresolvedGlyphs() == 0,
                        "got %d",
                        paragraph->unresolvedGlyphs());
        REPORTER_ASSERT(reporter,
                        paragraph->getMaxIntrinsicWidth() > 0.0f,
                        "got %g",
                        paragraph->getMaxIntrinsicWidth());
        const sk_sp<SkTypeface> bold = paragraph->getFontAtUTF16Offset(0).refTypeface();
        const sk_sp<SkTypeface> italic = paragraph->getFontAtUTF16Offset(5).refTypeface();
        REPORTER_ASSERT(reporter, bold && italic);
        if (bold && italic) {
            SkString italicPs;
            italic->getPostScriptName(&italicPs);
            REPORTER_ASSERT(reporter,
                            bold->fontStyle().weight() >= 600,
                            "got weight %d",
                            bold->fontStyle().weight());
            REPORTER_ASSERT(reporter,
                            italic->fontStyle().slant() != SkFontStyle::kUpright_Slant ||
                                    italicPs.contains("Italic"),
                            "got slant %d, PostScript name %s",
                            static_cast<int>(italic->fontStyle().slant()),
                            italicPs.c_str());
        }
    }

    for (const char* alias : {".AppleSystemUIFont", "SF Pro", "system-ui", "-apple-system"}) {
        skiatest::ReporterContext ctx(reporter, SkString(alias));
        ParagraphStyle paragraphStyle;
        paragraphStyle.setTextShapingEngine(TextShapingEngine::kCoreText);
        ParagraphBuilderImpl builder(paragraphStyle, fontCollection, get_unicode(paragraphStyle));
        TextStyle style;
        style.setFontFamilies({SkString(alias)});
        style.setFontSize(14.0f);
        builder.pushStyle(style);
        builder.addText("Apple Alias Test");
        builder.pop();

        auto paragraph = builder.Build();
        paragraph->layout(500.0f);

        REPORTER_ASSERT(reporter,
                        paragraph->unresolvedGlyphs() == 0,
                        "got %d",
                        paragraph->unresolvedGlyphs());
        REPORTER_ASSERT(reporter,
                        paragraph->getMaxIntrinsicWidth() > 0.0f,
                        "got %g",
                        paragraph->getMaxIntrinsicWidth());
    }
}

// Latin, CJK, emoji and Arabic resolve through the CoreText cascade in logical order, never to
// LastResort or the default font manager; an explicit family list keeps its first family
DEF_TEST(CoreTextPipeline_AppleSystemFonts_MultiScriptFallbackChain, reporter) {
    if (!has_font_families({"Courier", "Apple Color Emoji", "PingFang SC"})) {
        return;
    }
    auto fontCollection = sk_make_sp<FontCollection>();
    fontCollection->setDefaultFontManager(ToolUtils::TestFontMgr());

    {
        ParagraphStyle paragraphStyle;
        paragraphStyle.setTextShapingEngine(TextShapingEngine::kCoreText);
        ParagraphBuilderImpl builder(paragraphStyle, fontCollection, get_unicode(paragraphStyle));
        TextStyle style;
        style.setFontSize(16.0f);
        builder.pushStyle(style);
        const char* mixedText = "Hello 世界 🎉 مرحبا";
        builder.addText(mixedText);
        builder.pop();

        auto paragraph = builder.Build();
        paragraph->layout(1000.0f);

        REPORTER_ASSERT(reporter,
                        paragraph->unresolvedGlyphs() == 0,
                        "got %d",
                        paragraph->unresolvedGlyphs());
        REPORTER_ASSERT(reporter,
                        paragraph->getMaxIntrinsicWidth() > 0.0f,
                        "got %g",
                        paragraph->getMaxIntrinsicWidth());

        // Run::script() and isCursiveScript() have no public Paragraph getter
        auto impl = static_cast<ParagraphImpl*>(paragraph.get());
        REPORTER_ASSERT(reporter, impl->runs().size() >= 4, "got %zu runs", impl->runs().size());

        for (size_t i = 1; i < impl->runs().size(); ++i) {
            REPORTER_ASSERT(
                    reporter,
                    impl->runs()[i].textRange().start >= impl->runs()[i - 1].textRange().end,
                    "run %zu starts at %zu, previous ends at %zu",
                    i,
                    impl->runs()[i].textRange().start,
                    impl->runs()[i - 1].textRange().end);
        }

        bool foundLatin = false;
        bool foundCJK = false;
        bool foundEmoji = false;
        bool foundArabic = false;

        for (const auto& run : impl->runs()) {
            skiatest::ReporterContext ctx(
                    reporter,
                    SkStringPrintf("run [%zu, %zu)", run.textRange().start, run.textRange().end));
            REPORTER_ASSERT(reporter, run.size() > 0, "got %zu glyphs", run.size());
            REPORTER_ASSERT(reporter, run.advance().fX > 0.0f, "got %g", run.advance().fX);
            REPORTER_ASSERT(reporter, run.font().getTypeface() != nullptr);

            SkString family;
            run.font().getTypeface()->getFamilyName(&family);
            SkString psName;
            run.font().getTypeface()->getPostScriptName(&psName);

            REPORTER_ASSERT(reporter, !family.contains("LastResort"));
            REPORTER_ASSERT(reporter, !psName.contains("LastResort"));

            if (family.contains("Emoji") || psName.contains("Emoji")) {
                foundEmoji = true;
                REPORTER_ASSERT(reporter, run.leftToRight());
            } else if (run.script() == SkSetFourByteTag('L', 'a', 't', 'n')) {
                foundLatin = true;
                REPORTER_ASSERT(reporter, run.leftToRight());
            } else if (run.script() == SkSetFourByteTag('H', 'a', 'n', 'i')) {
                // The space after the ideographs inherits their script but stays in the
                // primary font
                const char* first = mixedText + run.textRange().start;
                if (SkUTF::NextUTF8(&first, mixedText + run.textRange().end) == ' ') {
                    continue;
                }
                foundCJK = true;
                REPORTER_ASSERT(reporter, run.leftToRight());
                REPORTER_ASSERT(reporter,
                                family.contains("PingFang") || family.contains("Hiragino") ||
                                        family.contains("Songti") || family.contains("Heiti") ||
                                        psName.contains("PingFang") || psName.contains("Hiragino"));
            } else if (run.script() == SkSetFourByteTag('A', 'r', 'a', 'b')) {
                foundArabic = true;
                REPORTER_ASSERT(reporter, !run.leftToRight());
                REPORTER_ASSERT(reporter, run.isCursiveScript());
            }
        }

        REPORTER_ASSERT(reporter, foundLatin);
        REPORTER_ASSERT(reporter, foundCJK);
        REPORTER_ASSERT(reporter, foundEmoji);
        REPORTER_ASSERT(reporter, foundArabic);

        auto boxesLatin =
                paragraph->getRectsForRange(0, 5, RectHeightStyle::kMax, RectWidthStyle::kTight);
        REPORTER_ASSERT(reporter, !boxesLatin.empty());
        if (!boxesLatin.empty()) {
            REPORTER_ASSERT(reporter,
                            boxesLatin[0].rect.width() > 0.0f,
                            "got %g",
                            boxesLatin[0].rect.width());
        }

        auto boxesCJK =
                paragraph->getRectsForRange(6, 8, RectHeightStyle::kMax, RectWidthStyle::kTight);
        REPORTER_ASSERT(reporter, !boxesCJK.empty());
        if (!boxesCJK.empty()) {
            REPORTER_ASSERT(
                    reporter, boxesCJK[0].rect.width() > 0.0f, "got %g", boxesCJK[0].rect.width());
        }

        auto boxesEmoji =
                paragraph->getRectsForRange(9, 11, RectHeightStyle::kMax, RectWidthStyle::kTight);
        REPORTER_ASSERT(reporter, !boxesEmoji.empty());
        if (!boxesEmoji.empty()) {
            REPORTER_ASSERT(reporter,
                            boxesEmoji[0].rect.width() > 0.0f,
                            "got %g",
                            boxesEmoji[0].rect.width());
        }

        auto boxesArabic =
                paragraph->getRectsForRange(12, 17, RectHeightStyle::kMax, RectWidthStyle::kTight);
        REPORTER_ASSERT(reporter, !boxesArabic.empty());
        if (!boxesArabic.empty()) {
            REPORTER_ASSERT(reporter,
                            boxesArabic[0].rect.width() > 0.0f,
                            "got %g",
                            boxesArabic[0].rect.width());
        }

        SkBitmap bitmap;
        bitmap.allocN32Pixels(500, 100);
        SkCanvas canvas(bitmap);
        canvas.clear(SK_ColorWHITE);
        paragraph->paint(&canvas, 0, 0);
    }

    {
        ParagraphStyle paragraphStyle;
        paragraphStyle.setTextShapingEngine(TextShapingEngine::kCoreText);
        ParagraphBuilderImpl builder(paragraphStyle, fontCollection, get_unicode(paragraphStyle));
        TextStyle style;
        style.setFontSize(16.0f);
        style.setFontFamilies(
                {SkString("Courier"), SkString("Apple Color Emoji"), SkString("PingFang SC")});
        builder.pushStyle(style);
        builder.addText("Courier: 世界 🎉");
        builder.pop();

        auto paragraph = builder.Build();
        paragraph->layout(1000.0f);

        REPORTER_ASSERT(reporter,
                        paragraph->unresolvedGlyphs() == 0,
                        "got %d",
                        paragraph->unresolvedGlyphs());
        REPORTER_ASSERT(reporter,
                        paragraph->getMaxIntrinsicWidth() > 0.0f,
                        "got %g",
                        paragraph->getMaxIntrinsicWidth());

        const std::vector<Paragraph::FontInfo> fonts = paragraph->getFonts();
        REPORTER_ASSERT(reporter, fonts.size() >= 3, "got %zu runs", fonts.size());
        if (!fonts.empty()) {
            SkString family0;
            fonts[0].fFont.getTypeface()->getFamilyName(&family0);
            REPORTER_ASSERT(reporter, family0.contains("Courier"), "got '%s'", family0.c_str());
        }
    }
}

// With fallback disabled installed families stay hidden as in FontCollection, so even Latin is
// glyph 0 and only a user face under the requested name draws, as Flutter disableFontFallback
DEF_TEST(CoreTextPipeline_DisableFontFallback, reporter) {
    auto fontCollection = sk_make_sp<FontCollection>();
    fontCollection->setDefaultFontManager(ToolUtils::TestFontMgr());
    fontCollection->disableFontFallback();
    REPORTER_ASSERT(reporter, !fontCollection->fontFallbackEnabled());

    ParagraphStyle paragraphStyle;
    paragraphStyle.setTextShapingEngine(TextShapingEngine::kCoreText);
    TextStyle style;
    style.setFontFamilies({SkString("Courier")});
    style.setFontSize(16.0f);

    {
        ParagraphBuilderImpl builder(paragraphStyle, fontCollection, get_unicode(paragraphStyle));
        builder.pushStyle(style);
        const char* mixedText = "Hello 世界 🎉 مرحبا";
        builder.addText(mixedText);
        builder.pop();

        auto paragraph = builder.Build();
        paragraph->layout(1000.0f);

        REPORTER_ASSERT(reporter,
                        paragraph->unresolvedGlyphs() > 0,
                        "got %d",
                        paragraph->unresolvedGlyphs());
        auto unresolvedSet = paragraph->unresolvedCodepoints();
        REPORTER_ASSERT(reporter, !unresolvedSet.empty());
        REPORTER_ASSERT(
                reporter, unresolvedSet.count('H') == 1, "got %zu", unresolvedSet.count('H'));
        REPORTER_ASSERT(reporter,
                        CoreTextFontRegistry::Instance()
                                .findTypefaces(*fontCollection,
                                               style.getFontFamilies(),
                                               style.getFontStyle(),
                                               std::nullopt)
                                .empty());
        REPORTER_ASSERT(reporter,
                        fontCollection->findTypefaces(style.getFontFamilies(), style.getFontStyle())
                                .empty());

        auto impl = static_cast<ParagraphImpl*>(paragraph.get());
        for (const auto& run : impl->runs()) {
            if (run.glyphs().empty()) {
                continue;
            }
            if (run.glyphs()[0] == 0) {
                for (size_t i = 0; i < run.size(); ++i) {
                    REPORTER_ASSERT(reporter,
                                    run.glyphs()[i] == 0,
                                    "glyph %zu: got %d",
                                    i,
                                    run.glyphs()[i]);
                }
            }
        }
    }

    {
        ParagraphBuilderImpl builder1(paragraphStyle, fontCollection, get_unicode(paragraphStyle));
        builder1.pushStyle(style);
        builder1.addText("世界");
        builder1.pop();
        auto p1 = builder1.Build();
        p1->layout(1000.0f);

        ParagraphBuilderImpl builder2(paragraphStyle, fontCollection, get_unicode(paragraphStyle));
        builder2.pushStyle(style);
        builder2.addText("世\u200D界\u200B");
        builder2.pop();
        auto p2 = builder2.Build();
        p2->layout(1000.0f);

        REPORTER_ASSERT(reporter,
                        SkScalarNearlyEqual(p1->getMaxIntrinsicWidth(), p2->getMaxIntrinsicWidth()),
                        "got %g, want %g",
                        p2->getMaxIntrinsicWidth(),
                        p1->getMaxIntrinsicWidth());
        REPORTER_ASSERT(reporter, p2->unresolvedGlyphs() == 2, "got %d", p2->unresolvedGlyphs());
        auto unresolvedP2 = p2->unresolvedCodepoints();
        REPORTER_ASSERT(reporter, unresolvedP2.size() == 2, "got %zu", unresolvedP2.size());
        REPORTER_ASSERT(
                reporter, unresolvedP2.count(0x4E16) == 1, "got %zu", unresolvedP2.count(0x4E16));
        REPORTER_ASSERT(
                reporter, unresolvedP2.count(0x754C) == 1, "got %zu", unresolvedP2.count(0x754C));
        REPORTER_ASSERT(
                reporter, unresolvedP2.count(0x200D) == 0, "got %zu", unresolvedP2.count(0x200D));
        REPORTER_ASSERT(
                reporter, unresolvedP2.count(0x200B) == 0, "got %zu", unresolvedP2.count(0x200B));
    }

    {
        fontCollection->enableFontFallback();
        fontCollection->getParagraphCache()->reset();
        REPORTER_ASSERT(reporter, fontCollection->fontFallbackEnabled());

        ParagraphBuilderImpl builder(paragraphStyle, fontCollection, get_unicode(paragraphStyle));
        builder.pushStyle(style);
        const char* mixedText = "Hello 世界 🎉 مرحبا";
        builder.addText(mixedText);
        builder.pop();

        auto paragraph = builder.Build();
        paragraph->layout(1000.0f);

        REPORTER_ASSERT(reporter,
                        paragraph->unresolvedGlyphs() == 0,
                        "got %d",
                        paragraph->unresolvedGlyphs());
        REPORTER_ASSERT(reporter, paragraph->unresolvedCodepoints().empty());
    }

    {
        sk_sp<SkTypeface> roboto =
                ToolUtils::CreateTypefaceFromResource("fonts/Roboto-Regular.ttf");
        // Only CoreText-backed font managers load resource data, so this block runs last
        if (!roboto) {
            return;
        }
        auto provider = sk_make_sp<TypefaceFontProvider>();
        provider->registerTypeface(roboto, SkString("Courier"));
        auto userFc = sk_make_sp<FontCollection>();
        userFc->setDefaultFontManager(ToolUtils::TestFontMgr());
        userFc->setDynamicFontManager(provider);
        userFc->disableFontFallback();

        auto faces = CoreTextFontRegistry::Instance().findTypefaces(
                *userFc, style.getFontFamilies(), style.getFontStyle(), std::nullopt);
        REPORTER_ASSERT(reporter,
                        faces.size() == 1 && faces[0]->uniqueID() == roboto->uniqueID(),
                        "got %zu faces",
                        faces.size());

        ParagraphBuilderImpl builder(paragraphStyle, userFc, get_unicode(paragraphStyle));
        builder.pushStyle(style);
        builder.addText("Hello \xE4\xB8\x96");
        builder.pop();
        auto paragraph = builder.Build();
        paragraph->layout(1000.0f);
        auto unresolved = paragraph->unresolvedCodepoints();
        REPORTER_ASSERT(reporter,
                        unresolved.size() == 1 && unresolved.count(0x4E16) == 1,
                        "got %zu unresolved",
                        unresolved.size());
        SkString robotoPs;
        roboto->getPostScriptName(&robotoPs);
        auto impl = static_cast<ParagraphImpl*>(paragraph.get());
        for (const auto& run : impl->runs()) {
            // Resolved runs carry a new SkTypeface wrapping the bound CTFont, so compare faces
            // by PostScript name rather than by uniqueID
            if (!run.glyphs().empty() && run.glyphs()[0] != 0) {
                SkString runPs;
                run.font().getTypeface()->getPostScriptName(&runPs);
                REPORTER_ASSERT(reporter, runPs == robotoPs, "%s", runPs.c_str());
            }
        }
    }
}

namespace {
// Lays out "abcabcabc" at 48pt in one family, optionally pinning a single variation axis.
// The sample is restricted to glyphs that fonts/Variable.ttf actually maps
std::unique_ptr<Paragraph> layout_axis_sample(const sk_sp<FontCollection>& fontCollection,
                                              const char* family,
                                              std::optional<Coordinate> coord) {
    TextStyle style;
    style.setFontFamilies({SkString(family)});
    style.setFontSize(48.0f);
    if (coord) {
        SkFontArguments args;
        args.setVariationDesignPosition({&*coord, 1});
        style.setFontArguments(args);
    }
    ParagraphStyle paragraphStyle;
    paragraphStyle.setTextShapingEngine(TextShapingEngine::kCoreText);
    ParagraphBuilderImpl builder(paragraphStyle, fontCollection, get_unicode(paragraphStyle));
    builder.pushStyle(style);
    builder.addText("abcabcabc");
    builder.pop();
    auto paragraph = builder.Build();
    paragraph->layout(1000.0f);
    return paragraph;
}

// Typeface of the first run in logical order, or nullptr when nothing was shaped; the paragraph
// keeps the typeface alive
const SkTypeface* first_run_typeface(Paragraph* paragraph) {
    const std::vector<Paragraph::FontInfo> fonts = paragraph->getFonts();
    return fonts.empty() ? nullptr : fonts[0].fFont.getTypeface();
}

// SkTypeface_Mac surfaces CTFontCopyVariation here, so the run typeface reports the exact
// position CoreText shaped with; axes left unset report the font's default value
std::optional<float> axis_value(const SkTypeface* typeface, SkFourByteTag axis) {
    constexpr int kMaxAxes = 16;
    Coordinate coords[kMaxAxes];
    const int count = typeface ? typeface->getVariationDesignPosition(coords) : -1;
    if (count < 0 || count > kMaxAxes) {
        return std::nullopt;
    }
    for (int i = 0; i < count; ++i) {
        if (coords[i].axis == axis) {
            return coords[i].value;
        }
    }
    return std::nullopt;
}

// True when typeface reports axis within 0.5 of expected; false when the axis is missing
bool axis_near(const SkTypeface* typeface, SkFourByteTag axis, float expected) {
    std::optional<float> value = axis_value(typeface, axis);
    return value && SkScalarNearlyEqual(*value, expected, 0.5f);
}
}  // namespace

// Variation axes from TextStyle font arguments reach the CTFont and move advances; a dropped
// axis would leave the three widths equal
DEF_TEST(CoreTextPipeline_FontArguments_VariableFontAxes, reporter) {
    sk_sp<SkTypeface> variable = ToolUtils::CreateTypefaceFromResource("fonts/Variable.ttf");
    if (!variable) {
        // Only CoreText-backed font managers load resource data; the portable one cannot
        return;
    }
    auto provider = sk_make_sp<TypefaceFontProvider>();
    provider->registerTypeface(variable, SkString("Variable"));
    auto fontCollection = sk_make_sp<FontCollection>();
    fontCollection->setAssetFontManager(provider);
    fontCollection->setDefaultFontManager(ToolUtils::TestFontMgr());

    auto layout = [&](std::optional<Coordinate> coord) {
        return layout_axis_sample(fontCollection, "Variable", coord);
    };

    // TextStyle font arguments must reach the CTFont: wdth moves advances in this font
    // (wght does not), so the three layouts can only agree when the axis is dropped
    auto regular = layout(std::nullopt);
    auto narrow = layout(Coordinate{Coordinate::wdth, 50.0f});
    auto wide = layout(Coordinate{Coordinate::wdth, 200.0f});
    for (Paragraph* paragraph : {regular.get(), narrow.get(), wide.get()}) {
        REPORTER_ASSERT(reporter, static_cast<ParagraphImpl*>(paragraph)->isCoreTextShaped());
        REPORTER_ASSERT(reporter,
                        paragraph->unresolvedGlyphs() == 0,
                        "got %d",
                        paragraph->unresolvedGlyphs());
    }
    const SkScalar regularWidth = regular->getMaxIntrinsicWidth();
    const SkScalar narrowWidth = narrow->getMaxIntrinsicWidth();
    const SkScalar wideWidth = wide->getMaxIntrinsicWidth();
    REPORTER_ASSERT(reporter,
                    narrowWidth < regularWidth - 10.0f,
                    "wdth=50 width %g vs default %g",
                    narrowWidth,
                    regularWidth);
    REPORTER_ASSERT(reporter,
                    wideWidth > regularWidth + 10.0f,
                    "wdth=200 width %g vs default %g",
                    wideWidth,
                    regularWidth);
    const SkTypeface* narrowTypeface = first_run_typeface(narrow.get());
    REPORTER_ASSERT(reporter,
                    axis_near(narrowTypeface, Coordinate::wdth, 50.0f),
                    "run typeface reports wdth %g",
                    axis_value(narrowTypeface, Coordinate::wdth).value_or(-1.0f));
}

// A wght axis on the system font changes its weight while opsz keeps following the point size,
// since pinning opsz would freeze the optical size the system font picks per size
DEF_TEST(CoreTextPipeline_FontArguments_SystemFontWeightAxis, reporter) {
    if (!has_font_families({".AppleSystemUIFont"})) {
        return;
    }
    // Flutter's default family on Apple platforms. wght must come from the font arguments,
    // while opsz keeps tracking the point size: pinning it (as SkCTFontCreateExactCopy does on
    // the HarfBuzz path) would freeze the optical size the system font picks per size
    auto check = [&](const sk_sp<FontCollection>& fontCollection) {
        auto layout = [&](std::optional<Coordinate> coord) {
            return layout_axis_sample(fontCollection, ".AppleSystemUIFont", coord);
        };
        auto regular = layout(std::nullopt);
        auto black = layout(Coordinate{Coordinate::wght, 900.0f});
        auto thin = layout(Coordinate{Coordinate::wght, 100.0f});
        for (Paragraph* paragraph : {regular.get(), black.get(), thin.get()}) {
            REPORTER_ASSERT(reporter, static_cast<ParagraphImpl*>(paragraph)->isCoreTextShaped());
            REPORTER_ASSERT(reporter,
                            paragraph->unresolvedGlyphs() == 0,
                            "got %d",
                            paragraph->unresolvedGlyphs());
        }
        const SkScalar regularWidth = regular->getMaxIntrinsicWidth();
        const SkScalar blackWidth = black->getMaxIntrinsicWidth();
        const SkScalar thinWidth = thin->getMaxIntrinsicWidth();
        REPORTER_ASSERT(reporter,
                        blackWidth > regularWidth + 20.0f,
                        "wght=900 width %g vs regular %g",
                        blackWidth,
                        regularWidth);
        REPORTER_ASSERT(reporter,
                        thinWidth < regularWidth - 5.0f,
                        "wght=100 width %g vs regular %g",
                        thinWidth,
                        regularWidth);

        const SkTypeface* regularTypeface = first_run_typeface(regular.get());
        const SkTypeface* blackTypeface = first_run_typeface(black.get());
        REPORTER_ASSERT(reporter,
                        axis_near(blackTypeface, Coordinate::wght, 900.0f),
                        "wght=900 run typeface reports wght %g",
                        axis_value(blackTypeface, Coordinate::wght).value_or(-1.0f));
        REPORTER_ASSERT(reporter,
                        axis_near(regularTypeface, Coordinate::opsz, 48.0f),
                        "regular run typeface reports opsz %g",
                        axis_value(regularTypeface, Coordinate::opsz).value_or(-1.0f));
        REPORTER_ASSERT(reporter,
                        axis_near(blackTypeface, Coordinate::opsz, 48.0f),
                        "wght=900 run typeface reports opsz %g",
                        axis_value(blackTypeface, Coordinate::opsz).value_or(-1.0f));
    };

    // An empty collection binds the system font through CoreText alone; a CoreText-backed
    // default manager resolves the same family to an SkTypeface_Mac first
    check(sk_make_sp<FontCollection>());
    auto withDefaultManager = sk_make_sp<FontCollection>();
    withDefaultManager->setDefaultFontManager(ToolUtils::TestFontMgr());
    check(withDefaultManager);
}

namespace {
// Flutter's default family on Apple platforms carrying the BCP-47 tag Flutter copies from
// Locale.toLanguageTag() into TextStyle::setLocale
TextStyle system_font_style(const char* locale) {
    TextStyle style;
    style.setFontFamilies({SkString(".AppleSystemUIFont")});
    style.setFontSize(16.0f);
    style.setLocale(SkString(locale));
    return style;
}

// Lays out U+9AA8 U+76F4 U+5203 in a single span through CoreText
std::unique_ptr<Paragraph> layout_locale_sample(const sk_sp<FontCollection>& fontCollection,
                                                const char* locale) {
    ParagraphStyle paragraphStyle;
    paragraphStyle.setTextShapingEngine(TextShapingEngine::kCoreText);
    ParagraphBuilderImpl builder(paragraphStyle, fontCollection, get_unicode(paragraphStyle));
    builder.pushStyle(system_font_style(locale));
    builder.addText("\xE9\xAA\xA8\xE7\x9B\xB4\xE5\x88\x83");
    builder.pop();
    auto paragraph = builder.Build();
    paragraph->layout(1000.0f);
    return paragraph;
}

// Family of the typeface CoreText bound to the index-th font range; empty when there is no
// such range so a failed contrast reports what was shaped instead of indexing past the end
SkString font_family(const Paragraph* paragraph, size_t index) {
    const std::vector<Paragraph::FontInfo> fonts = paragraph->getFonts();
    SkString family;
    if (index < fonts.size() && fonts[index].fFont.getTypeface()) {
        fonts[index].fFont.getTypeface()->getFamilyName(&family);
    }
    return family;
}
}  // namespace

// The span locale reaches CoreText as kCTLanguageAttributeName, so unified Han code points fall
// back to the Japanese, Chinese or Korean font the locale asks for
DEF_TEST(CoreTextPipeline_Locale_LanguageAttributeDrivesCJKFallback, reporter) {
    // An empty collection leaves every fallback decision to CoreText, which is where the
    // span locale has to arrive as kCTLanguageAttributeName
    auto fontCollection = sk_make_sp<FontCollection>();
    auto ja = layout_locale_sample(fontCollection, "ja");
    auto jaJP = layout_locale_sample(fontCollection, "ja-JP");
    auto ko = layout_locale_sample(fontCollection, "ko");
    auto zhHant = layout_locale_sample(fontCollection, "zh-Hant");
    auto zhHans = layout_locale_sample(fontCollection, "zh-Hans");

    ParagraphStyle paragraphStyle;
    paragraphStyle.setTextShapingEngine(TextShapingEngine::kCoreText);
    ParagraphBuilderImpl builder(paragraphStyle, fontCollection, get_unicode(paragraphStyle));
    builder.pushStyle(system_font_style("ja"));
    builder.addText("\xE9\xAA\xA8");
    builder.pop();
    builder.pushStyle(system_font_style("zh-Hant"));
    builder.addText("\xE9\xAA\xA8");
    builder.pop();
    auto mixed = builder.Build();
    mixed->layout(1000.0f);

    for (Paragraph* paragraph :
         {ja.get(), jaJP.get(), ko.get(), zhHant.get(), zhHans.get(), mixed.get()}) {
        auto impl = static_cast<ParagraphImpl*>(paragraph);
        REPORTER_ASSERT(reporter, impl->isCoreTextShaped());
        REPORTER_ASSERT(reporter,
                        paragraph->unresolvedGlyphs() == 0,
                        "got %d",
                        paragraph->unresolvedGlyphs());
        REPORTER_ASSERT(reporter, !paragraph->getFonts().empty());
    }

    // Han unification: U+9AA8 U+76F4 U+5203 are the same code points in ja, zh-Hans, zh-Hant
    // and ko, so only the span locale can tell CoreText which regional font to fall back to.
    // Contrasts stay between explicit tags: the no-locale pick follows the machine's languages
    const SkString jaFamily = font_family(ja.get(), 0);
    const SkString jaJPFamily = font_family(jaJP.get(), 0);
    const SkString koFamily = font_family(ko.get(), 0);
    const SkString zhHantFamily = font_family(zhHant.get(), 0);
    const SkString zhHansFamily = font_family(zhHans.get(), 0);
    REPORTER_ASSERT(reporter,
                    jaFamily.contains("Hiragino"),
                    "locale ja shaped with '%s'",
                    jaFamily.c_str());
    REPORTER_ASSERT(reporter,
                    jaJPFamily.contains("Hiragino"),
                    "locale ja-JP shaped with '%s'",
                    jaJPFamily.c_str());
    REPORTER_ASSERT(reporter,
                    koFamily.contains("Apple SD Gothic"),
                    "locale ko shaped with '%s'",
                    koFamily.c_str());
    REPORTER_ASSERT(reporter,
                    zhHantFamily.contains("PingFang") && zhHantFamily.contains("TC"),
                    "locale zh-Hant shaped with '%s'",
                    zhHantFamily.c_str());
    REPORTER_ASSERT(reporter,
                    !zhHansFamily.contains("Hiragino"),
                    "locale zh-Hans shaped with '%s'",
                    zhHansFamily.c_str());

    // The language attribute must cover the span range only: a paragraph-wide tag or one
    // leaking out of the first span would shape both ideographs with the Japanese font
    const size_t mixedRuns = mixed->getFonts().size();
    const SkString mixedJaFamily = font_family(mixed.get(), 0);
    const SkString mixedZhHantFamily = font_family(mixed.get(), 1);
    REPORTER_ASSERT(
            reporter, mixedRuns >= 2, "ja + zh-Hant spans produced %zu font range(s)", mixedRuns);
    REPORTER_ASSERT(reporter,
                    mixedJaFamily.contains("Hiragino"),
                    "ja span shaped with '%s'",
                    mixedJaFamily.c_str());
    REPORTER_ASSERT(reporter,
                    mixedZhHantFamily.contains("TC"),
                    "zh-Hant span shaped with '%s'",
                    mixedZhHantFamily.c_str());
}

namespace {
// Lays out "Synthetic" in one span so the first run carries the whole style request
std::unique_ptr<Paragraph> layout_styled_sample(const sk_sp<FontCollection>& fontCollection,
                                                const char* family,
                                                SkFontStyle fontStyle,
                                                bool fakeMissingFontStyles) {
    TextStyle style;
    style.setFontFamilies({SkString(family)});
    style.setFontSize(24.0f);
    style.setFontStyle(fontStyle);
    ParagraphStyle paragraphStyle;
    paragraphStyle.setTextShapingEngine(TextShapingEngine::kCoreText);
    paragraphStyle.setFakeMissingFontStyles(fakeMissingFontStyles);
    ParagraphBuilderImpl builder(paragraphStyle, fontCollection, get_unicode(paragraphStyle));
    builder.pushStyle(style);
    builder.addText("Synthetic");
    builder.pop();
    auto paragraph = builder.Build();
    paragraph->layout(1000.0f);
    return paragraph;
}
}  // namespace

// Fake bold and skew apply only when the bound face lacks the requested style: a bold italic
// request on a Regular-only face gets both (fakeMissingFontStyles), a real bold face gets neither.
// Fallback faces are open bug B7 and stay out of this test
DEF_TEST(CoreTextPipeline_SyntheticStyles_OnlyOnRequestedFace, reporter) {
    sk_sp<SkTypeface> regular = ToolUtils::CreateTypefaceFromResource("fonts/Roboto-Regular.ttf");
    if (!regular) {
        // Only CoreText-backed font managers load resource data; the portable one cannot
        return;
    }
    const SkFontStyle boldItalic(
            SkFontStyle::kBold_Weight, SkFontStyle::kNormal_Width, SkFontStyle::kItalic_Slant);
    REPORTER_ASSERT(reporter, ParagraphStyle().fakeMissingFontStyles());

    auto fontCollection = make_roboto_collection(regular);
    auto faked = layout_styled_sample(fontCollection, "Roboto", boldItalic, true);
    REPORTER_ASSERT(reporter, static_cast<ParagraphImpl*>(faked.get())->isCoreTextShaped());
    REPORTER_ASSERT(reporter, faked->unresolvedGlyphs() == 0, "got %d", faked->unresolvedGlyphs());
    const SkFont font = faked->getFontAt(0);
    REPORTER_ASSERT(reporter, font.getTypeface());
    if (!font.getTypeface()) {
        return;
    }

    // Only a Regular face is registered, so the bound typeface is upright and sub-semibold and
    // the request can only be met synthetically; a failure here means the resource changed
    const SkFontStyle bound = font.getTypeface()->fontStyle();
    REPORTER_ASSERT(reporter,
                    bound.weight() < SkFontStyle::kSemiBold_Weight,
                    "Roboto run typeface weight %d",
                    bound.weight());
    REPORTER_ASSERT(reporter,
                    bound.slant() != SkFontStyle::kItalic_Slant,
                    "Roboto run typeface slant %d",
                    static_cast<int>(bound.slant()));
    REPORTER_ASSERT(reporter, font.isEmbolden());
    REPORTER_ASSERT(reporter,
                    SkScalarNearlyEqual(font.getSkewX(), -0.25f),
                    "skewX %g, want -0.25",
                    font.getSkewX());

    // Sharing fontCollection also verifies ParagraphCache keys on fakeMissingFontStyles instead
    // of replaying the emboldened and skewed run of the flag-on entry above
    auto plain = layout_styled_sample(fontCollection, "Roboto", boldItalic, false);
    const SkFont plainFont = plain->getFontAt(0);
    REPORTER_ASSERT(reporter, static_cast<ParagraphImpl*>(plain.get())->isCoreTextShaped());
    REPORTER_ASSERT(reporter, !plainFont.isEmbolden());
    REPORTER_ASSERT(reporter,
                    plainFont.getSkewX() == 0,
                    "fakeMissingFontStyles=false skewed the run by %g",
                    plainFont.getSkewX());
    REPORTER_ASSERT(reporter,
                    fontCollection->getParagraphCache()->count() == 2,
                    "%d cache entries, want 2 (one per fakeMissingFontStyles)",
                    fontCollection->getParagraphCache()->count());

    // CoreText binds a real bold system face for a bold request, so synthesis must compare
    // against the bound face and stay off; keying on the requested weight alone fails here
    auto systemBold = layout_styled_sample(
            sk_make_sp<FontCollection>(), ".AppleSystemUIFont", SkFontStyle::Bold(), true);
    const SkFont systemFont = systemBold->getFontAt(0);
    REPORTER_ASSERT(reporter, static_cast<ParagraphImpl*>(systemBold.get())->isCoreTextShaped());
    REPORTER_ASSERT(reporter,
                    systemBold->unresolvedGlyphs() == 0,
                    "got %d",
                    systemBold->unresolvedGlyphs());
    REPORTER_ASSERT(reporter, systemFont.getTypeface());
    if (systemFont.getTypeface()) {
        const int boundWeight = systemFont.getTypeface()->fontStyle().weight();
        REPORTER_ASSERT(reporter,
                        boundWeight >= SkFontStyle::kSemiBold_Weight,
                        "system font bound weight %d for a bold request",
                        boundWeight);
        REPORTER_ASSERT(reporter,
                        !systemFont.isEmbolden(),
                        "real bold face (weight %d) was emboldened",
                        boundWeight);
        REPORTER_ASSERT(reporter,
                        systemFont.getSkewX() == 0,
                        "upright request skewed by %g",
                        systemFont.getSkewX());
    }
}

namespace {
// Lays out text in one span of family through CoreText; callers turn the paragraph cache
// off so every call is an independent typesetting pass
std::unique_ptr<Paragraph> layout_identity_sample(const sk_sp<FontCollection>& fontCollection,
                                                  const char* family,
                                                  const char* text) {
    TextStyle style;
    style.setFontFamilies({SkString(family)});
    style.setFontSize(20.0f);
    ParagraphStyle paragraphStyle;
    paragraphStyle.setTextShapingEngine(TextShapingEngine::kCoreText);
    ParagraphBuilderImpl builder(paragraphStyle, fontCollection, get_unicode(paragraphStyle));
    builder.pushStyle(style);
    builder.addText(text);
    builder.pop();
    auto paragraph = builder.Build();
    paragraph->layout(1000.0f);
    return paragraph;
}

// Identity of the typeface bound to the first run; 0 when nothing was shaped
SkTypefaceID first_run_typeface_id(Paragraph* paragraph) {
    const SkTypeface* typeface = first_run_typeface(paragraph);
    return typeface ? typeface->uniqueID() : 0;
}
}  // namespace

// Relayout binds the same SkTypefaceID for an asset face and for the system font, so glyph
// strikes keyed by typeface stay warm instead of missing on every typesetting pass
DEF_TEST(CoreTextPipeline_TypefaceIdentity_StableAcrossLayouts, reporter) {
    sk_sp<SkTypeface> roboto = ToolUtils::CreateTypefaceFromResource("fonts/Roboto-Regular.ttf");
    if (!roboto) {
        // Only CoreText-backed font managers load resource data; the portable one cannot
        return;
    }
    auto fontCollection = make_roboto_collection(roboto);
    // A cache hit replays the first layout's SkFonts, which would fake the stability under test
    fontCollection->getParagraphCache()->turnOn(false);

    // Glyph strikes downstream are keyed by SkTypefaceID, so a typeface minted per typesetting
    // pass would miss them on every relayout; the data-backed face goes through the registry's
    // descriptor path and the system font through its per-size re-creation
    for (const char* family : {"Roboto", ".AppleSystemUIFont"}) {
        auto first = layout_identity_sample(fontCollection, family, "Identity one");
        auto second = layout_identity_sample(fontCollection, family, "Identity two");
        REPORTER_ASSERT(reporter, static_cast<ParagraphImpl*>(first.get())->isCoreTextShaped());
        REPORTER_ASSERT(
                reporter, first->unresolvedGlyphs() == 0, "got %d", first->unresolvedGlyphs());
        const SkTypefaceID firstId = first_run_typeface_id(first.get());
        const SkTypefaceID secondId = first_run_typeface_id(second.get());
        REPORTER_ASSERT(reporter,
                        firstId != 0 && firstId == secondId,
                        "%s: typeface %u in the first layout, %u in the second",
                        family,
                        firstId,
                        secondId);

        first->markDirty();
        first->layout(500.0f);
        const SkTypefaceID relaidId = first_run_typeface_id(first.get());
        REPORTER_ASSERT(reporter,
                        relaidId == firstId,
                        "%s: typeface %u before relayout, %u after",
                        family,
                        firstId,
                        relaidId);
    }

    // The asset font must actually be what CoreText bound, or the Roboto pass above only
    // measured the system fallback twice
    auto asset = layout_identity_sample(fontCollection, "Roboto", "Identity");
    REPORTER_ASSERT(reporter,
                    font_family(asset.get(), 0).equals("Roboto"),
                    "Roboto resolved to '%s'",
                    font_family(asset.get(), 0).c_str());
}

namespace {
// Forwards to a real manager and counts every family or typeface query, so a test can prove a
// code path never consults it
class CountingFontMgr final : public SkFontMgr {
public:
    explicit CountingFontMgr(sk_sp<SkFontMgr> fontMgr) : fFontMgr(std::move(fontMgr)) {}
    int queries() const { return fQueries; }

protected:
    int onCountFamilies() const override {
        ++fQueries;
        return fFontMgr->countFamilies();
    }
    void onGetFamilyName(int index, SkString* familyName) const override {
        ++fQueries;
        fFontMgr->getFamilyName(index, familyName);
    }
    sk_sp<SkFontStyleSet> onCreateStyleSet(int index) const override {
        ++fQueries;
        return fFontMgr->createStyleSet(index);
    }
    sk_sp<SkFontStyleSet> onMatchFamily(const char familyName[]) const override {
        ++fQueries;
        return fFontMgr->matchFamily(familyName);
    }
    sk_sp<SkTypeface> onMatchFamilyStyle(const char familyName[],
                                         const SkFontStyle& style) const override {
        ++fQueries;
        return fFontMgr->matchFamilyStyle(familyName, style);
    }
    sk_sp<SkTypeface> onMatchFamilyStyleCharacter(const char familyName[],
                                                  const SkFontStyle& style,
                                                  const char* bcp47[],
                                                  int bcp47Count,
                                                  SkUnichar character) const override {
        ++fQueries;
        return fFontMgr->matchFamilyStyleCharacter(familyName, style, bcp47, bcp47Count, character);
    }
    sk_sp<SkTypeface> onMakeFromData(sk_sp<SkData> data, int ttcIndex) const override {
        ++fQueries;
        return fFontMgr->makeFromData(std::move(data), ttcIndex);
    }
    sk_sp<SkTypeface> onMakeFromStreamIndex(std::unique_ptr<SkStreamAsset> stream,
                                            int ttcIndex) const override {
        ++fQueries;
        return fFontMgr->makeFromStream(std::move(stream), ttcIndex);
    }
    sk_sp<SkTypeface> onMakeFromStreamArgs(std::unique_ptr<SkStreamAsset> stream,
                                           const SkFontArguments& args) const override {
        ++fQueries;
        return fFontMgr->makeFromStream(std::move(stream), args);
    }
    sk_sp<SkTypeface> onMakeFromFile(const char path[], int ttcIndex) const override {
        ++fQueries;
        return fFontMgr->makeFromFile(path, ttcIndex);
    }
    sk_sp<SkTypeface> onLegacyMakeTypeface(const char familyName[],
                                           SkFontStyle style) const override {
        ++fQueries;
        return fFontMgr->legacyMakeTypeface(familyName, style);
    }

private:
    sk_sp<SkFontMgr> fFontMgr;
    mutable int fQueries = 0;
};
}  // namespace

// System families, the cascade for CJK, Arabic and emoji, the strut, empty-line metrics and the
// ellipsis all resolve through CoreText, so the default manager is never asked anything
DEF_TEST(CoreTextPipeline_DefaultFontManagerIgnored, reporter) {
    if (!has_font_families({"Helvetica", "PingFang SC", "Apple Color Emoji"})) {
        return;
    }
    auto counting = sk_make_sp<CountingFontMgr>(ToolUtils::TestFontMgr());
    auto fontCollection = sk_make_sp<FontCollection>();
    fontCollection->setDefaultFontManager(counting);
    fontCollection->enableFontFallback();

    ParagraphStyle paragraphStyle;
    paragraphStyle.setTextShapingEngine(TextShapingEngine::kCoreText);
    paragraphStyle.setMaxLines(1);
    paragraphStyle.setEllipsis(u"\u2026");
    StrutStyle strut;
    strut.setStrutEnabled(true);
    strut.setFontFamilies({SkString("Helvetica")});
    strut.setFontSize(16.0f);
    paragraphStyle.setStrutStyle(strut);

    TextStyle style;
    style.setFontFamilies({SkString("Helvetica")});
    style.setFontSize(16.0f);
    ParagraphBuilderImpl builder(paragraphStyle, fontCollection, get_unicode(paragraphStyle));
    builder.pushStyle(style);
    builder.addText(
            "Latin \xE4\xB8\xAD\xE6\x96\x87 \xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A "
            "\xF0\x9F\x98\x80 and enough trailing text to need an ellipsis\n\n");
    builder.pop();
    auto paragraph = builder.Build();
    paragraph->layout(200.0f);

    auto impl = static_cast<ParagraphImpl*>(paragraph.get());
    REPORTER_ASSERT(reporter, impl->isCoreTextShaped());
    REPORTER_ASSERT(reporter, paragraph->didExceedMaxLines());
    REPORTER_ASSERT(reporter, impl->unresolvedGlyphs() == 0, "got %d", impl->unresolvedGlyphs());
    REPORTER_ASSERT(reporter, counting->queries() == 0, "queries=%d", counting->queries());
}

// Empty fontFamilies with an unresolved glyph (e.g. U+10FFFF hitting LastResort) must bind the
// default system typeface instead of dereferencing a null primary SkTypeface, and keep
// unresolvedGlyphs() and unresolvedCodepoints() synchronized across control/format characters
DEF_TEST(CoreTextPipeline_EmptyFontFamilies_LastResortAndUnresolvedSync, reporter) {
    auto fc = test_font_collection();
    ParagraphStyle ps;
    ps.setTextShapingEngine(TextShapingEngine::kCoreText);
    ps.setFakeMissingFontStyles(true);

    TextStyle ts;
    ts.setFontFamilies({});
    ts.setFontSize(16.0f);
    ts.setFontStyle(SkFontStyle::Bold());

    ParagraphBuilderImpl builder(ps, fc, get_unicode(ps));
    builder.pushStyle(ts);
    // U+10FFFF is a noncharacter guaranteed to fall through to LastResort, surrounded by ZWJ/ZWNJ
    // and newline controls that must not be recorded in unresolvedCodepoints
    builder.addText("A\n\xE2\x80\x8D\xF4\x8F\xBF\xBF\xE2\x80\x8B");
    builder.pop();

    auto p = builder.Build();
    p->layout(500.0f);

    REPORTER_ASSERT(reporter, p->unresolvedGlyphs() == 1, "unresolved=%d", p->unresolvedGlyphs());
    auto unresolved = p->unresolvedCodepoints();
    REPORTER_ASSERT(reporter, unresolved.size() == 1, "size=%zu", unresolved.size());
    REPORTER_ASSERT(
            reporter, unresolved.count(0x10FFFF) == 1, "got %zu", unresolved.count(0x10FFFF));
    REPORTER_ASSERT(reporter, unresolved.count('\n') == 0, "got %zu", unresolved.count('\n'));
    REPORTER_ASSERT(reporter, unresolved.count(0x200D) == 0, "got %zu", unresolved.count(0x200D));
    REPORTER_ASSERT(reporter, unresolved.count(0x200B) == 0, "got %zu", unresolved.count(0x200B));
}

namespace {
// text in 20pt "DejaVu Sans" from fontCollection with letterSpacing, laid out on one unwrapped
// kCoreText line with the rounding hack off
std::unique_ptr<Paragraph> layout_dejavu(const sk_sp<FontCollection>& fontCollection,
                                         std::string_view text,
                                         SkScalar letterSpacing) {
    ParagraphStyle paragraphStyle;
    paragraphStyle.setTextShapingEngine(TextShapingEngine::kCoreText);
    paragraphStyle.setApplyRoundingHack(false);
    TextStyle style;
    style.setFontFamilies({SkString("DejaVu Sans")});
    style.setFontSize(20.0f);
    style.setLetterSpacing(letterSpacing);
    ParagraphBuilderImpl builder(paragraphStyle, fontCollection, get_unicode(paragraphStyle));
    builder.pushStyle(style);
    builder.addText(text.data(), text.size());
    auto paragraph = builder.Build();
    paragraph->layout(10000);
    return paragraph;
}
}  // namespace

// A primary font without U+000A must still keep '\n' in the primary font's run as .notdef so
// each hard-break segment matches a standalone DejaVu Sans line in width, metrics, tight boxes,
// letterSpacing, and line-end hit-testing, while an empty '\n' line keeps primary-font height
DEF_TEST(CoreTextPipeline_HardBreak_PrimaryFontWithoutNewline, reporter) {
    sk_sp<SkTypeface> dejaVu = ToolUtils::CreateTypefaceFromResource("fonts/DejaVuSans.subset.ttf");
    if (!dejaVu) {
        return;
    }
    // DejaVuSans.subset.ttf maps 'H', 'a', and 'x', and omits '\n'
    REPORTER_ASSERT(
            reporter,
            SkFont(dejaVu).unicharToGlyph('\n') == 0 && SkFont(dejaVu).unicharToGlyph('a') != 0,
            "got glyph %d for '\\n', %d for 'a'",
            SkFont(dejaVu).unicharToGlyph('\n'),
            SkFont(dejaVu).unicharToGlyph('a'));
    auto provider = sk_make_sp<TypefaceFontProvider>();
    provider->registerTypeface(dejaVu, SkString("DejaVu Sans"));
    auto fontCollection = sk_make_sp<FontCollection>();
    fontCollection->setAssetFontManager(provider);
    fontCollection->setDefaultFontManager(ToolUtils::TestFontMgr());

    for (std::string_view text : {"aa\naa", "aaaaaaaaaaaaa\naaaa", "Ha\n\naxH"}) {
        std::vector<std::string_view> segments;
        for (size_t pos = 0;;) {
            const size_t nl = text.find('\n', pos);
            segments.push_back(text.substr(pos, nl == std::string_view::npos ? nl : nl - pos));
            if (nl == std::string_view::npos) {
                break;
            }
            pos = nl + 1;
        }

        auto unspaced = layout_dejavu(fontCollection, text, 0.0f);
        std::vector<LineMetrics> unspacedLines;
        unspaced->getLineMetrics(unspacedLines);
        REPORTER_ASSERT(reporter,
                        unspacedLines.size() == segments.size(),
                        "got %zu lines, want %zu",
                        unspacedLines.size(),
                        segments.size());
        if (unspacedLines.size() != segments.size()) {
            continue;
        }

        for (SkScalar letterSpacing : {0.0f, 0.6f}) {
            skiatest::ReporterContext ctx(
                    reporter,
                    SkStringPrintf(
                            "'%.*s' spacing=%g", (int)text.size(), text.data(), letterSpacing));
            auto ct = layout_dejavu(fontCollection, text, letterSpacing);
            std::vector<LineMetrics> ctLines;
            ct->getLineMetrics(ctLines);
            REPORTER_ASSERT(reporter,
                            ctLines.size() == segments.size(),
                            "got %zu lines, want %zu",
                            ctLines.size(),
                            segments.size());
            if (ctLines.size() != segments.size()) {
                continue;
            }
            // getFonts() reports one entry per run, in logical order
            const std::vector<Paragraph::FontInfo> ctFonts = ct->getFonts();
            REPORTER_ASSERT(reporter,
                            ctFonts.size() == ctLines.size(),
                            "got %zu runs, want %zu (one per segment)",
                            ctFonts.size(),
                            ctLines.size());
            for (size_t r = 1; r < ctFonts.size(); ++r) {
                REPORTER_ASSERT(reporter, ctFonts[r].fFont == ctFonts[0].fFont);
            }

            size_t segStart = 0;
            for (size_t i = 0; i < segments.size(); ++i) {
                const std::string_view seg = segments[i];
                const size_t segEnd = segStart + seg.size();
                if (seg.empty()) {
                    REPORTER_ASSERT(
                            reporter,
                            ctLines[i].fWidth == 0 && ctLines[i].fHeight == ctLines[0].fHeight,
                            "empty line %zu: got w=%.9g h=%.9g, want w=0 h=%.9g",
                            i,
                            ctLines[i].fWidth,
                            ctLines[i].fHeight,
                            ctLines[0].fHeight);
                    segStart = segEnd + 1;
                    continue;
                }
                auto single = layout_dejavu(fontCollection, seg, letterSpacing);
                std::vector<LineMetrics> singleLines;
                single->getLineMetrics(singleLines);
                REPORTER_ASSERT(
                        reporter, singleLines.size() == 1, "got %zu lines", singleLines.size());
                if (singleLines.size() != 1) {
                    segStart = segEnd + 1;
                    continue;
                }
                REPORTER_ASSERT(
                        reporter,
                        SkScalarNearlyEqual(ctLines[i].fWidth, singleLines[0].fWidth, 1e-4f) &&
                                ctLines[i].fHeight == singleLines[0].fHeight &&
                                ctLines[i].fAscent == singleLines[0].fAscent &&
                                ctLines[i].fDescent == singleLines[0].fDescent,
                        "line %zu: got w=%.9g h=%.9g, want w=%.9g h=%.9g",
                        i,
                        ctLines[i].fWidth,
                        ctLines[i].fHeight,
                        singleLines[0].fWidth,
                        singleLines[0].fHeight);
                const SkScalar wantGrowth = letterSpacing * seg.size();
                const SkScalar gotGrowth =
                        static_cast<SkScalar>(ctLines[i].fWidth - unspacedLines[i].fWidth);
                REPORTER_ASSERT(reporter,
                                SkScalarNearlyEqual(gotGrowth, wantGrowth, 1e-3f),
                                "line %zu growth: got %.6f, want %.6f",
                                i,
                                gotGrowth,
                                wantGrowth);

                const SkScalar y =
                        static_cast<SkScalar>(ctLines[i].fBaseline - ctLines[i].fAscent / 2);
                const SkScalar endX = static_cast<SkScalar>(ctLines[i].fWidth);
                const PositionWithAffinity ctPos = ct->getGlyphPositionAtCoordinate(endX, y);
                REPORTER_ASSERT(
                        reporter,
                        ctPos.position == SkToInt(segEnd) && ctPos.affinity == Affinity::kUpstream,
                        "line %zu end hit-test: got {%d, %d}, want {%d, %d}",
                        i,
                        ctPos.position,
                        static_cast<int>(ctPos.affinity),
                        SkToInt(segEnd),
                        static_cast<int>(Affinity::kUpstream));

                for (size_t k = 0; k < seg.size(); ++k) {
                    auto ctBoxes = ct->getRectsForRange(segStart + k,
                                                        segStart + k + 1,
                                                        RectHeightStyle::kTight,
                                                        RectWidthStyle::kTight);
                    auto singleBoxes = single->getRectsForRange(
                            k, k + 1, RectHeightStyle::kTight, RectWidthStyle::kTight);
                    REPORTER_ASSERT(reporter,
                                    ctBoxes.size() == 1 && singleBoxes.size() == 1,
                                    "line %zu char %zu: got %zu and %zu boxes",
                                    i,
                                    k,
                                    ctBoxes.size(),
                                    singleBoxes.size());
                    if (ctBoxes.size() == 1 && singleBoxes.size() == 1) {
                        REPORTER_ASSERT(
                                reporter,
                                SkScalarNearlyEqual(
                                        ctBoxes[0].rect.fLeft, singleBoxes[0].rect.fLeft, 1e-4f) &&
                                        SkScalarNearlyEqual(ctBoxes[0].rect.fRight,
                                                            singleBoxes[0].rect.fRight,
                                                            1e-4f),
                                "line %zu char %zu: got [%.9g, %.9g], want [%.9g, %.9g]",
                                i,
                                k,
                                ctBoxes[0].rect.fLeft,
                                ctBoxes[0].rect.fRight,
                                singleBoxes[0].rect.fLeft,
                                singleBoxes[0].rect.fRight);
                    }
                }
                segStart = segEnd + 1;
            }
        }
    }
}

namespace {
struct SegmentStyle {
    SkScalar fontSize;
    SkScalar letterSpacing;
    const char* text;
};

// Glyphs drawn on each line of a kCoreText Roboto paragraph made of segments joined by '\n',
// without the glyph of each '\n'
std::vector<std::vector<DrawnGlyph>> drawn_lines(skiatest::Reporter* reporter,
                                                 const sk_sp<FontCollection>& fontCollection,
                                                 SkSpan<const SegmentStyle> segments) {
    auto builder = contract_builder(fontCollection, contract_paragraph_style());
    std::string text;
    for (size_t i = 0; i < segments.size(); ++i) {
        TextStyle style;
        style.setFontFamilies({SkString("Roboto")});
        style.setFontSize(segments[i].fontSize);
        style.setLetterSpacing(segments[i].letterSpacing);
        builder->pushStyle(style);
        std::string segmentText = segments[i].text;
        if (i + 1 < segments.size()) {
            segmentText += '\n';
        }
        builder->addText(segmentText.data(), segmentText.size());
        builder->pop();
        text += segmentText;
    }
    auto paragraph = layout_built(reporter, builder.get(), 1e6f);
    std::vector<std::vector<DrawnGlyph>> lines(segments.size());
    for (const DrawnGlyph& glyph : drawn_glyphs(paragraph.get())) {
        if (text[glyph.utf8Start] != '\n' && glyph.lineNumber < SkToInt(lines.size())) {
            lines[glyph.lineNumber].push_back(glyph);
        }
    }
    return lines;
}
}  // namespace

// Each '\n' segment draws as it does alone, even when a non-adjacent segment of the paragraph has
// a nearly equal style: CoreTextRequestedFonts must not give it that segment's span font
DEF_TEST(CoreTextPipeline_RequestedFonts_SegmentsShapeAsAlone, reporter) {
    sk_sp<SkTypeface> roboto = ToolUtils::CreateTypefaceFromResource("fonts/Roboto-Regular.ttf");
    if (!roboto) {
        return;
    }
    auto fontCollection = make_roboto_collection(roboto);
    // ParagraphCache compares font sizes only nearly, so a standalone 14.0001pt "fi" would get
    // the cached 14pt layout
    fontCollection->getParagraphCache()->turnOn(false);
    // Roboto draws "fi" as one ligature glyph unless letterSpacing turns liga off; 1e-6 and
    // 14.0001 are nearly equal to the 0 and 14 of the first segment, and the 20pt segments keep
    // nearly equal ones from being merged with their neighbors. "سلام" falls back to a cursive
    // font, whose letter-spaced font is made once and reused on the later segment
    constexpr SegmentStyle kSegments[] = {
            {14, 0, "fi"},
            {14, 1.5f, "\xD8\xB3\xD9\x84\xD8\xA7\xD9\x85 fi"},
            {20, 0, "fi"},
            {14, 1e-6f, "fi"},
            {20, 0, "fi"},
            {14.0001f, 0, "fi"},
            {14, 1.5f, "\xD8\xB3\xD9\x84\xD8\xA7\xD9\x85 fi"},
            {14, 0, "fi"},
    };
    const std::vector<std::vector<DrawnGlyph>> together =
            drawn_lines(reporter, fontCollection, kSegments);
    for (size_t i = 0; i < std::size(kSegments); ++i) {
        skiatest::ReporterContext ctx(reporter, SkStringPrintf("segment %zu", i));
        const std::vector<DrawnGlyph> alone =
                drawn_lines(reporter, fontCollection, SkSpan(&kSegments[i], 1)).front();
        REPORTER_ASSERT(reporter,
                        together[i].size() == alone.size(),
                        "got %zu glyphs, want %zu",
                        together[i].size(),
                        alone.size());
        for (size_t k = 0; k < std::min(together[i].size(), alone.size()); ++k) {
            const DrawnGlyph& got = together[i][k];
            const DrawnGlyph& want = alone[k];
            const SkTypefaceID gotFace = got.font.getTypeface()->uniqueID();
            const SkTypefaceID wantFace = want.font.getTypeface()->uniqueID();
            REPORTER_ASSERT(reporter,
                            got.glyph == want.glyph && gotFace == wantFace,
                            "glyph %zu: got id %d %s, want id %d %s",
                            k,
                            got.glyph,
                            family_name(got.font).c_str(),
                            want.glyph,
                            family_name(want.font).c_str());
            // Positions on a later line of one CTTypesetter differ in float rounding only
            REPORTER_ASSERT(reporter,
                            got.font.getSize() == want.font.getSize() &&
                                    SkScalarNearlyEqual(got.position.fX, want.position.fX, 1e-4f),
                            "glyph %zu: got size %.9g x %.9g, want size %.9g x %.9g",
                            k,
                            got.font.getSize(),
                            got.position.fX,
                            want.font.getSize(),
                            want.position.fX);
        }
    }
}
