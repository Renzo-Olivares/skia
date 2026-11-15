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
#include "include/core/SkFontStyle.h"
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

#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace skia::textlayout;
using namespace skia::textlayout::coretext_test;

namespace {
// Ink box of a drawn glyph in paragraph coordinates: the bounds extendedVisit reports
// (SkFont::getBounds) placed where visit draws the glyph, offsets included
SkRect drawn_ink(const DrawnGlyph& glyph) {
    SkRect bounds;
    glyph.font.getBounds({&glyph.glyph, 1}, {&bounds, 1}, nullptr);
    return bounds.makeOffset(glyph.position);
}
}  // namespace

// SH13: above 10,240 UTF-16 units CTTypesetter drops GSUB/GPOS/morx unless unbounded layout is
// on; a long paragraph must keep ligatures, so each "fi" past the limit still draws one glyph
DEF_TEST(CoreTextContract_SH13_LongTextKeepsLigatures, reporter) {
    if (!has_font_families({"Helvetica"})) {
        return;
    }
    const TextStyle style = contract_text_style("Helvetica", 20.0f);
    auto shortParagraph = layout_text(reporter,
                                      system_collection(),
                                      contract_paragraph_style(),
                                      style,
                                      "fi",
                                      SK_ScalarInfinity);
    const int ligatureGlyphs = static_cast<int>(drawn_glyphs(shortParagraph.get()).size());
    // The property needs a font that ligates "fi"; without the ligature it checks nothing
    REPORTER_ASSERT(reporter, ligatureGlyphs == 1, "got %d", ligatureGlyphs);

    constexpr int kWords = 5200;  // 15,599 UTF-16 units
    std::string text = "fi";
    text.reserve(kWords * 3);
    for (int i = 1; i < kWords; ++i) {
        text += " fi";
    }
    auto longParagraph = layout_text(
            reporter, system_collection(), contract_paragraph_style(), style, text, 1e7f);
    // One ligature per word plus the spaces between words
    const int glyphs = static_cast<int>(drawn_glyphs(longParagraph.get()).size());
    REPORTER_ASSERT(reporter, glyphs == 2 * kWords - 1, "got %d, want %d", glyphs, 2 * kWords - 1);
}

// SH1: glyphs come from the chosen font through CoreText, in text order: an unligated word draws
// the font's cmap glyphs left to right, and the GSUB "fi" ligature draws one glyph for two letters
DEF_TEST(CoreTextContract_SH1_GlyphsFromChosenFont, reporter) {
    if (!has_font_families({"Helvetica"})) {
        return;
    }
    const TextStyle style = contract_text_style("Helvetica", 20);
    constexpr std::string_view kText = "Hamburgefonstiv";
    auto paragraph = layout_text(
            reporter, system_collection(), contract_paragraph_style(), style, kText, 1000);
    const std::vector<DrawnGlyph> glyphs = drawn_glyphs(paragraph.get());
    REPORTER_ASSERT(reporter,
                    glyphs.size() == kText.size(),
                    "got %zu, want %zu",
                    glyphs.size(),
                    kText.size());
    if (glyphs.size() != kText.size()) {
        return;
    }
    const SkString family = family_name(glyphs[0].font);
    REPORTER_ASSERT(reporter, family.equals("Helvetica"), "got %s", family.c_str());
    std::array<SkGlyphID, kText.size()> cmap;
    glyphs[0].font.textToGlyphs(kText.data(), kText.size(), SkTextEncoding::kUTF8, cmap);
    for (size_t k = 0; k < glyphs.size(); ++k) {
        skiatest::ReporterContext ctx(reporter, SkStringPrintf("glyph %zu", k));
        REPORTER_ASSERT(
                reporter, glyphs[k].glyph == cmap[k], "got %u, want %u", glyphs[k].glyph, cmap[k]);
        REPORTER_ASSERT(reporter,
                        k == 0 || glyphs[k].position.fX > glyphs[k - 1].position.fX,
                        "got x %g after %g",
                        glyphs[k].position.fX,
                        glyphs[k - (k > 0)].position.fX);
    }
    auto ligature = layout_text(
            reporter, system_collection(), contract_paragraph_style(), style, "fi", 1000);
    const size_t ligatureGlyphs = drawn_glyphs(ligature.get()).size();
    REPORTER_ASSERT(reporter, ligatureGlyphs == 1, "got %zu", ligatureGlyphs);
}

