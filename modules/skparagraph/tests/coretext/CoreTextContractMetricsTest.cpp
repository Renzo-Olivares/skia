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

#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontArguments.h"
#include "include/core/SkFontMetrics.h"
#include "include/core/SkFourByteTag.h"
#include "include/core/SkRefCnt.h"
#include "include/core/SkScalar.h"
#include "include/core/SkString.h"
#include "include/core/SkTypeface.h"
#include "include/core/SkTypes.h"
#include "include/ports/SkTypeface_mac.h"
#include "modules/skparagraph/include/DartTypes.h"
#include "modules/skparagraph/include/FontCollection.h"
#include "modules/skparagraph/include/Paragraph.h"
#include "modules/skparagraph/include/ParagraphStyle.h"
#include "modules/skparagraph/include/TextStyle.h"
#include "modules/skparagraph/src/ParagraphBuilderImpl.h"
#include "modules/skparagraph/tests/coretext/CoreTextTestUtils.h"
#include "src/utils/mac/SkUniqueCFRef.h"
#include "tests/Test.h"
#include "tools/fonts/FontToolUtils.h"
#include "tools/fonts/TestEmptyTypeface.h"

#include <CoreText/CoreText.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string_view>
#include <tuple>
#include <vector>

using namespace skia::textlayout;
using namespace skia::textlayout::coretext_test;

namespace {
struct NativeLineBounds {
    SkScalar ascent;
    SkScalar descent;
};

// Typographic bounds of a native CTLine of text in the named font at size, as UILabel measures
NativeLineBounds native_line_bounds(const char* family, SkScalar size, const char* text) {
    SkUniqueCFRef<CTLineRef> line = native_line(native_font(family, size).get(), text, 0);
    CGFloat ascent = 0, descent = 0;
    CTLineGetTypographicBounds(line.get(), &ascent, &descent, nullptr);
    return {static_cast<SkScalar>(ascent), static_cast<SkScalar>(descent)};
}

// Tight ink of a drawn glyph as CoreText reports it, y down from the baseline. SkFont::getBounds
// pads color bitmap glyphs by up to 1.5px, which is not what gets painted
SkRect coretext_ink(const SkFont& font, SkGlyphID glyph) {
    SkUniqueCFRef<CTFontRef> sized(CTFontCreateCopyWithAttributes(
            SkTypeface_GetCTFontRef(font.getTypeface()), font.getSize(), nullptr, nullptr));
    const CGGlyph cgGlyph = glyph;
    CGRect ink;
    CTFontGetBoundingRectsForGlyphs(sized.get(), kCTFontOrientationHorizontal, &cgGlyph, &ink, 1);
    return SkRect::MakeLTRB(static_cast<SkScalar>(CGRectGetMinX(ink)),
                            static_cast<SkScalar>(-CGRectGetMaxY(ink)),
                            static_cast<SkScalar>(CGRectGetMaxX(ink)),
                            static_cast<SkScalar>(-CGRectGetMinY(ink)));
}

// Exposes base's CTFontRef when non-null (so CoreTextFontRegistry classifies it as a system UI
// face), uses empty metrics when uncloned, and returns varied on wght=2 while counting clones
class CountingCloneTypeface final : public TestEmptyTypeface {
public:
    static sk_sp<SkTypeface> Make(sk_sp<SkTypeface> base, sk_sp<SkTypeface> varied, int* clones) {
        return sk_sp<SkTypeface>(
                new CountingCloneTypeface(std::move(base), std::move(varied), clones));
    }

private:
    CountingCloneTypeface(sk_sp<SkTypeface> base, sk_sp<SkTypeface> varied, int* clones)
            : fBase(std::move(base)), fVaried(std::move(varied)), fCloneCount(clones) {}

