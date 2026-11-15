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

#include "include/core/SkFourByteTag.h"
#include "include/core/SkRefCnt.h"
#include "include/core/SkScalar.h"
#include "include/core/SkString.h"
#include "include/core/SkTypes.h"
#include "include/ports/SkTypeface_mac.h"
#include "include/private/SkTo.h"
#include "modules/skparagraph/include/DartTypes.h"
#include "modules/skparagraph/include/FontCollection.h"
#include "modules/skparagraph/include/Paragraph.h"
#include "modules/skparagraph/include/ParagraphStyle.h"
#include "modules/skparagraph/include/TextStyle.h"
#include "modules/skparagraph/include/TypefaceFontProvider.h"
#include "modules/skparagraph/tests/coretext/CoreTextTestUtils.h"
#include "tests/Test.h"
#include "tools/fonts/FontToolUtils.h"

#include <algorithm>
#include <array>
#include <vector>

using namespace skia::textlayout;
using namespace skia::textlayout::coretext_test;

// EL1: a cut paragraph reports didExceedMaxLines (also unlimited, with an ellipsis), fits any
// TextAlign, hides cut text from line and glyph info, and an ellipsis tap hits its end upstream
DEF_TEST(CoreTextContract_EL1_EllipsisFits, reporter) {
    if (!has_font_families({"Helvetica"})) {
        return;
    }
    const TextStyle style = contract_text_style("Helvetica", 20.0f);
    const char* text = "Hello world again and again";
    for (TextAlign align : {TextAlign::kLeft,
                            TextAlign::kRight,
                            TextAlign::kCenter,
                            TextAlign::kJustify,
                            TextAlign::kStart,
                            TextAlign::kEnd}) {
        skiatest::ReporterContext ctx(reporter,
                                      SkStringPrintf("align %d", static_cast<int>(align)));
        ParagraphStyle paragraphStyle = contract_paragraph_style();
        paragraphStyle.setMaxLines(1);
        paragraphStyle.setEllipsis(u"\u2026");
        paragraphStyle.setTextAlign(align);
        auto paragraph =
                layout_text(reporter, system_collection(), paragraphStyle, style, text, 100.0f);
        REPORTER_ASSERT(reporter, paragraph->didExceedMaxLines());
        REPORTER_ASSERT(reporter,
                        paragraph->getLongestLine() <= 100,
                        "got %g",
                        paragraph->getLongestLine());
    }

    ParagraphStyle paragraphStyle = contract_paragraph_style();
    paragraphStyle.setMaxLines(1);
    paragraphStyle.setEllipsis(u"\u2026");
    auto paragraph =
            layout_text(reporter, system_collection(), paragraphStyle, style, text, 100.0f);
    const std::vector<LineMetrics> lines = line_metrics(paragraph.get());
    REPORTER_ASSERT(reporter, lines.size() == 1, "got %zu lines", lines.size());
    if (lines.size() != 1) {
        return;
    }
    const size_t hidden = 20;
    REPORTER_ASSERT(reporter, lines[0].fEndIndex < hidden, "got %zu", lines[0].fEndIndex);
    REPORTER_ASSERT(reporter,
                    paragraph->getLineNumberAtUTF16Offset(hidden) == -1,
                    "got %d",
                    paragraph->getLineNumberAtUTF16Offset(hidden));
    Paragraph::GlyphInfo glyphInfo;
    REPORTER_ASSERT(reporter, !paragraph->getGlyphInfoAtUTF16Offset(hidden, &glyphInfo));
    const SkScalar ellipsisX = drawn_glyphs(paragraph.get()).back().position.fX;
    const PositionWithAffinity tap = paragraph->getGlyphPositionAtCoordinate(
            (ellipsisX + paragraph->getLongestLine()) / 2, 10);
    REPORTER_ASSERT(reporter,
                    tap.position == SkToInt(lines[0].fEndIndex),
                    "got %d, want %zu",
                    tap.position,
                    lines[0].fEndIndex);
    REPORTER_ASSERT(reporter, tap.affinity == Affinity::kUpstream);

    ParagraphStyle unlimited = contract_paragraph_style();
    unlimited.setEllipsis(u"\u2026");
    auto unlimitedParagraph =
            layout_text(reporter, system_collection(), unlimited, style, text, 100.0f);
    REPORTER_ASSERT(reporter, unlimitedParagraph->didExceedMaxLines());
}