// SH2: the system font's optical size follows the point size, and an explicit opsz replaces it
// within the axis range (17-96), as dart:ui FontVariation.opticalSize documents
DEF_TEST(CoreTextContract_SH2_OpticalSizeFollowsSize, reporter) {
    if (!has_font_families({".AppleSystemUIFont"})) {
        return;
    }
    struct Case {
        SkScalar size;
        std::optional<SkScalar> explicitOpsz;
        SkScalar expected;
    };
    constexpr auto kCases = std::to_array<Case>(
            {{24, std::nullopt, 24}, {40, std::nullopt, 40}, {40, 20, 20}, {40, 200, 96}});
    for (const Case& c : kCases) {
        skiatest::ReporterContext ctx(
                reporter, SkStringPrintf("%gpt opsz %g", c.size, c.explicitOpsz.value_or(-1)));
        TextStyle style = contract_text_style(".AppleSystemUIFont", c.size);
        if (c.explicitOpsz) {
            const SkFontArguments::VariationPosition::Coordinate coordinate = {
                    SkSetFourByteTag('o', 'p', 's', 'z'), *c.explicitOpsz};
            SkFontArguments arguments;
            arguments.setVariationDesignPosition({&coordinate, 1});
            style.setFontArguments(arguments);
        }
        auto paragraph = layout_text(
                reporter, system_collection(), contract_paragraph_style(), style, "Optical", 1000);
        const SkScalar opsz = axis_coordinate(paragraph->getFontAtUTF16Offset(0).getTypeface(),
                                              SkSetFourByteTag('o', 'p', 's', 'z'))
                                      .value_or(-1);
        REPORTER_ASSERT(reporter,
                        SkScalarNearlyEqual(opsz, c.expected, 0.01f),
                        "got %g, want %g",
                        opsz,
                        c.expected);
    }
}

// SH3: CoreText applies the size-dependent trak of the system font and its fallbacks: repeated
// unkerned glyphs draw at a pitch off the font's own advance, by a different amount at 17, 40pt
DEF_TEST(CoreTextContract_SH3_TrackingFollowsSize, reporter) {
    if (!has_font_families({".AppleSystemUIFont"})) {
        return;
    }
    // U+6C34 is not in the system font and falls back to PingFang
    for (std::string_view text : {"llll", "\u6C34\u6C34\u6C34\u6C34"}) {
        constexpr SkScalar kSizes[] = {17, 40};
        SkScalar trakPerEm[2] = {};
        for (int i = 0; i < 2; ++i) {
            skiatest::ReporterContext ctx(reporter,
                                          SkStringPrintf("'%s' %gpt", text.data(), kSizes[i]));
            auto paragraph = layout_text(reporter,
                                         system_collection(),
                                         contract_paragraph_style(),
                                         contract_text_style(".AppleSystemUIFont", kSizes[i]),
                                         text,
                                         1000);
            const std::vector<DrawnGlyph> glyphs = drawn_glyphs(paragraph.get());
            REPORTER_ASSERT(reporter, glyphs.size() == 4, "got %zu", glyphs.size());
            if (glyphs.size() != 4) {
                return;
            }
            const SkScalar pitch = (glyphs[3].position.fX - glyphs[0].position.fX) / 3;
            trakPerEm[i] = (pitch - font_advance(glyphs[0])) / kSizes[i];
            REPORTER_ASSERT(reporter,
                            std::abs(trakPerEm[i]) > 1e-4f,
                            "got %g per em in %s",
                            trakPerEm[i],
                            family_name(glyphs[0].font).c_str());
        }
        REPORTER_ASSERT(reporter,
                        !SkScalarNearlyEqual(trakPerEm[0], trakPerEm[1], 1e-4f),
                        "'%s': got %g per em at both sizes",
                        text.data(),
                        trakPerEm[0]);
    }
}