    void* onGetCTFontRef() const override {
        return fBase ? const_cast<void*>(static_cast<const void*>(
                               SkTypeface_GetCTFontRef(fBase.get())))
                     : nullptr;
    }
    sk_sp<SkTypeface> onMakeClone(const SkFontArguments& args) const override {
        ++*fCloneCount;
        const auto pos = args.getVariationDesignPosition();
        for (int i = 0; i < pos.coordinateCount; ++i) {
            if (pos.coordinates[i].axis == SkSetFourByteTag('w', 'g', 'h', 't') &&
                pos.coordinates[i].value == 2.0f) {
                return fVaried;
            }
        }
        return nullptr;
    }

    sk_sp<SkTypeface> fBase;
    sk_sp<SkTypeface> fVaried;
    int* fCloneCount;
};
}  // namespace

// MT6: an empty paragraph has no lines and no glyph info, zero intrinsic widths, and still a
// positive height and baselines from the style's font, which Flutter uses to size the caret
DEF_TEST(CoreTextContract_MT6_EmptyParagraph, reporter) {
    sk_sp<FontCollection> fontCollection = asset_collection({{"fonts/ahem.ttf", "Ahem"}});
    if (!fontCollection) {
        return;
    }
    auto paragraph = layout_text(reporter,
                                 fontCollection,
                                 contract_paragraph_style(),
                                 contract_text_style("Ahem", 10.0f),
                                 "",
                                 100.0f);
    REPORTER_ASSERT(reporter, paragraph->lineNumber() == 0, "got %zu", paragraph->lineNumber());
    std::vector<LineMetrics> metrics;
    paragraph->getLineMetrics(metrics);
    REPORTER_ASSERT(reporter, metrics.empty(), "got %zu lines", metrics.size());
    LineMetrics lineMetrics;
    REPORTER_ASSERT(reporter, !paragraph->getLineMetricsAt(0, &lineMetrics));
    Paragraph::GlyphInfo glyphInfo;
    REPORTER_ASSERT(reporter, !paragraph->getGlyphInfoAtUTF16Offset(0, &glyphInfo));
    REPORTER_ASSERT(reporter, !paragraph->getClosestUTF16GlyphInfoAt(5, 5, &glyphInfo));
    REPORTER_ASSERT(reporter,
                    paragraph->getMinIntrinsicWidth() == 0,
                    "got %g",
                    paragraph->getMinIntrinsicWidth());
    REPORTER_ASSERT(reporter,
                    paragraph->getMaxIntrinsicWidth() == 0,
                    "got %g",
                    paragraph->getMaxIntrinsicWidth());
    REPORTER_ASSERT(reporter, paragraph->getHeight() > 0, "got %g", paragraph->getHeight());
    REPORTER_ASSERT(reporter,
                    paragraph->getAlphabeticBaseline() > 0,
                    "got %g",
                    paragraph->getAlphabeticBaseline());
    REPORTER_ASSERT(reporter,
                    paragraph->getIdeographicBaseline() > 0,
                    "got %g",
                    paragraph->getIdeographicBaseline());

    sk_sp<FontCollection> robotoOnly = asset_collection({{"fonts/Roboto-Regular.ttf", "Roboto"}});
    sk_sp<SkTypeface> ahem = ToolUtils::CreateTypefaceFromResource("fonts/ahem.ttf");
    if (!robotoOnly || !ahem) {
        return;
    }
    auto byFamily = layout_text(reporter,
                                fontCollection,
                                contract_paragraph_style(),
                                contract_text_style("Ahem", 40.0f),
                                "",
                                100.0f);
    TextStyle explicitAhem;
    explicitAhem.setFontSize(40.0f);
    explicitAhem.setTypeface(ahem);
    auto byTypeface =
            layout_text(reporter, robotoOnly, contract_paragraph_style(), explicitAhem, "", 100.0f);
    REPORTER_ASSERT(reporter,
                    byTypeface->getHeight() == byFamily->getHeight(),
                    "got %g, want %g",
                    byTypeface->getHeight(),
                    byFamily->getHeight());
    REPORTER_ASSERT(
            reporter,
            SkScalarNearlyEqual(
                    byTypeface->getAlphabeticBaseline(), byFamily->getAlphabeticBaseline(), 0.01f),
            "got %g, want %g",
            byTypeface->getAlphabeticBaseline(),
            byFamily->getAlphabeticBaseline());
}