// EL1 / R128: a Default_Ignorable ellipsis (U+FEFF, U+200D, or LastResort-cascaded U+3164) remaps
// to an in-range invisible zero-advance glyph instead of 0xFFFF or a .notdef tofu box
DEF_TEST(CoreTextContract_EL1_IgnorableEllipsisZeroGlyphs, reporter) {
    auto checkIgnorableEllipsis = [&](const std::vector<DrawnGlyph>& glyphs, TextDirection dir) {
        REPORTER_ASSERT(reporter, !glyphs.empty());
        if (glyphs.empty()) {
            return;
        }
        for (const DrawnGlyph& g : glyphs) {
            const int glyphLimit = g.font.getTypeface()->countGlyphs();
            REPORTER_ASSERT(reporter,
                            g.glyph != 0 && g.glyph < glyphLimit,
                            "got glyph %u, limit %d",
                            g.glyph,
                            glyphLimit);
        }
        const DrawnGlyph& ellipGlyph = dir == TextDirection::kLtr ? glyphs.back() : glyphs.front();
        const SkGlyphID spaceGlyph = ellipGlyph.font.unicharToGlyph(' ');
        REPORTER_ASSERT(reporter,
                        ellipGlyph.glyph == spaceGlyph,
                        "got glyph %u, want space %u",
                        ellipGlyph.glyph,
                        spaceGlyph);
    };

    if (has_font_families({".AppleSystemUIFont"})) {
        const sk_sp<FontCollection> sysCollection = system_collection();
        for (TextDirection dir : {TextDirection::kLtr, TextDirection::kRtl}) {
            skiatest::ReporterContext context(reporter, SkStringPrintf("dir %d", (int)dir));
            ParagraphStyle paragraphStyle = contract_paragraph_style();
            paragraphStyle.setTextDirection(dir);
            paragraphStyle.setMaxLines(1);
            paragraphStyle.setEllipsis(SkString("\xEF\xBB\xBF"));
            auto paragraph = layout_text(reporter,
                                         sysCollection,
                                         paragraphStyle,
                                         contract_text_style(".AppleSystemUIFont", 20.0f),
                                         "Hello world long tail",
                                         80.0f);
            REPORTER_ASSERT(reporter, paragraph->didExceedMaxLines());
            const std::vector<LineMetrics> lines = line_metrics(paragraph.get());
            REPORTER_ASSERT(reporter, lines.size() == 1, "got %zu lines", lines.size());
            if (lines.size() != 1) {
                continue;
            }
            REPORTER_ASSERT(reporter, lines[0].fWidth <= 80.0f, "got %g", lines[0].fWidth);
            checkIgnorableEllipsis(drawn_glyphs(paragraph.get()), dir);
            const SkScalar edgeX = dir == TextDirection::kLtr ? 1000.0f : 0.0f;
            const PositionWithAffinity hit = paragraph->getGlyphPositionAtCoordinate(edgeX, 10.0f);
            REPORTER_ASSERT(reporter,
                            hit.position == SkToS32(lines[0].fEndIndex),
                            "got %d, want %zu",
                            hit.position,
                            lines[0].fEndIndex);
        }
    }

    sk_sp<FontCollection> robotoCollection =
            asset_collection({{"fonts/Roboto-Regular.ttf", "Roboto"}});
    if (!robotoCollection) {
        return;
    }
    const TextStyle robotoStyle = contract_text_style("Roboto", 20.0f);
    auto plainHello = layout_text(
            reporter, robotoCollection, contract_paragraph_style(), robotoStyle, "Hello", 200.0f);
    for (const char* ellipsis :
         {"\xE2\x80\x8D", "\xE1\x85\x9F", "\xE3\x85\xA4", "\xEF\xBE\xA0", "\xF3\xA0\x80\x81"}) {
        skiatest::ReporterContext context(reporter, SkString(ellipsis));
        ParagraphStyle noFallbackStyle = contract_paragraph_style();
        noFallbackStyle.setMaxLines(1);
        noFallbackStyle.setEllipsis(SkString(ellipsis));
        auto robotoParagraph = layout_text(
                reporter, robotoCollection, noFallbackStyle, robotoStyle, "Hello\nworld", 200.0f);
        REPORTER_ASSERT(reporter, robotoParagraph->didExceedMaxLines());
        checkIgnorableEllipsis(drawn_glyphs(robotoParagraph.get()), TextDirection::kLtr);
        REPORTER_ASSERT(
                reporter,
                SkScalarNearlyEqual(
                        robotoParagraph->getLongestLine(), plainHello->getLongestLine(), 0.05f),
                "got %g, want %g",
                robotoParagraph->getLongestLine(),
                plainHello->getLongestLine());
    }
}