// SH4: kerning comes from CoreText: a kerned pair is narrower than its advances, and the line
// and pair caret match the native CTLine, also at Optima 21.26pt where 16.16 scaling lands off
DEF_TEST(CoreTextContract_SH4_KerningFromCoreText, reporter) {
    struct Case {
        const char* family;
        SkScalar size;
    };
    constexpr auto kCases = std::to_array<Case>({{"Helvetica", 12},
                                                 {"Helvetica", 17.3f},
                                                 {"Avenir", 24},
                                                 {"Arial", 17.3f},
                                                 {"Futura", 24},
                                                 {"Optima", 21.26f}});
    for (const Case& c : kCases) {
        if (!has_font_families({c.family})) {
            continue;
        }
        SkUniqueCFRef<CTFontRef> font = native_font(c.family, c.size);
        for (std::string_view text : {"AV", "To", "Wa"}) {
            skiatest::ReporterContext ctx(
                    reporter, SkStringPrintf("%s %gpt '%s'", c.family, c.size, text.data()));
            auto paragraph = layout_text(reporter,
                                         system_collection(),
                                         contract_paragraph_style(),
                                         contract_text_style(c.family, c.size),
                                         text,
                                         1000);
            const std::vector<DrawnGlyph> glyphs = drawn_glyphs(paragraph.get());
            REPORTER_ASSERT(reporter, glyphs.size() == 2, "got %zu", glyphs.size());
            if (glyphs.size() != 2) {
                continue;
            }
            const SkScalar width = first_line_width(paragraph.get());
            const SkScalar kern = width - font_advance(glyphs[0]) - font_advance(glyphs[1]);
            REPORTER_ASSERT(reporter, kern < 0, "got %g", kern);
            const SkScalar pen = glyphs[0].position.fX + font_advance(glyphs[0]);
            REPORTER_ASSERT(reporter,
                            SkScalarNearlyEqual(glyphs[1].position.fX, pen + kern, 1e-3f),
                            "got %g, want %g",
                            glyphs[1].position.fX,
                            pen + kern);

            SkUniqueCFRef<CTLineRef> line = native_line(font.get(), text, 0);
            const SkScalar nativeWidth = native_line_width(line.get());
            REPORTER_ASSERT(reporter,
                            SkScalarNearlyEqual(width, nativeWidth, kNativeWidthTolerance),
                            "got %.6f, want %.6f",
                            width,
                            nativeWidth);
            const std::vector<TextBox> boxes = paragraph->getRectsForRange(
                    0, 1, RectHeightStyle::kTight, RectWidthStyle::kTight);
            REPORTER_ASSERT(reporter, boxes.size() == 1, "got %zu", boxes.size());
            if (boxes.size() != 1) {
                continue;
            }
            const SkScalar nativeCaret =
                    static_cast<SkScalar>(CTLineGetOffsetForStringIndex(line.get(), 1, nullptr));
            REPORTER_ASSERT(reporter,
                            SkScalarNearlyEqual(boxes[0].rect.fRight, nativeCaret, 1e-3f) &&
                                    SkScalarNearlyEqual(nativeCaret, pen + kern / 2, 1e-3f),
                            "got %.6f, native %.6f, kern / 2 at %.6f",
                            boxes[0].rect.fRight,
                            nativeCaret,
                            pen + kern / 2);
        }
    }
}

// SH5: with subpixel on (Flutter's only mode) and the rounding hack off, positions are not
// rounded: 13.3pt Ahem draws at exact multiples of 13.3 and Helvetica keeps fractional advances
DEF_TEST(CoreTextContract_SH5_FractionalPositionsKept, reporter) {
    sk_sp<FontCollection> ahem = asset_collection({{"fonts/ahem.ttf", "Ahem"}});
    if (!ahem) {
        return;
    }
    constexpr SkScalar kSize = 13.3f;
    auto paragraph = layout_text(reporter,
                                 ahem,
                                 contract_paragraph_style(),
                                 contract_text_style("Ahem", kSize),
                                 "iiii",
                                 1000);
    const std::vector<DrawnGlyph> glyphs = drawn_glyphs(paragraph.get());
    REPORTER_ASSERT(reporter, glyphs.size() == 4, "got %zu", glyphs.size());
    for (size_t k = 0; k < glyphs.size(); ++k) {
        REPORTER_ASSERT(reporter,
                        SkScalarNearlyEqual(glyphs[k].position.fX, k * kSize, 1e-4f),
                        "glyph %zu: got %g, want %g",
                        k,
                        glyphs[k].position.fX,
                        k * kSize);
    }
    if (!has_font_families({"Helvetica"})) {
        return;
    }
    auto helvetica = layout_text(reporter,
                                 system_collection(),
                                 contract_paragraph_style(),
                                 contract_text_style("Helvetica", 13.7f),
                                 "iiii",
                                 1000);
    int fractional = 0;
    for (const DrawnGlyph& glyph : drawn_glyphs(helvetica.get())) {
        fractional += glyph.position.fX != SkScalarRoundToScalar(glyph.position.fX);
    }
    REPORTER_ASSERT(reporter, fractional == 3, "got %d", fractional);
    const SkScalar width = helvetica->getLongestLine();
    REPORTER_ASSERT(reporter, width != SkScalarRoundToScalar(width), "got %g", width);
}