// MT6: an empty paragraph with an explicit variable typeface and FontArguments takes its metrics
// from the varied clone in FontCollection's cache (non-system and system UI) across relayouts
DEF_TEST(CoreTextContract_MT6_EmptyParagraphExplicitVariableTypeface, reporter) {
    sk_sp<SkTypeface> ahem = ToolUtils::CreateTypefaceFromResource("fonts/ahem.ttf");
    sk_sp<FontCollection> ahemFonts = asset_collection({{"fonts/ahem.ttf", "Ahem"}});
    sk_sp<FontCollection> robotoOnly = asset_collection({{"fonts/Roboto-Regular.ttf", "Roboto"}});
    if (!ahem || !ahemFonts || !robotoOnly) {
        return;
    }
    auto ahemRef = layout_text(reporter,
                               ahemFonts,
                               contract_paragraph_style(),
                               contract_text_style("Ahem", 40.0f),
                               "",
                               100.0f);
    std::vector<sk_sp<SkTypeface>> bases = {nullptr};
    if (has_font_families({".AppleSystemUIFont"})) {
        SkUniqueCFRef<CTFontRef> ctSys(
                CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, 20.0, nullptr));
        if (sk_sp<SkTypeface> sysBase = ctSys ? SkMakeTypefaceFromCTFont(ctSys.get()) : nullptr) {
            bases.push_back(std::move(sysBase));
        }
    }
    const Coordinate coord = {SkSetFourByteTag('w', 'g', 'h', 't'), 2.0f};
    SkFontArguments args;
    args.setVariationDesignPosition({&coord, 1});
    for (const sk_sp<SkTypeface>& base : bases) {
        skiatest::ReporterContext baseCtx(reporter, base ? "system" : "custom");
        int explicitClones = 0;
        sk_sp<SkTypeface> countingFace = CountingCloneTypeface::Make(base, ahem, &explicitClones);
        TextStyle unvariedStyle;
        unvariedStyle.setFontSize(40.0f);
        unvariedStyle.setTypeface(countingFace);
        auto unvaried = layout_text(
                reporter, robotoOnly, contract_paragraph_style(), unvariedStyle, "", 100.0f);
        REPORTER_ASSERT(reporter,
                        ahemRef->getHeight() != unvaried->getHeight(),
                        "varied %g == unvaried %g",
                        ahemRef->getHeight(),
                        unvaried->getHeight());

        TextStyle explicitStyle = unvariedStyle;
        explicitStyle.setFontArguments(args);
        auto byTypeface = layout_text(
                reporter, robotoOnly, contract_paragraph_style(), explicitStyle, "", 100.0f);
        for (SkScalar width : {100.0f, 50.0f}) {
            skiatest::ReporterContext widthCtx(reporter, SkStringPrintf("width %g", width));
            byTypeface->layout(width);
            REPORTER_ASSERT(reporter,
                            byTypeface->getHeight() == ahemRef->getHeight(),
                            "got %g, want %g",
                            byTypeface->getHeight(),
                            ahemRef->getHeight());
            REPORTER_ASSERT(
                    reporter,
                    byTypeface->getAlphabeticBaseline() == ahemRef->getAlphabeticBaseline(),
                    "got %g, want %g",
                    byTypeface->getAlphabeticBaseline(),
                    ahemRef->getAlphabeticBaseline());
        }
        REPORTER_ASSERT(reporter, explicitClones == 1, "got %d clones, want 1", explicitClones);
    }
}