// EL2: the ellipsis is shaped like text in its font: a kerned "AV" ellipsis is as wide as the
// paragraph "AV", and narrower than A and V apart
DEF_TEST(CoreTextContract_EL2_EllipsisShapedLikeText, reporter) {
    if (!has_font_families({"Helvetica"})) {
        return;
    }
    const TextStyle style = contract_text_style("Helvetica", 20.0f);
    const ParagraphStyle plain = contract_paragraph_style();
    auto layout = [&](const char* text) {
        return layout_text(reporter, system_collection(), plain, style, text, 1000);
    };
    const SkScalar kerned = layout("AV")->getMaxIntrinsicWidth();
    const SkScalar apart =
            layout("A")->getMaxIntrinsicWidth() + layout("V")->getMaxIntrinsicWidth();
    REPORTER_ASSERT(reporter, kerned < apart, "got %g, apart %g", kerned, apart);

    ParagraphStyle paragraphStyle = contract_paragraph_style();
    paragraphStyle.setMaxLines(1);
    paragraphStyle.setEllipsis(u"AV");
    auto paragraph = layout_text(reporter,
                                 system_collection(),
                                 paragraphStyle,
                                 style,
                                 "Hello world again and again",
                                 100.0f);
    const std::vector<DrawnGlyph> glyphs = drawn_glyphs(paragraph.get());
    REPORTER_ASSERT(reporter, glyphs.size() > 2, "got %zu", glyphs.size());
    if (glyphs.size() <= 2) {
        return;
    }
    const SkScalar ellipsisWidth =
            paragraph->getLongestLine() - glyphs[glyphs.size() - 2].position.fX;
    REPORTER_ASSERT(reporter,
                    SkScalarNearlyEqual(ellipsisWidth, kerned, 1e-3f),
                    "got %g, want %g",
                    ellipsisWidth,
                    kerned);
}

// EL3: in an RTL paragraph the ellipsis ends the line on the left (Flutter #117831, #113453)
DEF_TEST(CoreTextContract_EL3_RtlEllipsisLeft, reporter) {
    if (!has_font_families({"Arial Hebrew"})) {
        return;
    }
    ParagraphStyle paragraphStyle = contract_paragraph_style();
    paragraphStyle.setMaxLines(1);
    paragraphStyle.setEllipsis(u"\u2026");
    paragraphStyle.setTextDirection(TextDirection::kRtl);
    auto paragraph = layout_text(reporter,
                                 system_collection(),
                                 paragraphStyle,
                                 contract_text_style("Arial Hebrew", 20.0f),
                                 "\u05e9\u05dc\u05d5\u05dd \u05e2\u05d5\u05dc\u05dd "
                                 "\u05e9\u05dc\u05d5\u05dd \u05e2\u05d5\u05dc\u05dd",
                                 100.0f);
    REPORTER_ASSERT(reporter, paragraph->didExceedMaxLines());
    const std::vector<DrawnGlyph> glyphs = drawn_glyphs(paragraph.get());
    int ellipses = 0;
    SkScalar ellipsisX = 0;
    SkScalar textMinX = SK_ScalarInfinity;
    for (const DrawnGlyph& glyph : glyphs) {
        if (glyph.glyph == glyph.font.unicharToGlyph(0x2026)) {
            ++ellipses;
            ellipsisX = glyph.position.fX;
        } else {
            textMinX = std::min(textMinX, glyph.position.fX);
        }
    }
    REPORTER_ASSERT(reporter, ellipses == 1, "got %d", ellipses);
    REPORTER_ASSERT(reporter, ellipsisX < textMinX, "got %g, want < %g", ellipsisX, textMinX);
}