// SH6: with subpixel off (Skia-only setSubpixel) every glyph and the line width land on whole
// pixels, also after ZWJ and a soft hyphen, which CoreText gives no advance
DEF_TEST(CoreTextContract_SH6_NonSubpixelWholePixels, reporter) {
    struct Case {
        sk_sp<FontCollection> fontCollection;
        const char* family;
        std::string_view text;
    };
    if (!has_font_families({"Helvetica"})) {
        return;
    }
    sk_sp<FontCollection> ahem = asset_collection({{"fonts/ahem.ttf", "Ahem"}});
    if (!ahem) {
        return;
    }
    const std::array<Case, 2> cases = {{
            {ahem, "Ahem", "Actions section"},
            {system_collection(), "Helvetica", "a\u200Db\u00ADc Actions"},
    }};
    for (const Case& c : cases) {
        for (SkScalar size : {13.3f, 16.8f, 23.7f}) {
            skiatest::ReporterContext ctx(reporter, SkStringPrintf("%s %gpt", c.family, size));
            TextStyle style = contract_text_style(c.family, size);
            style.setSubpixel(false);
            auto paragraph = layout_text(
                    reporter, c.fontCollection, contract_paragraph_style(), style, c.text, 1000);
            const std::vector<DrawnGlyph> glyphs = drawn_glyphs(paragraph.get());
            REPORTER_ASSERT(reporter, !glyphs.empty());
            for (const DrawnGlyph& glyph : glyphs) {
                const SkScalar x = glyph.position.fX;
                REPORTER_ASSERT(reporter, x == SkScalarRoundToScalar(x), "got x %g", x);
            }
            const SkScalar width = paragraph->getLongestLine();
            REPORTER_ASSERT(reporter, width == SkScalarRoundToScalar(width), "got width %g", width);
        }
    }
}

// SH7: a combining mark takes no width and CoreText draws it over its base: the line is as wide
// as the same text without the mark, and the mark's ink lies within the box of its grapheme
DEF_TEST(CoreTextContract_SH7_MarkAddsNoWidth, reporter) {
    struct Case {
        const char* family;
        std::string_view text;
        std::string_view withoutMark;
        uint32_t markUtf8;
        size_t graphemeStart16;
        size_t graphemeEnd16;
        TextDirection direction;
    };
    // SF draws U+0301 left of the pen after its base; Arial Hebrew draws qamats right of it
    constexpr auto kCases = std::to_array<Case>({
            {"Helvetica", "ax\u0301b", "axb", 2, 1, 3, TextDirection::kLtr},
            {".AppleSystemUIFont", "ab\u0301c", "abc", 2, 1, 3, TextDirection::kLtr},
            {"Arial Hebrew",
             "\u05D0\u05D1\u05B8\u05D2",
             "\u05D0\u05D1\u05D2",
             4,
             1,
             3,
             TextDirection::kRtl},
    });
    for (const Case& c : kCases) {
        if (!has_font_families({c.family})) {
            continue;
        }
        skiatest::ReporterContext ctx(reporter, SkStringPrintf("%s", c.family));
        ParagraphStyle paragraphStyle = contract_paragraph_style();
        paragraphStyle.setTextDirection(c.direction);
        const TextStyle style = contract_text_style(c.family, 20);
        auto marked =
                layout_text(reporter, system_collection(), paragraphStyle, style, c.text, 1000);
        auto unmarked = layout_text(
                reporter, system_collection(), paragraphStyle, style, c.withoutMark, 1000);
        const SkScalar width = first_line_width(marked.get());
        const SkScalar expected = first_line_width(unmarked.get());
        REPORTER_ASSERT(reporter,
                        SkScalarNearlyEqual(width, expected, 1e-3f),
                        "got %g, want %g",
                        width,
                        expected);

        const std::vector<TextBox> boxes = marked->getRectsForRange(c.graphemeStart16,
                                                                    c.graphemeEnd16,
                                                                    RectHeightStyle::kTight,
                                                                    RectWidthStyle::kTight);
        REPORTER_ASSERT(reporter, boxes.size() == 1, "got %zu boxes", boxes.size());
        int marks = 0;
        for (const DrawnGlyph& glyph : drawn_glyphs(marked.get())) {
            if (glyph.utf8Start != c.markUtf8 || boxes.size() != 1) {
                continue;
            }
            ++marks;
            const SkRect ink = drawn_ink(glyph);
            const SkRect& box = boxes[0].rect;
            REPORTER_ASSERT(reporter,
                            box.fLeft <= ink.fLeft && ink.fRight <= box.fRight,
                            "mark ink %g..%g, grapheme box %g..%g",
                            ink.fLeft,
                            ink.fRight,
                            box.fLeft,
                            box.fRight);
        }
        REPORTER_ASSERT(reporter, marks == 1, "got %d mark glyphs", marks);
    }
}