// MT1 (R93, MT1=A): line ascent and descent come from the faces CoreText picks, as in a
// native CTLine: a 20pt line with an emoji is 24pt like UILabel, not 29 (Flutter #31661)
DEF_TEST(CoreTextContract_MT1_LineMetricsFromChosenFont, reporter) {
    if (!has_font_families({"Helvetica", ".AppleSystemUIFont", "Apple Color Emoji"})) {
        return;
    }
    struct MetricsCase {
        const char* family;
        const char* text;
    };
    constexpr MetricsCase kCases[] = {
            {"Helvetica", "Hello"},
            {".AppleSystemUIFont", "Hello"},
            {".AppleSystemUIFont", "Hello \xF0\x9F\x98\x80"},
    };
    for (const MetricsCase& c : kCases) {
        skiatest::ReporterContext ctx(reporter, SkStringPrintf("%s '%s'", c.family, c.text));
        auto paragraph = layout_text(reporter,
                                     system_collection(),
                                     contract_paragraph_style(),
                                     contract_text_style(c.family, 20.0f),
                                     c.text,
                                     1000.0f);
        const std::vector<LineMetrics> lines = line_metrics(paragraph.get());
        REPORTER_ASSERT(reporter, lines.size() == 1, "got %zu lines", lines.size());
        if (lines.size() != 1) {
            continue;
        }
        const NativeLineBounds native = native_line_bounds(c.family, 20.0f, c.text);
        REPORTER_ASSERT(reporter,
                        SkScalarNearlyEqual(lines[0].fAscent, native.ascent, 1e-4f),
                        "got %g, want %g",
                        lines[0].fAscent,
                        native.ascent);
        REPORTER_ASSERT(reporter,
                        SkScalarNearlyEqual(lines[0].fDescent, native.descent, 1e-4f),
                        "got %g, want %g",
                        lines[0].fDescent,
                        native.descent);
        // Shared code rounds the line height to whole points
        const SkScalar nativeHeight = std::round(native.ascent + native.descent);
        REPORTER_ASSERT(reporter,
                        lines[0].fHeight == nativeHeight,
                        "got %g, want %g",
                        lines[0].fHeight,
                        nativeHeight);

        SkScalar runAscent = 0, runDescent = 0;
        for (const DrawnGlyph& glyph : drawn_glyphs(paragraph.get())) {
            SkFontMetrics metrics;
            glyph.font.getMetrics(&metrics);
            runAscent = std::max(runAscent, -metrics.fAscent);
            runDescent = std::max(runDescent, metrics.fDescent);
        }
        REPORTER_ASSERT(reporter,
                        SkScalarNearlyEqual(lines[0].fAscent, runAscent, 1e-4f),
                        "got %g, want %g",
                        lines[0].fAscent,
                        runAscent);
        REPORTER_ASSERT(reporter,
                        SkScalarNearlyEqual(lines[0].fDescent, runDescent, 1e-4f),
                        "got %g, want %g",
                        lines[0].fDescent,
                        runDescent);
    }
    auto emoji = layout_text(reporter,
                             system_collection(),
                             contract_paragraph_style(),
                             contract_text_style(".AppleSystemUIFont", 20.0f),
                             "Hello \xF0\x9F\x98\x80",
                             1000.0f);
    // UILabel and NSTextField measured 24pt for this line
    REPORTER_ASSERT(reporter, emoji->getHeight() == 24, "got %g", emoji->getHeight());
}

// MT1: the shorter emoji line must not clip the emoji (Flutter #79773): at 20pt its CoreText ink
// stays inside the line box from getLineMetrics. At 17pt native UILabel overflows too
DEF_TEST(CoreTextContract_MT1_EmojiInkInsideLine, reporter) {
    if (!has_font_families({".AppleSystemUIFont", "Apple Color Emoji"})) {
        return;
    }
    auto paragraph = layout_text(reporter,
                                 system_collection(),
                                 contract_paragraph_style(),
                                 contract_text_style(".AppleSystemUIFont", 20.0f),
                                 "Hello \xF0\x9F\x98\x80",
                                 1000.0f);
    const std::vector<LineMetrics> lines = line_metrics(paragraph.get());
    REPORTER_ASSERT(reporter, lines.size() == 1, "got %zu lines", lines.size());
    if (lines.size() != 1) {
        return;
    }
    const SkScalar top = lines[0].fBaseline - lines[0].fAscent;
    const SkScalar bottom = lines[0].fBaseline + lines[0].fDescent;
    const SkFont primary = paragraph->getFontAtUTF16Offset(0);
    int emojiGlyphs = 0;
    for (const DrawnGlyph& glyph : drawn_glyphs(paragraph.get())) {
        if (glyph.font.getTypeface()->uniqueID() == primary.getTypeface()->uniqueID()) {
            continue;
        }
        ++emojiGlyphs;
        const SkRect ink = coretext_ink(glyph.font, glyph.glyph).makeOffset(glyph.position);
        REPORTER_ASSERT(reporter, ink.fTop >= top, "got %g, want >= %g", ink.fTop, top);
        REPORTER_ASSERT(reporter, ink.fBottom <= bottom, "got %g, want <= %g", ink.fBottom, bottom);
    }
    REPORTER_ASSERT(reporter, emojiGlyphs == 1, "got %d", emojiGlyphs);
}