// EL4: maxLines that cuts only the empty line after a trailing '\n' does not count as exceeded,
// and kDisableLastDescent applies to the last visible line
DEF_TEST(CoreTextContract_EL4_TrailingNewlineNotExceeded, reporter) {
    sk_sp<FontCollection> fontCollection = asset_collection({{"fonts/ahem.ttf", "Ahem"}});
    if (!fontCollection) {
        return;
    }
    for (size_t maxLines : {1, 2}) {
        skiatest::ReporterContext ctx(reporter, SkStringPrintf("maxLines %zu", maxLines));
        ParagraphStyle paragraphStyle = contract_paragraph_style();
        paragraphStyle.setMaxLines(maxLines);
        auto paragraph = layout_text(reporter,
                                     fontCollection,
                                     paragraphStyle,
                                     contract_text_style("Ahem", 10.0f),
                                     "Single line\n",
                                     1000.0f);
        REPORTER_ASSERT(
                reporter, paragraph->lineNumber() == maxLines, "got %zu", paragraph->lineNumber());
        REPORTER_ASSERT(reporter, !paragraph->didExceedMaxLines());
    }

    ParagraphStyle paragraphStyle = contract_paragraph_style();
    paragraphStyle.setMaxLines(1);
    paragraphStyle.setTextHeightBehavior(TextHeightBehavior::kDisableLastDescent);
    TextStyle style = contract_text_style("Ahem", 10.0f);
    style.setHeight(2.0f);
    style.setHeightOverride(true);
    auto paragraph = layout_text(reporter, fontCollection, paragraphStyle, style, "a\nb", 1000.0f);
    // Height 2 gives 16 + 4; without the last descent's extra 2 the only visible line is 18
    REPORTER_ASSERT(reporter, paragraph->getHeight() == 18, "got %g", paragraph->getHeight());
}