// SH8: Noto Nastaliq Urdu joins letters with cursive attachment, which lifts glyphs off the
// baseline as native CoreText draws them, and draws each fatha above the baseline over its base
DEF_TEST(CoreTextContract_SH8_CursiveAttachment, reporter) {
    constexpr const char* kFamily = "Noto Nastaliq Urdu";
    if (!has_font_families({kFamily})) {
        return;
    }
    // Fatha (UTF-8 at 2 and 8) over noon (0) and teh (6)
    constexpr std::string_view kText = "\u0646\u064E\u0633\u062A\u064E\u0639\u0644\u06CC\u0642";
    struct MarkOnBase {
        uint32_t mark;
        uint32_t base;
    };
    constexpr MarkOnBase kMarks[] = {{2, 0}, {8, 6}};
    ParagraphStyle paragraphStyle = contract_paragraph_style();
    paragraphStyle.setTextDirection(TextDirection::kRtl);
    auto paragraph = layout_text(reporter,
                                 system_collection(),
                                 paragraphStyle,
                                 contract_text_style(kFamily, 40),
                                 kText,
                                 1000);
    REPORTER_ASSERT(
            reporter, paragraph->unresolvedGlyphs() == 0, "got %d", paragraph->unresolvedGlyphs());
    std::vector<LineMetrics> lines;
    paragraph->getLineMetrics(lines);
    REPORTER_ASSERT(reporter, lines.size() == 1, "got %zu", lines.size());
    if (lines.size() != 1) {
        return;
    }
    const SkScalar baseline = static_cast<SkScalar>(lines[0].fBaseline);
    const std::vector<DrawnGlyph> glyphs = drawn_glyphs(paragraph.get());
    int offBaseline = 0;
    for (const DrawnGlyph& glyph : glyphs) {
        offBaseline += glyph.position.fY != baseline;
    }
    REPORTER_ASSERT(reporter, offBaseline > 0, "got %d of %zu", offBaseline, glyphs.size());
    for (const MarkOnBase& expected : kMarks) {
        skiatest::ReporterContext ctx(reporter, SkStringPrintf("fatha at %u", expected.mark));
        const DrawnGlyph* mark = nullptr;
        const DrawnGlyph* base = nullptr;
        for (const DrawnGlyph& glyph : glyphs) {
            mark = glyph.utf8Start == expected.mark ? &glyph : mark;
            base = glyph.utf8Start == expected.base ? &glyph : base;
        }
        REPORTER_ASSERT(reporter, mark && base);
        if (!mark || !base) {
            continue;
        }
        const SkScalar baseX = base->position.fX;
        const SkScalar markX = mark->position.fX;
        const SkScalar baseAdvance = font_advance(*base);
        const SkRect ink = drawn_ink(*mark);
        REPORTER_ASSERT(reporter,
                        baseX <= markX && markX <= baseX + baseAdvance && ink.fBottom < baseline,
                        "mark at %g, ink bottom %g; base %g..%g, baseline %g",
                        markX,
                        ink.fBottom,
                        baseX,
                        baseX + baseAdvance,
                        baseline);
    }
}

// SH9: invisible controls draw nothing and take no width, as in native CoreText (HarfBuzz would
// draw .notdef for U+0000/U+0001): "a<c>b" is as wide as "ab" (both in a resolved font and in a
// disallowed fallback run) and <c> alone paints no pixel
DEF_TEST(CoreTextContract_SH9_InvisibleControlsZeroWidth, reporter) {
    sk_sp<FontCollection> ahem = asset_collection({{"fonts/ahem.ttf", "Ahem"}});
    if (!ahem) {
        return;
    }
    const TextStyle style = contract_text_style("Ahem", kAhemSize);
    sk_sp<FontCollection> noFonts = test_font_collection();
    noFonts->disableFontFallback();
    const TextStyle noFontStyle = contract_text_style("NoSuchFont", kAhemSize);
    const SkScalar noFontAbWidth = first_line_width(
            layout_text(reporter, noFonts, contract_paragraph_style(), noFontStyle, "ab", 1000)
                    .get());
    REPORTER_ASSERT(reporter, noFontAbWidth > 0, "got %g", noFontAbWidth);
    struct Case {
        const char* name;
        std::string_view control;
    };
    constexpr auto kCases = std::to_array<Case>({{"ZWSP", "\u200B"},
                                                 {"ZWJ", "\u200D"},
                                                 {"WJ", "\u2060"},
                                                 {"ZWNBSP", "\uFEFF"},
                                                 {"U+0000", std::string_view("\0", 1)},
                                                 {"U+0001", "\x01"}});
    for (const Case& c : kCases) {
        skiatest::ReporterContext ctx(reporter, SkStringPrintf("%s", c.name));
        const std::string text = "a" + std::string(c.control) + "b";
        auto paragraph = layout_text(reporter, ahem, contract_paragraph_style(), style, text, 1000);
        const SkScalar width = first_line_width(paragraph.get());
        REPORTER_ASSERT(reporter, width == 2 * kAhemSize, "got %g", width);
        REPORTER_ASSERT(reporter,
                        paragraph->unresolvedGlyphs() == 0,
                        "got %d unresolved",
                        paragraph->unresolvedGlyphs());

        auto fallback =
                layout_text(reporter, noFonts, contract_paragraph_style(), noFontStyle, text, 1000);
        const SkScalar fallbackWidth = first_line_width(fallback.get());
        REPORTER_ASSERT(reporter,
                        fallbackWidth == noFontAbWidth,
                        "got %g, want %g",
                        fallbackWidth,
                        noFontAbWidth);
        REPORTER_ASSERT(reporter,
                        fallback->unresolvedGlyphs() == 2,
                        "got %d unresolved",
                        fallback->unresolvedGlyphs());

        auto alone =
                layout_text(reporter, ahem, contract_paragraph_style(), style, c.control, 1000);
        const SkScalar aloneWidth = first_line_width(alone.get());
        REPORTER_ASSERT(reporter, aloneWidth == 0, "got %g", aloneWidth);
        REPORTER_ASSERT(reporter,
                        alone->unresolvedGlyphs() == 0,
                        "got %d unresolved",
                        alone->unresolvedGlyphs());
        SkBitmap bitmap;
        bitmap.allocN32Pixels(4 * kAhemSize, 2 * kAhemSize);
        bitmap.eraseColor(SK_ColorTRANSPARENT);
        SkCanvas canvas(bitmap);
        alone->paint(&canvas, kAhemSize, 0);
        int painted = 0;
        for (int y = 0; y < bitmap.height(); ++y) {
            for (int x = 0; x < bitmap.width(); ++x) {
                painted += bitmap.getColor(x, y) != SK_ColorTRANSPARENT;
            }
        }
        REPORTER_ASSERT(reporter, painted == 0, "got %d pixels", painted);
    }
}