// MT1 / R134 (Flutter #79773): at 14pt, emoji keeps plain-text line height without inflating to
// 19px ink, and paint() draws overhanging emoji pixels unclipped onto a taller canvas (as UILabel)
DEF_TEST(CoreTextContract_MT1_SmallEmojiLineMetricsAndUnclippedPaint, reporter) {
    if (!has_font_families({".AppleSystemUIFont", "Apple Color Emoji"})) {
        return;
    }
    const TextStyle style = contract_text_style(".AppleSystemUIFont", 14.0f);
    auto plain = layout_text(
            reporter, system_collection(), contract_paragraph_style(), style, "Hello", 1000.0f);
    auto withEmoji = layout_text(reporter,
                                 system_collection(),
                                 contract_paragraph_style(),
                                 style,
                                 "Hello \xF0\x9F\x98\x80",
                                 1000.0f);
    REPORTER_ASSERT(reporter,
                    SkScalarNearlyEqual(withEmoji->getHeight(), plain->getHeight(), 0.05f),
                    "got %g, want %g",
                    withEmoji->getHeight(),
                    plain->getHeight());

    const int lineBottom = SkScalarFloorToInt(withEmoji->getHeight());
    const int canvasHeight = lineBottom + 12;
    SkBitmap bitmap;
    bitmap.allocN32Pixels(200, canvasHeight);
    bitmap.eraseColor(SK_ColorWHITE);
    SkCanvas canvas(bitmap);
    withEmoji->paint(&canvas, 0.0f, 0.0f);

    int inkPixelsBelowLineBox = 0;
    for (int y = lineBottom; y < canvasHeight; ++y) {
        for (int x = 0; x < bitmap.width(); ++x) {
            if (bitmap.getColor(x, y) != SK_ColorWHITE) {
                ++inkPixelsBelowLineBox;
            }
        }
    }
    REPORTER_ASSERT(reporter,
                    inkPixelsBelowLineBox > 0,
                    "expected unclipped emoji ink at y >= %d, got %d pixels",
                    lineBottom,
                    inkPixelsBelowLineBox);
}