namespace {

// Asset faces for the affix retry cases: OnlyA0 maps only ! " 0-4 A, Roboto maps U+2026, and Ahem
// maps U+F000, which no macOS system font covers
sk_sp<FontCollection> affix_retry_collection(bool fallbackEnabled) {
    sk_sp<SkTypeface> onlyA0 = ToolUtils::CreateTypefaceFromResource("fonts/test.ttc", 0);
    sk_sp<SkTypeface> roboto = ToolUtils::CreateTypefaceFromResource("fonts/Roboto-Regular.ttf");
    sk_sp<SkTypeface> ahem = ToolUtils::CreateTypefaceFromResource("fonts/ahem.ttf");
    if (!onlyA0 || !roboto || !ahem) {
        return nullptr;
    }
    auto provider = sk_make_sp<TypefaceFontProvider>();
    provider->registerTypeface(std::move(onlyA0), SkString("OnlyA0"));
    provider->registerTypeface(std::move(roboto), SkString("Roboto"));
    provider->registerTypeface(std::move(ahem), SkString("Ahem"));
    auto fonts = sk_make_sp<FontCollection>();
    fonts->setAssetFontManager(provider);
    if (!fallbackEnabled) {
        fonts->disableFontFallback();
    }
    return fonts;
}

// One RTL line cut at '\n': RLO in clusterless Block 0 (OnlyA0) makes it the AffixBlock style,
// while the cut cluster "b" resolves in clusterFamily
std::unique_ptr<Paragraph> layout_rtl_affix_case(const sk_sp<FontCollection>& fonts,
                                                 const char* clusterFamily,
                                                 const std::u16string& ellipsis,
                                                 TextShapingEngine engine) {
    ParagraphStyle ps = contract_paragraph_style();
    ps.setTextShapingEngine(engine);
    ps.setTextDirection(TextDirection::kRtl);
    ps.setMaxLines(1);
    ps.setEllipsis(ellipsis);
    ParagraphBuilderImpl builder(ps, fonts, contract_unicode());
    builder.pushStyle(contract_text_style("OnlyA0", 20.0f));
    builder.addText("\u202E");
    builder.pop();
    builder.pushStyle(contract_text_style(clusterFamily, 20.0f));
    builder.addText("b\nb");
    builder.pop();
    auto p = builder.Build();
    p->layout(100.0f);
    return p;
}

#if !defined(SK_PARAGRAPH_DISABLE_HARFBUZZ)
// Sorted ids of the first count drawn glyphs: an RTL line paints its ellipsis first, and kDefault
// shapes it without reordering, so only the glyph set compares across engines
std::vector<SkGlyphID> sorted_leading_glyphs(Paragraph* paragraph, size_t count) {
    std::vector<SkGlyphID> ids = drawn_glyph_ids(paragraph);
    ids.resize(std::min(ids.size(), count));
    std::sort(ids.begin(), ids.end());
    return ids;
}

void check_leading_glyphs_like_default(skiatest::Reporter* reporter,
                                       const sk_sp<FontCollection>& fonts,
                                       const char* clusterFamily,
                                       const std::u16string& ellipsis) {
    if (!fonts) {
        return;
    }
    auto ct = layout_rtl_affix_case(fonts, clusterFamily, ellipsis, TextShapingEngine::kCoreText);
    auto hb = layout_rtl_affix_case(fonts, clusterFamily, ellipsis, TextShapingEngine::kDefault);
    const std::vector<SkGlyphID> ctIds = sorted_leading_glyphs(ct.get(), ellipsis.size());
    const std::vector<SkGlyphID> hbIds = sorted_leading_glyphs(hb.get(), ellipsis.size());
    REPORTER_ASSERT(reporter, ctIds.size() == ellipsis.size(), "got %zu", ctIds.size());
    REPORTER_ASSERT(reporter, hbIds.size() == ellipsis.size(), "got %zu", hbIds.size());
    for (size_t i = 0; i < std::min(ctIds.size(), hbIds.size()); ++i) {
        REPORTER_ASSERT(reporter, ctIds[i] != 0, "got unresolved glyph at %zu", i);
        REPORTER_ASSERT(reporter, ctIds[i] == hbIds[i], "got %u, want %u", ctIds[i], hbIds[i]);
    }
}
#endif

}  // namespace