// SH10: CR in CRLF, U+2028, U+2029, VT, FF, lone CR and U+0085 take no width, as in native
// CoreText; lone CR and U+0085 do not break yet (open bug B5), so only the others check breaks
DEF_TEST(CoreTextContract_SH10_BreakCharactersZeroWidth, reporter) {
    sk_sp<FontCollection> ahem = asset_collection({{"fonts/ahem.ttf", "Ahem"}});
    if (!ahem) {
        return;
    }
    struct Case {
        const char* name;
        std::string_view text;
        bool checkBreak;
    };
    constexpr auto kCases = std::to_array<Case>({
            {"CRLF", "A\r\nB", true},
            {"U+2028", "A\u2028B", true},
            {"U+2029", "A\u2029B", true},
            {"VT", "A\vB", true},
            {"FF", "A\fB", true},
            {"CR", "A\rB", false},
            {"U+0085", "A\u0085B", false},
    });
    for (const Case& c : kCases) {
        skiatest::ReporterContext ctx(reporter, SkStringPrintf("%s", c.name));
        auto paragraph = layout_text(reporter,
                                     ahem,
                                     contract_paragraph_style(),
                                     contract_text_style("Ahem", kAhemSize),
                                     c.text,
                                     1000);
        std::vector<LineMetrics> lines;
        paragraph->getLineMetrics(lines);
        REPORTER_ASSERT(
                reporter, !c.checkBreak || lines.size() == 2, "got %zu lines", lines.size());
        double width = 0;
        for (const LineMetrics& line : lines) {
            width += line.fWidth;
        }
        REPORTER_ASSERT(reporter, width == 2 * kAhemSize, "got %g", width);
    }
}

// SH11: Flutter always replaces TAB with a space before shaping (setReplaceTabCharacters), so a
// tab is exactly as wide as the font's space, also on a line after a wrap
DEF_TEST(CoreTextContract_SH11_ReplacedTabIsSpace, reporter) {
    sk_sp<FontCollection> ahem = asset_collection({{"fonts/ahem.ttf", "Ahem"}});
    if (!ahem) {
        return;
    }
    ParagraphStyle paragraphStyle = contract_paragraph_style();
    paragraphStyle.setReplaceTabCharacters(true);
    const TextStyle style = contract_text_style("Ahem", kAhemSize);
    // At width 100 both wrap after "aaaaaa ", leaving "a\tb" or "a b" on the second line
    auto tabbed = layout_text(reporter, ahem, paragraphStyle, style, "aaaaaa a\tb", 100);
    auto spaced = layout_text(reporter, ahem, paragraphStyle, style, "aaaaaa a b", 100);
    std::vector<LineMetrics> tabbedLines;
    std::vector<LineMetrics> spacedLines;
    tabbed->getLineMetrics(tabbedLines);
    spaced->getLineMetrics(spacedLines);
    REPORTER_ASSERT(reporter, tabbedLines.size() == 2, "got %zu lines", tabbedLines.size());
    if (tabbedLines.size() != 2 || spacedLines.size() != 2) {
        return;
    }
    for (size_t i = 0; i < 2; ++i) {
        REPORTER_ASSERT(reporter,
                        tabbedLines[i].fWidth == spacedLines[i].fWidth,
                        "line %zu: got %g, want %g",
                        i,
                        tabbedLines[i].fWidth,
                        spacedLines[i].fWidth);
    }
    REPORTER_ASSERT(
            reporter, tabbedLines[1].fWidth == 3 * kAhemSize, "got %g", tabbedLines[1].fWidth);
    auto unwrapped = layout_text(reporter, ahem, paragraphStyle, style, "a\tb", 1000);
    const SkScalar width = first_line_width(unwrapped.get());
    REPORTER_ASSERT(reporter, width == 3 * kAhemSize, "got %g", width);
}