// MT2: height, halfLeading, TextHeightBehavior and a forced strut follow Flutter rules: Ahem's
// 0.8 / 0.2 split scales, halfLeading splits evenly, kDisableFirstAscent drops it on line 1
DEF_TEST(CoreTextContract_MT2_HeightAndStrut, reporter) {
    sk_sp<FontCollection> fontCollection = asset_collection({{"fonts/ahem.ttf", "Ahem"}});
    if (!fontCollection) {
        return;
    }
    constexpr auto kCases =
            std::to_array<std::tuple<bool, TextHeightBehavior, SkScalar, SkScalar>>({
                    {false, TextHeightBehavior::kAll, 16, 20},
                    {true, TextHeightBehavior::kAll, 13, 20},
                    {false, TextHeightBehavior::kDisableFirstAscent, 8, 12},
            });
    for (const auto& [halfLeading, behavior, baseline, height] : kCases) {
        skiatest::ReporterContext ctx(
                reporter, SkStringPrintf("halfLeading %d, behavior %d", halfLeading, behavior));
        ParagraphStyle paragraphStyle = contract_paragraph_style();
        paragraphStyle.setTextHeightBehavior(behavior);
        TextStyle style = contract_text_style("Ahem", 10.0f);
        style.setHeight(2.0f);
        style.setHeightOverride(true);
        style.setHalfLeading(halfLeading);
        auto paragraph = layout_text(reporter, fontCollection, paragraphStyle, style, "a", 100);
        const std::vector<LineMetrics> lines = line_metrics(paragraph.get());
        REPORTER_ASSERT(reporter, lines.size() == 1, "got %zu lines", lines.size());
        if (lines.size() != 1) {
            continue;
        }
        // LineMetrics ascent and descent keep the full height on both engines; the baseline
        // shows where halfLeading and TextHeightBehavior put the extra space
        REPORTER_ASSERT(reporter,
                        SkScalarNearlyEqual(lines[0].fBaseline, baseline, 1e-3f),
                        "got %g, want %g",
                        lines[0].fBaseline,
                        baseline);
        REPORTER_ASSERT(
                reporter, lines[0].fHeight == height, "got %g, want %g", lines[0].fHeight, height);
    }

    ParagraphStyle strutStyle = contract_paragraph_style();
    StrutStyle strut;
    strut.setStrutEnabled(true);
    strut.setForceStrutHeight(true);
    strut.setFontFamilies({SkString("Ahem")});
    strut.setFontSize(20.0f);
    strutStyle.setStrutStyle(strut);
    std::unique_ptr<ParagraphBuilderImpl> builder = contract_builder(fontCollection, strutStyle);
    builder->pushStyle(contract_text_style("Ahem", 10.0f));
    builder->addText("a");
    builder->addPlaceholder(PlaceholderStyle(
            20.0f, 50.0f, PlaceholderAlignment::kBaseline, TextBaseline::kAlphabetic, 50.0f));
    auto placeholder = layout_built(reporter, builder.get(), 100.0f);
    REPORTER_ASSERT(reporter, placeholder->getHeight() == 20, "got %g", placeholder->getHeight());
}

// MT3: size 0 gives zero line widths, width stays the constraint (infinity too) and
// maxIntrinsicWidth stays finite for TextPainter. Intrinsics are ~0, not 0: both engines start
// them at FLT_MIN (TextWrapper.cpp), invisible in pixels (user decision Nov 14 2026)
DEF_TEST(CoreTextContract_MT3_ZeroFontSize, reporter) {
    sk_sp<FontCollection> fontCollection =
            asset_collection({{"fonts/Roboto-Regular.ttf", "Roboto"}});
    if (!fontCollection) {
        return;
    }
    auto paragraph = layout_text(reporter,
                                 fontCollection,
                                 contract_paragraph_style(),
                                 contract_text_style("Roboto", 0.0f),
                                 "Hello world",
                                 100.0f);
    REPORTER_ASSERT(
            reporter, paragraph->getLongestLine() == 0, "got %g", paragraph->getLongestLine());
    REPORTER_ASSERT(reporter, paragraph->getMaxWidth() == 100, "got %g", paragraph->getMaxWidth());
    for (SkScalar intrinsic :
         {paragraph->getMinIntrinsicWidth(), paragraph->getMaxIntrinsicWidth()}) {
        REPORTER_ASSERT(reporter, intrinsic >= 0 && intrinsic < 1e-6f, "got %g", intrinsic);
    }
    for (const LineMetrics& line : line_metrics(paragraph.get())) {
        skiatest::ReporterContext ctx(reporter, SkStringPrintf("line %zu", line.fLineNumber));
        REPORTER_ASSERT(reporter, line.fWidth == 0, "got %g", line.fWidth);
    }
    paragraph->layout(SK_ScalarInfinity);
    REPORTER_ASSERT(reporter,
                    paragraph->getMaxWidth() == SK_ScalarInfinity,
                    "got %g",
                    paragraph->getMaxWidth());
    REPORTER_ASSERT(reporter,
                    SkIsFinite(paragraph->getMaxIntrinsicWidth()),
                    "got %g",
                    paragraph->getMaxIntrinsicWidth());
}