// EL1: with fallback disabled, an ellipsis whose AffixBlock style lacks U+2026 still resolves in
// the truncated cluster's face (as kDefault does), and an explicit affix typeface resolves directly
DEF_TEST(CoreTextContract_EL1_DisabledFallbackClusterAndExplicitTypeface, reporter) {
    sk_sp<FontCollection> fonts = affix_retry_collection(false);
    sk_sp<SkTypeface> roboto = ToolUtils::CreateTypefaceFromResource("fonts/Roboto-Regular.ttf");
    if (!fonts || !roboto) {
        return;
    }

    auto ctPara = layout_rtl_affix_case(fonts, "Roboto", u"\u2026", TextShapingEngine::kCoreText);
    REPORTER_ASSERT(reporter, ctPara->didExceedMaxLines());
    const std::vector<DrawnGlyph> ctGlyphs = drawn_glyphs(ctPara.get());
    REPORTER_ASSERT(reporter, !ctGlyphs.empty(), "got %zu glyphs", ctGlyphs.size());
    if (!ctGlyphs.empty()) {
        // RTL lines paint their ellipsis run first on the left
        const SkGlyphID ellipGlyph = ctGlyphs.front().glyph;
        REPORTER_ASSERT(reporter, ellipGlyph != 0, "got unresolved glyph %u", ellipGlyph);
    }
#if !defined(SK_PARAGRAPH_DISABLE_HARFBUZZ)
    auto hbPara = layout_rtl_affix_case(fonts, "Roboto", u"\u2026", TextShapingEngine::kDefault);
    const std::vector<DrawnGlyph> hbGlyphs = drawn_glyphs(hbPara.get());
    REPORTER_ASSERT(reporter, !hbGlyphs.empty(), "got %zu glyphs", hbGlyphs.size());
    if (!ctGlyphs.empty() && !hbGlyphs.empty()) {
        REPORTER_ASSERT(reporter,
                        ctGlyphs.front().glyph == hbGlyphs.front().glyph,
                        "got %u, want %u",
                        ctGlyphs.front().glyph,
                        hbGlyphs.front().glyph);
    }
#endif

    ParagraphStyle explicitPs = contract_paragraph_style();
    explicitPs.setMaxLines(1);
    explicitPs.setEllipsis(u"\u2026");
    TextStyle explicitRoboto = contract_text_style("NoSuchFamily", 20.0f);
    explicitRoboto.setTypeface(roboto);
    auto explicitPara = layout_text(
            reporter, fonts, explicitPs, explicitRoboto, "Hello world again and again", 80.0f);
    const std::vector<DrawnGlyph> explicitGlyphs = drawn_glyphs(explicitPara.get());
    REPORTER_ASSERT(reporter, !explicitGlyphs.empty(), "got %zu glyphs", explicitGlyphs.size());
    if (!explicitGlyphs.empty()) {
        REPORTER_ASSERT(reporter,
                        explicitGlyphs.back().glyph != 0,
                        "got unresolved glyph %u",
                        explicitGlyphs.back().glyph);
    }
}