// SH12: synthetic bold and italic go only on a requested family that lacks the face (Roboto
// gets them, Helvetica Bold and the system italic do not); fallback faces (B7) are not covered
DEF_TEST(CoreTextContract_SH12_SyntheticOnlyOnRequestedFace, reporter) {
    sk_sp<FontCollection> roboto = asset_collection({{"fonts/Roboto-Regular.ttf", "Roboto"}});
    if (!roboto) {
        return;
    }
    TextStyle style = contract_text_style("Roboto", 20);
    style.setFontStyle(SkFontStyle::BoldItalic());
    auto synthetic =
            layout_text(reporter, roboto, contract_paragraph_style(), style, "Roboto", 1000);
    const SkFont font = synthetic->getFontAtUTF16Offset(0);
    REPORTER_ASSERT(reporter, font.isEmbolden());
    REPORTER_ASSERT(reporter, font.getSkewX() < 0, "got skew %g", font.getSkewX());

    if (!has_font_families({"Helvetica", ".AppleSystemUIFont"})) {
        return;
    }
    TextStyle bold = contract_text_style("Helvetica", 20);
    bold.setFontStyle(SkFontStyle::Bold());
    auto realBold = layout_text(
            reporter, system_collection(), contract_paragraph_style(), bold, "Bold", 1000);
    const SkFont boldFont = realBold->getFontAtUTF16Offset(0);
    REPORTER_ASSERT(reporter, !boldFont.isEmbolden());
    REPORTER_ASSERT(reporter,
                    boldFont.getTypeface()->fontStyle().weight() >= SkFontStyle::kBold_Weight,
                    "got weight %d",
                    boldFont.getTypeface()->fontStyle().weight());

    TextStyle italic = contract_text_style(".AppleSystemUIFont", 20);
    italic.setFontStyle(SkFontStyle::Italic());
    auto realItalic = layout_text(
            reporter, system_collection(), contract_paragraph_style(), italic, "Italic", 1000);
    const SkFont italicFont = realItalic->getFontAtUTF16Offset(0);
    REPORTER_ASSERT(reporter, italicFont.getSkewX() == 0, "got skew %g", italicFont.getSkewX());
    // System italic faces report an upright SkFontStyle (Flutter #130492), so the name tells
    // a real italic face from a synthetic slant
    SkString italicName;
    italicFont.getTypeface()->getPostScriptName(&italicName);
    REPORTER_ASSERT(reporter, italicName.contains("Italic"), "got %s", italicName.c_str());

    // An unassigned code point (U+0378) in .AppleSystemUIFont Italic falls back to primary
    // .SFNS-RegularItalic as tofu and must not receive a synthetic -0.25 double slant
    auto unmapped = layout_text(
            reporter, system_collection(), contract_paragraph_style(), italic, "a\xCD\xB8", 1000);
    const SkFont tofuFont = unmapped->getFontAtUTF16Offset(1);
    REPORTER_ASSERT(reporter, tofuFont.getSkewX() == 0, "got skew %g", tofuFont.getSkewX());

    ParagraphStyle ellipStyle = contract_paragraph_style();
    ellipStyle.setMaxLines(1);
    ellipStyle.setEllipsis(SkString("\xCD\xB8"));
    auto unmappedEllip = layout_text(
            reporter, system_collection(), ellipStyle, italic, "Italic text truncated here", 60);
    const std::vector<DrawnGlyph> ellipGlyphs = drawn_glyphs(unmappedEllip.get());
    REPORTER_ASSERT(reporter, !ellipGlyphs.empty(), "got %zu glyphs", ellipGlyphs.size());
    if (!ellipGlyphs.empty()) {
        const SkScalar ellipSkew = ellipGlyphs.back().font.getSkewX();
        REPORTER_ASSERT(reporter, ellipSkew == 0, "got ellipsis skew %g", ellipSkew);
    }
}