// MT3: NaN fontSize collapses to zero-size runs and zero advances (including the ellipsis), and
// a NaN fontSize or variation coordinate gets ParagraphCache hits like any other style value
DEF_TEST(CoreTextContract_MT3_NanFontSizeZeroWidth, reporter) {
    sk_sp<FontCollection> fontCollection =
            asset_collection({{"fonts/Roboto-Regular.ttf", "Roboto"}});
    if (!fontCollection) {
        return;
    }
    ParagraphStyle style = contract_paragraph_style();
    style.setMaxLines(1);
    style.setEllipsis(SkString("..."));
    const TextStyle nanRoboto = contract_text_style("Roboto", SK_ScalarNaN);
    for (const char* text : {"Hello world", "Hello\nworld"}) {
        skiatest::ReporterContext ctx(reporter, SkString(text));
        auto paragraph = layout_text(reporter, fontCollection, style, nanRoboto, text, 100.0f);
        const bool hardBreak = std::string_view(text).find('\n') != std::string_view::npos;
        if (hardBreak) {
            REPORTER_ASSERT(reporter, paragraph->didExceedMaxLines());
        }
        REPORTER_ASSERT(
                reporter, paragraph->getLongestLine() == 0, "got %g", paragraph->getLongestLine());
        REPORTER_ASSERT(
                reporter,
                paragraph->getMaxIntrinsicWidth() >= 0 && paragraph->getMaxIntrinsicWidth() < 1e-6f,
                "got %g",
                paragraph->getMaxIntrinsicWidth());
        const std::vector<DrawnGlyph> glyphs = drawn_glyphs(paragraph.get());
        // "Hello" and the three ellipsis dots after the cut line, or all of "Hello world"
        const size_t expectedGlyphs = hardBreak ? 8 : 11;
        REPORTER_ASSERT(reporter,
                        glyphs.size() == expectedGlyphs,
                        "got %zu glyphs, expected %zu",
                        glyphs.size(),
                        expectedGlyphs);
        for (const DrawnGlyph& glyph : glyphs) {
            REPORTER_ASSERT(reporter, glyph.font.getSize() == 0.0f, "got %g", glyph.font.getSize());
        }
    }
    // Same text and style again: a hit adds no entry
    layout_text(reporter, fontCollection, style, nanRoboto, "Hello world", 100.0f);
    REPORTER_ASSERT(reporter,
                    fontCollection->getParagraphCache()->count() == 2,
                    "got cache count %d",
                    fontCollection->getParagraphCache()->count());

    TextStyle nanAxisStyle = contract_text_style("Roboto", 20.0f);
    const SkFontArguments::VariationPosition::Coordinate kNanCoord = {
            SkSetFourByteTag('w', 'g', 'h', 't'), SK_ScalarNaN};
    SkFontArguments fontArgs;
    fontArgs.setVariationDesignPosition({&kNanCoord, 1});
    nanAxisStyle.setFontArguments(fontArgs);
    for (int pass = 0; pass < 2; ++pass) {
        auto paragraph = layout_text(
                reporter, fontCollection, contract_paragraph_style(), nanAxisStyle, "Hello", 200);
        REPORTER_ASSERT(reporter,
                        paragraph->getMaxIntrinsicWidth() > 0,
                        "got %g",
                        paragraph->getMaxIntrinsicWidth());
    }
    // One entry for both passes: the second one hit the first one's entry
    REPORTER_ASSERT(reporter,
                    fontCollection->getParagraphCache()->count() == 3,
                    "got cache count %d",
                    fontCollection->getParagraphCache()->count());
}