// EL1: an ellipsis inheriting or retrying a varied system UI clusterTypeface keeps its variation
// axes (opsz and wght) both on a default-family span and across an unvaried AffixBlock retry
DEF_TEST(CoreTextContract_EL1_SystemFontEllipsisKeepsVariation, reporter) {
    if (!has_font_families({".AppleSystemUIFont"})) {
        return;
    }
    const SkFourByteTag wght = SkSetFourByteTag('w', 'g', 'h', 't');
    const SkFourByteTag opsz = SkSetFourByteTag('o', 'p', 's', 'z');
    const std::array<Coordinate, 2> coords = {{{wght, 650.0f}, {opsz, 40.0f}}};
    SkFontArguments args;
    args.setVariationDesignPosition({coords.data(), SkToInt(coords.size())});

    ParagraphStyle ps = contract_paragraph_style();
    ps.setMaxLines(1);
    ps.setEllipsis(u"\u2026");
    TextStyle defaultSysStyle;
    defaultSysStyle.setFontFamilies({});
    defaultSysStyle.setFontSize(14.0f);
    defaultSysStyle.setFontArguments(args);
    auto single = layout_text(
            reporter, system_collection(), ps, defaultSysStyle, "Hamburgefonstiv\nb", 500.0f);
    REPORTER_ASSERT(reporter, single->didExceedMaxLines());
    const std::vector<DrawnGlyph> singleGlyphs = drawn_glyphs(single.get());
    REPORTER_ASSERT(reporter, singleGlyphs.size() == 16, "got %zu glyphs", singleGlyphs.size());
    if (!singleGlyphs.empty()) {
        const SkGlyphID wantEllipsis = singleGlyphs.back().font.unicharToGlyph(0x2026);
        REPORTER_ASSERT(reporter,
                        singleGlyphs.back().glyph == wantEllipsis,
                        "got %u, want %u",
                        singleGlyphs.back().glyph,
                        wantEllipsis);
        const SkTypeface* tf = singleGlyphs.back().font.getTypeface();
        const SkScalar gotOpsz = axis_coordinate(tf, opsz).value_or(-1.0f);
        const SkScalar gotWght = axis_coordinate(tf, wght).value_or(-1.0f);
        REPORTER_ASSERT(reporter,
                        SkScalarNearlyEqual(gotOpsz, 40.0f, 0.5f),
                        "single got opsz %g, want 40",
                        gotOpsz);
        REPORTER_ASSERT(reporter,
                        SkScalarNearlyEqual(gotWght, 650.0f, 0.5f),
                        "single got wght %g, want 650",
                        gotWght);
    }

    sk_sp<FontCollection> noFallback = affix_retry_collection(false);
    SkUniqueCFRef<CTFontRef> ctSys(
            CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, 14.0, nullptr));
    sk_sp<SkTypeface> sysTf = ctSys ? SkMakeTypefaceFromCTFont(ctSys.get()) : nullptr;
    if (!noFallback || !sysTf) {
        return;
    }
    auto emptyNoFallback = sk_make_sp<FontCollection>();
    emptyNoFallback->disableFontFallback();
    ParagraphStyle rtlPs = ps;
    rtlPs.setTextDirection(TextDirection::kRtl);
    TextStyle clusterStyle;
    clusterStyle.setFontSize(14.0f);
    clusterStyle.setTypeface(sysTf);
    clusterStyle.setFontArguments(args);
    struct RetryCase {
        const char* label;
        sk_sp<FontCollection> collection;
        const char* prefixFamily;
    };
    const std::array<RetryCase, 2> retryCases = {{
            {"onlyA0", noFallback, "OnlyA0"},
            {"emptyStandIn", emptyNoFallback, "NoSuchFamily"},
    }};
    for (const RetryCase& c : retryCases) {
        skiatest::ReporterContext ctx(reporter, c.label);
        ParagraphBuilderImpl builder(rtlPs, c.collection, contract_unicode());
        builder.pushStyle(contract_text_style(c.prefixFamily, 14.0f));
        builder.addText("\u202E");
        builder.pop();
        builder.pushStyle(clusterStyle);
        builder.addText("b\nb");
        builder.pop();
        auto retryPara = builder.Build();
        retryPara->layout(100.0f);
        REPORTER_ASSERT(reporter, retryPara->didExceedMaxLines());
        const std::vector<DrawnGlyph> retryGlyphs = drawn_glyphs(retryPara.get());
        REPORTER_ASSERT(reporter, !retryGlyphs.empty(), "got %zu glyphs", retryGlyphs.size());
        if (!retryGlyphs.empty()) {
            const SkGlyphID wantEllipsis = retryGlyphs.front().font.unicharToGlyph(0x2026);
            REPORTER_ASSERT(reporter,
                            retryGlyphs.front().glyph != 0 &&
                                    retryGlyphs.front().glyph == wantEllipsis,
                            "got %u, want %u",
                            retryGlyphs.front().glyph,
                            wantEllipsis);
            const SkTypeface* tf = retryGlyphs.front().font.getTypeface();
            const SkScalar gotOpsz = axis_coordinate(tf, opsz).value_or(-1.0f);
            const SkScalar gotWght = axis_coordinate(tf, wght).value_or(-1.0f);
            REPORTER_ASSERT(reporter,
                            SkScalarNearlyEqual(gotOpsz, 40.0f, 0.5f),
                            "retry got opsz %g, want 40",
                            gotOpsz);
            REPORTER_ASSERT(reporter,
                            SkScalarNearlyEqual(gotWght, 650.0f, 0.5f),
                            "retry got wght %g, want 650",
                            gotWght);
        }
    }
}

#if !defined(SK_PARAGRAPH_DISABLE_HARFBUZZ)
// EL1: with fallback disabled, a disallowed CTRun after an allowed one ("A" resolves in OnlyA0,
// U+2026 does not) still retries the cut cluster's face, as kDefault does for the whole ellipsis
DEF_TEST(CoreTextContract_EL1_DisabledFallbackRetriesLaterDisallowedRun, reporter) {
    check_leading_glyphs_like_default(
            reporter, affix_retry_collection(false), "Roboto", u"A\u2026");
}