#if defined(SK_UNICODE_APPLE_IMPLEMENTATION)
// SH14: a leading U+FEFF is ZWNBSP, not a BOM: it stays with no width and shifts every UTF-16
// index by one; SkUnicode_apple keeps it, ICU builds are not covered
DEF_TEST(CoreTextContract_SH14_LeadingZwnbspStays, reporter) {
    sk_sp<FontCollection> ahem = asset_collection({{"fonts/ahem.ttf", "Ahem"}});
    if (!ahem) {
        return;
    }
    const TextStyle style = contract_text_style("Ahem", kAhemSize);
    auto withMark =
            layout_text(reporter, ahem, contract_paragraph_style(), style, "\uFEFFabc", 1000);
    auto plain = layout_text(reporter, ahem, contract_paragraph_style(), style, "abc", 1000);
    const SkScalar width = first_line_width(withMark.get());
    REPORTER_ASSERT(reporter, width == 3 * kAhemSize, "got %g", width);
    for (size_t i = 0; i < 3; ++i) {
        skiatest::ReporterContext ctx(reporter, SkStringPrintf("letter %zu", i));
        const std::vector<TextBox> shifted = withMark->getRectsForRange(
                i + 1, i + 2, RectHeightStyle::kTight, RectWidthStyle::kTight);
        const std::vector<TextBox> expected =
                plain->getRectsForRange(i, i + 1, RectHeightStyle::kTight, RectWidthStyle::kTight);
        REPORTER_ASSERT(reporter,
                        shifted.size() == 1 && expected.size() == 1,
                        "got %zu and %zu boxes",
                        shifted.size(),
                        expected.size());
        if (shifted.size() != 1 || expected.size() != 1) {
            continue;
        }
        REPORTER_ASSERT(reporter,
                        shifted[0].rect == expected[0].rect,
                        "got %g..%g, want %g..%g",
                        shifted[0].rect.fLeft,
                        shifted[0].rect.fRight,
                        expected[0].rect.fLeft,
                        expected[0].rect.fRight);
    }
    Paragraph::GlyphInfo glyphInfo;
    REPORTER_ASSERT(reporter, withMark->getGlyphInfoAtUTF16Offset(0, &glyphInfo));
    REPORTER_ASSERT(reporter,
                    glyphInfo.fGraphemeClusterTextRange.start == 0,
                    "got %zu",
                    glyphInfo.fGraphemeClusterTextRange.start);
}
#endif  // defined(SK_UNICODE_APPLE_IMPLEMENTATION)

// SH15: emoji, explicit or fallen back from Helvetica, take the native CTLine width with no
// gap: wider than the point size only up to 25pt, and three emoji are exactly three times one
DEF_TEST(CoreTextContract_SH15_EmojiNativeWidthNoGap, reporter) {
    if (!has_font_families({"Apple Color Emoji", "Helvetica"})) {
        return;
    }
    constexpr std::string_view kOne = "\U0001F600";
    constexpr std::string_view kThree = "\U0001F600\U0001F600\U0001F600";
    for (const char* family : {"Apple Color Emoji", "Helvetica"}) {
        for (SkScalar size : {17.0f, 25.0f, 40.0f}) {
            skiatest::ReporterContext ctx(reporter, SkStringPrintf("%s %gpt", family, size));
            const TextStyle style = contract_text_style(family, size);
            auto one = layout_text(
                    reporter, system_collection(), contract_paragraph_style(), style, kOne, 1000);
            auto three = layout_text(
                    reporter, system_collection(), contract_paragraph_style(), style, kThree, 1000);
            const SkScalar oneWidth = first_line_width(one.get());
            const SkScalar threeWidth = first_line_width(three.get());
            REPORTER_ASSERT(reporter,
                            SkScalarNearlyEqual(threeWidth, 3 * oneWidth, 1e-3f),
                            "got %g, want %g",
                            threeWidth,
                            3 * oneWidth);
            SkUniqueCFRef<CTFontRef> font = native_font(family, size);
            SkUniqueCFRef<CTLineRef> line = native_line(font.get(), kOne, 0);
            const SkScalar nativeWidth = native_line_width(line.get());
            REPORTER_ASSERT(reporter,
                            SkScalarNearlyEqual(oneWidth, nativeWidth, kNativeWidthTolerance),
                            "got %g, want %g",
                            oneWidth,
                            nativeWidth);
            // Measured on macOS 26.7: 22 at 17 pt, 26 at 25 pt, 40 at 40 pt
            REPORTER_ASSERT(reporter,
                            size <= 25 ? oneWidth > size : oneWidth == size,
                            "got %g at %gpt",
                            oneWidth,
                            size);
        }
    }
}
