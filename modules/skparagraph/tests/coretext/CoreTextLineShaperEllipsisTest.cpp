/*
 * Copyright 2026 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "include/core/SkFont.h"
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
#include "modules/skparagraph/src/Run.h"
#include "modules/skparagraph/tests/coretext/CoreTextTestUtils.h"
#include "tests/Test.h"
#include "tools/fonts/FontToolUtils.h"

#include <string.h>

#include <algorithm>
#include <array>
#include <memory>
#include <vector>

using namespace skia::textlayout;
using namespace skia::textlayout::coretext_test;

// Tests assert the internal ParagraphImpl::isCoreTextShaped(): no public API reports the engine,
// and a silent HarfBuzz fallback would pass every other assert

// The ellipsis of a truncated CoreText line is shaped and resolved through CoreText, so it
// still appears in a build without HarfBuzz
DEF_TEST(CoreTextPipeline_EllipsisShapedByCoreText, reporter) {
    if (!has_font_families({"Helvetica"})) {
        return;
    }
    auto fontCollection = test_font_collection();
    ParagraphStyle style;
    style.setTextShapingEngine(TextShapingEngine::kCoreText);
    style.setMaxLines(1);
    style.setEllipsis(u"\u2026");
    ParagraphBuilderImpl builder(style, fontCollection, get_unicode(style));
    TextStyle textStyle;
    textStyle.setFontFamilies({SkString("Helvetica")});
    textStyle.setFontSize(20);
    builder.pushStyle(textStyle);
    builder.addText("Ellipsis must survive a build without HarfBuzz");
    builder.pop();
    auto paragraph = builder.Build();
    paragraph->layout(150);
    auto impl = static_cast<ParagraphImpl*>(paragraph.get());
    REPORTER_ASSERT(reporter, impl->isCoreTextShaped());
    REPORTER_ASSERT(reporter, paragraph->didExceedMaxLines());
    REPORTER_ASSERT(reporter, impl->lines().size() == 1, "got %zu lines", impl->lines().size());
    if (impl->lines().size() != 1) {
        return;
    }
    const Run* ellipsis = impl->lines()[0].ellipsis();
    REPORTER_ASSERT(reporter,
                    ellipsis && ellipsis->size() == 1 && ellipsis->isResolved(),
                    "got %zu glyphs",
                    ellipsis ? ellipsis->size() : 0);
    REPORTER_ASSERT(reporter,
                    paragraph->getLongestLine() <= 150,
                    "ellipsized line is %g wide",
                    paragraph->getLongestLine());
}

// With font fallback disabled an ellipsis glyph the requested face has stays visible when an
// earlier ellipsis character falls back to a hidden system font, on both engines
DEF_TEST(CoreTextPipeline_DisabledFallbackEllipsisKeepsRequestedFaceGlyph, reporter) {
    sk_sp<SkTypeface> roboto = ToolUtils::CreateTypefaceFromResource("fonts/Roboto-Regular.ttf");
    if (!roboto) {
        return;
    }
    auto provider = sk_make_sp<TypefaceFontProvider>();
    provider->registerTypeface(roboto, SkString("Roboto"));
    auto fontCollection = sk_make_sp<FontCollection>();
    fontCollection->setDefaultFontManager(ToolUtils::TestFontMgr());
    fontCollection->setDynamicFontManager(provider);
    fontCollection->disableFontFallback();

    // A HarfBuzz-free build aborts on kDefault, so only the CoreText half can run there
    constexpr TextShapingEngine kEngines[] = {
#if !defined(SK_PARAGRAPH_DISABLE_HARFBUZZ) && defined(SK_UNICODE_ICU_IMPLEMENTATION)
            TextShapingEngine::kDefault,
#endif
            TextShapingEngine::kCoreText};
    for (TextShapingEngine engine : kEngines) {
        ParagraphStyle paragraphStyle;
        paragraphStyle.setTextShapingEngine(engine);
        paragraphStyle.setMaxLines(1);
        paragraphStyle.setEllipsis(u"\U0001F600\u2026");
        ParagraphBuilderImpl builder(paragraphStyle, fontCollection, get_unicode(paragraphStyle));
        TextStyle style;
        style.setFontFamilies({SkString("Roboto")});
        style.setFontSize(20);
        builder.pushStyle(style);
        builder.addText("A line long enough to need an ellipsis at this width");
        builder.pop();
        auto paragraph = builder.Build();
        paragraph->layout(200);
        REPORTER_ASSERT(reporter, paragraph->didExceedMaxLines());

        // The ellipsis is the last run TextLine visits on the truncated LTR line
        std::vector<SkGlyphID> ellipsisGlyphs;
        paragraph->visit([&](int lineNumber, const Paragraph::VisitorInfo* info) {
            if (lineNumber == 0 && info) {
                ellipsisGlyphs.assign(info->glyphs, info->glyphs + info->count);
            }
        });
        const auto resolved = std::count_if(ellipsisGlyphs.begin(),
                                            ellipsisGlyphs.end(),
                                            [](SkGlyphID glyph) { return glyph != 0; });
        REPORTER_ASSERT(reporter,
                        ellipsisGlyphs.size() == 2 && resolved == 1,
                        "engine %d: %zu ellipsis glyphs, %zd resolved",
                        static_cast<int>(engine),
                        ellipsisGlyphs.size(),
                        static_cast<ptrdiff_t>(resolved));
    }
}

// A letter-spaced ellipsis mixing Latin and Arabic in one face keeps every glyph: the cursive
// CTRun gets the same face without liga=0, and that feature difference alone must not mark it
// as a fallback run whose glyphs become 0
DEF_TEST(CoreTextPipeline_LetterSpacedMixedScriptEllipsisKeepsGlyphs, reporter) {
    if (!has_font_families({"Arial"})) {
        return;
    }
    ParagraphStyle paragraphStyle;
    paragraphStyle.setTextShapingEngine(TextShapingEngine::kCoreText);
    paragraphStyle.setMaxLines(1);
    // "fi " then Allah (alef, lam, lam, heh), which Arial draws as one ligature
    paragraphStyle.setEllipsis(u"fi \u0627\u0644\u0644\u0647");
    ParagraphBuilderImpl builder(
            paragraphStyle, test_font_collection(), get_unicode(paragraphStyle));
    TextStyle style;
    style.setFontFamilies({SkString("Arial")});
    style.setFontSize(20);
    style.setLetterSpacing(2.0f);
    builder.pushStyle(style);
    builder.addText("A line long enough to need an ellipsis at this width");
    builder.pop();
    auto paragraph = builder.Build();
    paragraph->layout(300);
    REPORTER_ASSERT(reporter, paragraph->didExceedMaxLines());

    // The ellipsis is the last run TextLine visits on the truncated LTR line
    std::vector<SkGlyphID> ellipsisGlyphs;
    paragraph->visit([&](int lineNumber, const Paragraph::VisitorInfo* info) {
        if (lineNumber == 0 && info) {
            ellipsisGlyphs.assign(info->glyphs, info->glyphs + info->count);
        }
    });
    const auto unresolved = std::count(ellipsisGlyphs.begin(), ellipsisGlyphs.end(), 0);
    REPORTER_ASSERT(reporter,
                    !ellipsisGlyphs.empty() && unresolved == 0,
                    "%zu ellipsis glyphs, %zd are 0",
                    ellipsisGlyphs.size(),
                    static_cast<ptrdiff_t>(unresolved));
}

// Multi-run ellipsis (e.g. Latin + Emoji or fallback characters) must coalesce into a single
// RunHandler buffer, painted as one blob of all its glyphs, without triggering SkASSERT(!fRun) in
// TextLine::ShapeString
DEF_TEST(CoreTextPipeline_MultiRunEllipsisAndAdjacentStyleRuns, reporter) {
    if (!has_font_families({"Helvetica", "Apple Color Emoji"})) {
        return;
    }
    auto fc = test_font_collection();
    ParagraphStyle ps;
    ps.setTextShapingEngine(TextShapingEngine::kCoreText);
    ps.setMaxLines(1);
    ps.setEllipsis(SkString("...\xF0\x9F\x98\x80"));

    ParagraphBuilderImpl builder(ps, fc, get_unicode(ps));
    TextStyle regular;
    regular.setFontFamilies({SkString("Helvetica")});
    regular.setFontSize(18.0f);
    regular.setFontEdging(SkFont::Edging::kAlias);
    builder.pushStyle(regular);
    builder.addText("First span ");
    builder.pop();

    TextStyle subpixelBold = regular;
    subpixelBold.setFontEdging(SkFont::Edging::kSubpixelAntiAlias);
    builder.pushStyle(subpixelBold);
    builder.addText("Second span that is long enough to trigger ellipsis truncation");
    builder.pop();

    auto p = builder.Build();
    p->layout(140.0f);

    REPORTER_ASSERT(reporter, static_cast<ParagraphImpl*>(p.get())->isCoreTextShaped());
    REPORTER_ASSERT(reporter, p->didExceedMaxLines());
    const std::vector<Paragraph::FontInfo> fonts = p->getFonts();
    REPORTER_ASSERT(reporter, fonts.size() >= 2, "got %zu runs", fonts.size());
    if (fonts.size() < 2) {
        return;
    }
    REPORTER_ASSERT(reporter, fonts[0].fFont.getEdging() == SkFont::Edging::kAlias);
    REPORTER_ASSERT(reporter, fonts[1].fFont.getEdging() == SkFont::Edging::kSubpixelAntiAlias);
    const std::vector<SkScalar> ellipsisX = last_blob_x(p.get());
    REPORTER_ASSERT(reporter, ellipsisX.size() == 4, "got %zu glyphs", ellipsisX.size());
}

namespace {
// A one-line RTL Hebrew paragraph in family that overflows, so it ends in ellipsis
std::unique_ptr<Paragraph> layout_hebrew_ellipsis(const char* family, const char* ellipsis) {
    ParagraphStyle paragraphStyle;
    paragraphStyle.setTextShapingEngine(TextShapingEngine::kCoreText);
    paragraphStyle.setTextDirection(TextDirection::kRtl);
    paragraphStyle.setMaxLines(1);
    paragraphStyle.setEllipsis(SkString(ellipsis));
    TextStyle style;
    style.setFontFamilies({SkString(family)});
    style.setFontSize(20);
    ParagraphBuilderImpl builder(
            paragraphStyle, test_font_collection(), get_unicode(paragraphStyle));
    builder.pushStyle(style);
    builder.addText(
            "\u05E9\u05DC\u05D5\u05DD \u05E2\u05D5\u05DC\u05DD "
            "\u05E9\u05DC\u05D5\u05DD \u05E2\u05D5\u05DC\u05DD "
            "\u05E9\u05DC\u05D5\u05DD \u05E2\u05D5\u05DC\u05DD");
    auto paragraph = builder.Build();
    paragraph->layout(120);
    return paragraph;
}

// TextLine::ellipsis() run positions and offsets are not exposed on Paragraph or LineMetrics
const Run* first_line_ellipsis(Paragraph* paragraph) {
    auto* impl = static_cast<ParagraphImpl*>(paragraph);
    return impl->lines().empty() ? nullptr : impl->lines()[0].ellipsis();
}
}  // namespace

// The ellipsis is shaped apart from the text with the same mark rule as the text: a qamats adds
// a glyph but no advance, and its drawn y belongs in the offset, not the position
DEF_TEST(CoreTextPipeline_EllipsisMarksTakeNoAdvance, reporter) {
    if (!has_font_families({"Arial", "Arial Hebrew"})) {
        return;
    }
    struct Case {
        const char* withMark;
        const char* withoutMark;
    };
    constexpr auto kCases = std::to_array<Case>({
            {"\u05D1\u05B8", "\u05D1"},
            {"\u05D1\u05B8\u2026", "\u05D1\u2026"},
    });
    for (const char* family : {"Arial", "Arial Hebrew"}) {
        for (const Case& c : kCases) {
            skiatest::ReporterContext context(reporter,
                                              SkStringPrintf("%s '%s'", family, c.withMark));
            auto marked = layout_hebrew_ellipsis(family, c.withMark);
            auto bare = layout_hebrew_ellipsis(family, c.withoutMark);
            REPORTER_ASSERT(reporter,
                            static_cast<ParagraphImpl*>(marked.get())->isCoreTextShaped());
            const Run* run = first_line_ellipsis(marked.get());
            const Run* bareRun = first_line_ellipsis(bare.get());
            REPORTER_ASSERT(reporter, run && bareRun);
            if (!run || !bareRun) {
                continue;
            }
            REPORTER_ASSERT(reporter,
                            run->size() == bareRun->size() + 1,
                            "%zu ellipsis glyphs, want %zu",
                            run->size(),
                            bareRun->size() + 1);
            REPORTER_ASSERT(reporter,
                            SkScalarNearlyEqual(run->advance().fX, bareRun->advance().fX, 0.01f),
                            "ellipsis advance %g, want %g as without the mark",
                            run->advance().fX,
                            bareRun->advance().fX);
            for (size_t k = 0; k < run->size(); ++k) {
                // Arial draws the qamats off the baseline; its y belongs in the offset
                REPORTER_ASSERT(reporter,
                                run->positions()[k].fY == 0,
                                "ellipsis glyph %zu: position y %g",
                                k,
                                run->positions()[k].fY);
            }
        }
    }
}

// SH4: an ellipsis run shaped via ShapeEllipsis keeps pair kerning, so its glyphs are drawn closer
// than their unkerned advances at the same tracking, and strictly left to right after splitting
// kerning offsets
DEF_TEST(CoreTextPipeline_PairKerning_EllipsisKernedAndOrdered, reporter) {
    auto fontCollection = test_font_collection();
    for (const char* family : {"Helvetica Neue", "Times New Roman"}) {
        if (!has_font_families({family})) {
            continue;
        }
        for (const char* ellipsis : {"AV\xE2\x80\xA6", "To."}) {
            for (SkScalar fontSize : {12.0f, 17.3f, 24.0f}) {
                skiatest::ReporterContext ctx(
                        reporter, SkStringPrintf("%s '%s' size=%g", family, ellipsis, fontSize));
                ParagraphStyle paragraphStyle;
                paragraphStyle.setTextShapingEngine(TextShapingEngine::kCoreText);
                paragraphStyle.setApplyRoundingHack(false);
                paragraphStyle.setEllipsis(SkString(ellipsis));
                paragraphStyle.setMaxLines(1);
                TextStyle style;
                style.setFontFamilies({SkString(family)});
                style.setFontSize(fontSize);
                ParagraphBuilderImpl builder(
                        paragraphStyle, fontCollection, get_unicode(paragraphStyle));
                builder.pushStyle(style);
                builder.addText("AVATAR Wave To Yo, We'll Try P.J. Fay");
                auto paragraph = builder.Build();
                paragraph->layout(fontSize * 6);
                SkFont font;
                const std::vector<PaintedGlyph> glyphs = painted_glyphs(paragraph.get(), &font);
                // With maxLines 1, visit ends with the ellipsis run
                std::array<SkGlyphID, 3> ellipsisIds = {};
                const size_t ellipsisCount = font.textToGlyphs(
                        ellipsis, strlen(ellipsis), SkTextEncoding::kUTF8, ellipsisIds);
                REPORTER_ASSERT(reporter, ellipsisCount == 3, "got %zu glyphs", ellipsisCount);
                REPORTER_ASSERT(
                        reporter, glyphs.size() >= 3, "got %zu painted glyphs", glyphs.size());
                if (ellipsisCount != 3 || glyphs.size() < 3) {
                    continue;
                }
                const PaintedGlyph* run = glyphs.data() + glyphs.size() - 3;
                REPORTER_ASSERT(reporter,
                                run[0].id == ellipsisIds[0] && run[1].id == ellipsisIds[1] &&
                                        run[2].id == ellipsisIds[2],
                                "got [%u, %u, %u], want [%u, %u, %u]",
                                run[0].id,
                                run[1].id,
                                run[2].id,
                                ellipsisIds[0],
                                ellipsisIds[1],
                                ellipsisIds[2]);
                // The span to the last glyph holds both kerned pairs; its own advance has no pair
                const SkScalar drawnSpan = run[2].drawnX - run[0].drawnX;
                REPORTER_ASSERT(reporter,
                                drawnSpan < run[0].nominal + run[1].nominal,
                                "got span %g, want < nominal %g",
                                drawnSpan,
                                run[0].nominal + run[1].nominal);
                for (size_t i = 1; i < 3; ++i) {
                    REPORTER_ASSERT(reporter,
                                    run[i - 1].drawnX < run[i].drawnX,
                                    "glyph %zu: drawn at %g after %g",
                                    i,
                                    run[i].drawnX,
                                    run[i - 1].drawnX);
                }
            }
        }
    }
}