// EL1: when an ellipsis splits across two allowed faces ("A" in OnlyA0, U+2026 in Roboto or the
// system cascade), ShapeEllipsis retries clusterTypeface instead of zeroing the secondary face
DEF_TEST(CoreTextContract_EL1_SplitAllowedAffixRetriesClusterTypeface, reporter) {
    sk_sp<FontCollection> noFallback = affix_retry_collection(false);
    if (!noFallback) {
        return;
    }
    auto runCascadeCase = [&](TextShapingEngine engine) {
        ParagraphStyle ps = contract_paragraph_style();
        ps.setTextShapingEngine(engine);
        ps.setMaxLines(1);
        ps.setEllipsis(u"A\u2026");
        TextStyle style;
        style.setFontSize(20.0f);
        style.setFontFamilies({SkString("OnlyA0"), SkString("Roboto")});
        ParagraphBuilderImpl builder(ps, noFallback, contract_unicode());
        builder.pushStyle(style);
        builder.addText("b\nb");
        builder.pop();
        auto p = builder.Build();
        p->layout(100.0f);
        return p;
    };
    auto ct = runCascadeCase(TextShapingEngine::kCoreText);
    auto hb = runCascadeCase(TextShapingEngine::kDefault);
    const std::vector<SkGlyphID> ctIds = drawn_glyph_ids(ct.get());
    const std::vector<SkGlyphID> hbIds = drawn_glyph_ids(hb.get());
    REPORTER_ASSERT(reporter, ctIds.size() == 3, "got %zu glyphs", ctIds.size());
    REPORTER_ASSERT(reporter, hbIds.size() == 3, "got %zu glyphs", hbIds.size());
    for (size_t i = 0; i < std::min(ctIds.size(), hbIds.size()); ++i) {
        REPORTER_ASSERT(reporter, ctIds[i] != 0, "got unresolved glyph at %zu", i);
        REPORTER_ASSERT(reporter, ctIds[i] == hbIds[i], "got %u, want %u", ctIds[i], hbIds[i]);
    }

    if (has_font_families({".AppleSystemUIFont"})) {
        sk_sp<FontCollection> withSystem = affix_retry_collection(true);
        if (withSystem) {
            withSystem->setDefaultFontManager(ToolUtils::TestFontMgr());
            check_leading_glyphs_like_default(
                    reporter, withSystem, ".AppleSystemUIFont", u"A\u2026");
        }
    }
}

// EL1: with fallback enabled, a code point the system cascade sends to LastResort but the cut
// cluster's face maps (Ahem U+F000) resolves in that face, as in kDefault
DEF_TEST(CoreTextContract_EL1_LastResortAffixRetriesClusterTypeface, reporter) {
    check_leading_glyphs_like_default(reporter, affix_retry_collection(true), "Ahem", u"\uF000");
}

// EL1: when the cut cluster's face resolves no more of the ellipsis than the AffixBlock face
// (unassigned U+0378), the run keeps the AffixBlock face, which kDefault also tries last
DEF_TEST(CoreTextContract_EL1_FailedClusterRetryKeepsAffixFace, reporter) {
    sk_sp<FontCollection> fonts = affix_retry_collection(false);
    if (!fonts) {
        return;
    }
    auto ct = layout_rtl_affix_case(fonts, "Roboto", u"\u0378", TextShapingEngine::kCoreText);
    auto hb = layout_rtl_affix_case(fonts, "Roboto", u"\u0378", TextShapingEngine::kDefault);
    const std::vector<DrawnGlyph> ctGlyphs = drawn_glyphs(ct.get());
    const std::vector<DrawnGlyph> hbGlyphs = drawn_glyphs(hb.get());
    REPORTER_ASSERT(reporter, !ctGlyphs.empty() && !hbGlyphs.empty());
    if (ctGlyphs.empty() || hbGlyphs.empty()) {
        return;
    }
    const SkString ctFamily = family_name(ctGlyphs.front().font);
    const SkString hbFamily = family_name(hbGlyphs.front().font);
    REPORTER_ASSERT(reporter,
                    ctFamily.equals(hbFamily),
                    "got %s, want %s",
                    ctFamily.c_str(),
                    hbFamily.c_str());
}
#endif