// MT4: the alphabetic baseline is the first line's ascent and the ideographic one its ascent plus
// descent; getLineMetricsAt returns the getLineMetrics entry and false out of range
DEF_TEST(CoreTextContract_MT4_Baselines, reporter) {
    sk_sp<FontCollection> fontCollection = asset_collection({{"fonts/ahem.ttf", "Ahem"}});
    if (!fontCollection) {
        return;
    }
    auto paragraph = layout_text(reporter,
                                 fontCollection,
                                 contract_paragraph_style(),
                                 contract_text_style("Ahem", 10.0f),
                                 "a\nb",
                                 100.0f);
    const std::vector<LineMetrics> lines = line_metrics(paragraph.get());
    REPORTER_ASSERT(reporter, lines.size() == 2, "got %zu lines", lines.size());
    if (lines.size() != 2) {
        return;
    }
    REPORTER_ASSERT(reporter,
                    paragraph->getAlphabeticBaseline() == lines[0].fAscent,
                    "got %g, want %g",
                    paragraph->getAlphabeticBaseline(),
                    lines[0].fAscent);
    const SkScalar ideographic = lines[0].fAscent + lines[0].fDescent;
    REPORTER_ASSERT(reporter,
                    paragraph->getIdeographicBaseline() == ideographic,
                    "got %g, want %g",
                    paragraph->getIdeographicBaseline(),
                    ideographic);
    LineMetrics second;
    REPORTER_ASSERT(reporter, paragraph->getLineMetricsAt(1, &second));
    REPORTER_ASSERT(reporter,
                    second.fStartIndex == lines[1].fStartIndex,
                    "got %zu, want %zu",
                    second.fStartIndex,
                    lines[1].fStartIndex);
    REPORTER_ASSERT(reporter,
                    second.fBaseline == lines[1].fBaseline,
                    "got %g, want %g",
                    second.fBaseline,
                    lines[1].fBaseline);
    REPORTER_ASSERT(reporter,
                    second.fHeight == lines[1].fHeight,
                    "got %g, want %g",
                    second.fHeight,
                    lines[1].fHeight);
    LineMetrics outside;
    REPORTER_ASSERT(reporter, !paragraph->getLineMetricsAt(2, &outside));
    REPORTER_ASSERT(reporter, !paragraph->getLineMetricsAt(-1, &outside));
}

// MT5: the strut uses the first family of its list, a forced strut sets every line's height
// over taller text, and a disabled strut leaves the text's own height
DEF_TEST(CoreTextContract_MT5_StrutFirstFamily, reporter) {
    sk_sp<FontCollection> fontCollection =
            asset_collection({{"fonts/ahem.ttf", "Ahem"}, {"fonts/Roboto-Regular.ttf", "Roboto"}});
    if (!fontCollection) {
        return;
    }
    const TextStyle style = contract_text_style("Roboto", 20.0f);
    auto unstrutted = layout_text(
            reporter, fontCollection, contract_paragraph_style(), style, "Hello\nworld", 100);
    const SkScalar robotoHeight = unstrutted->getHeight() / 2;
    const auto kCases = std::to_array<std::tuple<const char*, const char*, bool, SkScalar>>({
            {"Ahem", "Roboto", true, 20},
            {"Roboto", "Ahem", true, robotoHeight},
            {"Ahem", "Roboto", false, robotoHeight},
    });
    REPORTER_ASSERT(reporter, robotoHeight != 20, "got %g", robotoHeight);
    for (const auto& [first, second, enabled, height] : kCases) {
        skiatest::ReporterContext ctx(
                reporter, SkStringPrintf("strut %s, %s, enabled %d", first, second, enabled));
        ParagraphStyle paragraphStyle = contract_paragraph_style();
        StrutStyle strut;
        strut.setStrutEnabled(enabled);
        strut.setForceStrutHeight(true);
        strut.setFontFamilies({SkString(first), SkString(second)});
        strut.setFontSize(20.0f);
        paragraphStyle.setStrutStyle(strut);
        auto paragraph =
                layout_text(reporter, fontCollection, paragraphStyle, style, "Hello\nworld", 100);
        for (const LineMetrics& line : line_metrics(paragraph.get())) {
            skiatest::ReporterContext lineCtx(reporter,
                                              SkStringPrintf("line %zu", line.fLineNumber));
            REPORTER_ASSERT(
                    reporter, line.fHeight == height, "got %g, want %g", line.fHeight, height);
        }
    }
}
