/*
 * Copyright 2026 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontStyle.h"
#include "include/core/SkRefCnt.h"
#include "include/core/SkScalar.h"
#include "include/core/SkString.h"
#include "include/core/SkTypeface.h"
#include "include/core/SkTypes.h"
#include "include/ports/SkTypeface_mac.h"
#include "modules/skparagraph/include/FontCollection.h"
#include "modules/skparagraph/include/Paragraph.h"
#include "modules/skparagraph/include/ParagraphPainter.h"
#include "modules/skparagraph/include/ParagraphStyle.h"
#include "modules/skparagraph/include/TextStyle.h"
#include "modules/skparagraph/src/ParagraphBuilderImpl.h"
#include "modules/skparagraph/src/ParagraphImpl.h"
#include "modules/skparagraph/tests/coretext/CoreTextTestUtils.h"
#include "tests/Test.h"
// Native CTLines tell a line that keeps the system font's trak from one that drops it
#include "src/utils/mac/SkUniqueCFRef.h"

#include <CoreText/CoreText.h>

#include <array>
#include <initializer_list>
#include <memory>
#include <vector>

using namespace skia::textlayout;
using namespace skia::textlayout::coretext_test;

namespace {

// A HarfBuzz-free build aborts on kDefault, so only the CoreText half of each contract runs
constexpr TextShapingEngine kEngines[] = {
#if !defined(SK_PARAGRAPH_DISABLE_HARFBUZZ) && defined(SK_UNICODE_ICU_IMPLEMENTATION)
        TextShapingEngine::kDefault,
#endif
        TextShapingEngine::kCoreText};

const char* engine_name(TextShapingEngine engine) {
    return engine == TextShapingEngine::kCoreText ? "kCoreText" : "kDefault";
}

// Lays out text in the single style textStyle on engine at width. Also asserts that engine took
// effect: no public API reports the engine, and a silent HarfBuzz fallback would pass every
// other assert
std::unique_ptr<Paragraph> layout_styled(skiatest::Reporter* reporter,
                                         const sk_sp<FontCollection>& fontCollection,
                                         TextShapingEngine engine,
                                         const TextStyle& textStyle,
                                         const char* text,
                                         SkScalar width,
                                         ParagraphStyle paragraphStyle = ParagraphStyle()) {
    paragraphStyle.setTextShapingEngine(engine);
    ParagraphBuilderImpl builder(paragraphStyle, fontCollection, get_unicode(paragraphStyle));
    builder.pushStyle(textStyle);
    builder.addText(text);
    builder.pop();
    auto paragraph = builder.Build();
    paragraph->layout(width);
    REPORTER_ASSERT(reporter,
                    static_cast<ParagraphImpl*>(paragraph.get())->isCoreTextShaped() ==
                            (engine == TextShapingEngine::kCoreText));
    return paragraph;
}

// layout_styled with one font family, size and letterSpacing; unlike
// coretext_test::layout_text, which runs only kCoreText, it takes either engine
std::unique_ptr<Paragraph> layout_font(skiatest::Reporter* reporter,
                                       const sk_sp<FontCollection>& fontCollection,
                                       TextShapingEngine engine,
                                       const char* text,
                                       const char* family,
                                       SkScalar fontSize,
                                       SkScalar letterSpacing,
                                       SkScalar width) {
    TextStyle ts;
    ts.setFontFamilies({SkString(family)});
    ts.setFontSize(fontSize);
    ts.setLetterSpacing(letterSpacing);
    return layout_styled(reporter, fontCollection, engine, ts, text, width);
}

// Longest line of text in family on one 2000pt line; asserts every glyph resolved, since a
// fallback face would bring another font's trak and opsz
SkScalar longest_line(skiatest::Reporter* reporter,
                      const sk_sp<FontCollection>& fontCollection,
                      TextShapingEngine engine,
                      const char* text,
                      const char* family,
                      SkScalar fontSize,
                      SkScalar letterSpacing) {
    auto paragraph = layout_font(
            reporter, fontCollection, engine, text, family, fontSize, letterSpacing, 2000.0f);
    REPORTER_ASSERT(reporter,
                    paragraph->unresolvedGlyphs() == 0,
                    "%s %gpt: got %d unresolved",
                    family,
                    fontSize,
                    paragraph->unresolvedGlyphs());
    return paragraph->getLongestLine();
}

// family first, then Helvetica as the fallback list Flutter apps pass in fontFamilyFallback
TextStyle style_with_helvetica_after(const char* family,
                                     SkScalar fontSize,
                                     SkFontStyle fontStyle = SkFontStyle()) {
    TextStyle ts;
    ts.setFontFamilies({SkString(family), SkString("Helvetica")});
    ts.setFontSize(fontSize);
    ts.setFontStyle(fontStyle);
    return ts;
}

// Asserts that paragraph shapes at least one run and every run uses an Emoji family
void check_all_runs_in_emoji_font(skiatest::Reporter* reporter, const Paragraph& paragraph) {
    const std::vector<Paragraph::FontInfo> fonts = paragraph.getFonts();
    REPORTER_ASSERT(reporter, !fonts.empty(), "got %zu runs", fonts.size());
    for (size_t i = 0; i < fonts.size(); ++i) {
        const SkString family = family_name(fonts[i].fFont);
        REPORTER_ASSERT(reporter,
                        family.contains("Emoji"),
                        "run %zu/%zu [%zu, %zu): got '%s'",
                        i,
                        fonts.size(),
                        fonts[i].fTextRange.start,
                        fonts[i].fTextRange.end,
                        family.c_str());
    }
}

// Native line widths of text in the system UI font at fontSize, with and without its trak
struct NativeTrakWidths {
    SkScalar tracked;
    SkScalar untracked;
};

NativeTrakWidths native_system_trak_widths(const char* text, SkScalar fontSize) {
    SkUniqueCFRef<CTFontRef> system(
            CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, fontSize, nullptr));
    SkUniqueCFRef<CTFontRef> untracked = untracked_copy(system.get());
    return {native_line_width(native_line(system.get(), text, 0).get()),
            native_line_width(native_line(untracked.get(), text, 0).get())};
}

}  // namespace

// Flutter 150824, 18915, 22572: .AppleSystemUIFont applies size-dependent optical sizing (opsz)
// and tracking (trak) on CoreText, so its width per point drops as the size grows; Helvetica has
// neither table and scales linearly on both engines, which shows the drop comes from the font
DEF_TEST(SkParagraph_Flutter_150824_18915_SystemFontTightensWithSize, reporter) {
    if (!has_font_families({"Helvetica", ".AppleSystemUIFont"})) {
        return;
    }
    auto fontCollection = test_font_collection();
    const char* text = "DynamicTrackingTest Hello World";
    auto perPoint = [&](TextShapingEngine engine, const char* family, SkScalar fontSize) {
        return longest_line(reporter, fontCollection, engine, text, family, fontSize, 0) / fontSize;
    };

    for (TextShapingEngine engine : kEngines) {
        skiatest::ReporterContext context(reporter, SkString(engine_name(engine)));
        const SkScalar helvetica11 = perPoint(engine, "Helvetica", 11.0f);
        const SkScalar helvetica48 = perPoint(engine, "Helvetica", 48.0f);
        REPORTER_ASSERT(reporter,
                        SkScalarNearlyEqual(helvetica11, helvetica48, 0.01f),
                        "Helvetica width per point: got %f at 48pt, want %f as at 11pt",
                        helvetica48,
                        helvetica11);
    }

    const TextShapingEngine ct = TextShapingEngine::kCoreText;
    const SkScalar sf11 = perPoint(ct, ".AppleSystemUIFont", 11.0f);
    const SkScalar sf17 = perPoint(ct, ".AppleSystemUIFont", 17.0f);
    const SkScalar sf48 = perPoint(ct, ".AppleSystemUIFont", 48.0f);
    const SkScalar ratio28to14 =
            longest_line(reporter, fontCollection, ct, text, ".AppleSystemUIFont", 28.0f, 0) /
            longest_line(reporter, fontCollection, ct, text, ".AppleSystemUIFont", 14.0f, 0);
    // #22572 is the 17pt HIG body size, #18915 28pt vs 14pt, #150824 48pt vs 11pt
    REPORTER_ASSERT(reporter, sf11 - sf17 > 0.3f, "11pt - 17pt per point: got %f", sf11 - sf17);
    REPORTER_ASSERT(reporter, ratio28to14 < 1.88f, "28pt/14pt: got %f", ratio28to14);
    REPORTER_ASSERT(reporter, sf11 - sf48 > 1.5f, "11pt - 48pt per point: got %f", sf11 - sf48);

#if !defined(SK_PARAGRAPH_DISABLE_HARFBUZZ) && defined(SK_UNICODE_ICU_IMPLEMENTATION)
    // HarfBuzz is printed, not asserted: other changes may fix it
    const TextShapingEngine hb = TextShapingEngine::kDefault;
    const SkScalar hb11 = perPoint(hb, ".AppleSystemUIFont", 11.0f);
    INFOF(reporter,
          "HarfBuzz: 11pt - 17pt %f, 28pt/14pt %f, 11pt - 48pt %f\n",
          hb11 - perPoint(hb, ".AppleSystemUIFont", 17.0f),
          perPoint(hb, ".AppleSystemUIFont", 28.0f) * 2 / perPoint(hb, ".AppleSystemUIFont", 14.0f),
          hb11 - perPoint(hb, ".AppleSystemUIFont", 48.0f));
#endif
}

// Flutter 150824 at its own sizes and texts: CupertinoSystemText and CupertinoSystemDisplay
// resolve to the face SwiftUI's .system(size:) uses and keep its trak, so the line is nearer
// the native tracked CTLine than the trak-free one (a property, not native equality)
DEF_TEST(SkParagraph_Flutter_150824_CupertinoAliasesKeepSystemTrak, reporter) {
    if (!has_font_families({".AppleSystemUIFont"})) {
        return;
    }
    auto fontCollection = test_font_collection();
    struct IssueSizes {
        const char* family;
        const char* label;
        std::initializer_list<int> sizes;
    };
    const std::array<IssueSizes, 2> kIssueSizes = {{
            {"CupertinoSystemText",
             "CupertinoSystemText",
             {3, 4, 5, 7, 8, 9, 10, 11, 13, 15, 17, 19}},
            {"CupertinoSystemDisplay", "SystemDisplay", {20, 22, 25, 28, 32, 36}},
    }};
    for (const IssueSizes& issue : kIssueSizes) {
        for (int size : issue.sizes) {
            const SkString line = SkStringPrintf("%s size %d", issue.label, size);
            skiatest::ReporterContext context(reporter, line);
            const SkScalar fontSize = static_cast<SkScalar>(size);
            SkUniqueCFRef<CTFontRef> system(
                    CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, fontSize, nullptr));
            SkUniqueCFRef<CFStringRef> systemPsName(CTFontCopyPostScriptName(system.get()));
            char systemName[128] = {};
            CFStringGetCString(
                    systemPsName.get(), systemName, sizeof(systemName), kCFStringEncodingUTF8);
            const NativeTrakWidths native = native_system_trak_widths(line.c_str(), fontSize);
            // Without trak at this size any width would be as near one native line as the other
            REPORTER_ASSERT(reporter,
                            SkScalarAbs(native.tracked - native.untracked) > 0.01f,
                            "trak moves the native line by %f",
                            native.tracked - native.untracked);

            auto paragraph = layout_font(reporter,
                                         fontCollection,
                                         TextShapingEngine::kCoreText,
                                         line.c_str(),
                                         issue.family,
                                         fontSize,
                                         0,
                                         2000.0f);
            const std::vector<Paragraph::FontInfo> fonts = paragraph->getFonts();
            REPORTER_ASSERT(reporter,
                            paragraph->lineNumber() == 1,
                            "got %zu lines",
                            paragraph->lineNumber());
            REPORTER_ASSERT(reporter, fonts.size() == 1, "got %zu runs", fonts.size());
            if (fonts.size() != 1) {
                continue;
            }
            const SkString face = postscript_name(*fonts[0].fFont.getTypeface());
            REPORTER_ASSERT(
                    reporter, face.equals(systemName), "got %s, want %s", face.c_str(), systemName);
            const SkScalar width = paragraph->getLongestLine();
            REPORTER_ASSERT(reporter,
                            SkScalarNearlyEqual(width, native.tracked, 1.0f / 4096.0f),
                            "got %g, want %g (untracked %g)",
                            width,
                            native.tracked,
                            native.untracked);
        }
    }
}

// Flutter 22572: CupertinoTextThemeData's HIG letterSpacing (-0.41 at 17pt, +0.36 at 28pt) adds
// to .SFNS's built-in trak instead of replacing it: the spaced line differs from the unspaced
// one by letterSpacing per glyph, and the unspaced line keeps the trak
DEF_TEST(SkParagraph_Flutter_22572_CupertinoLetterSpacingStacksOnTrak, reporter) {
    if (!has_font_families({".AppleSystemUIFont"})) {
        return;
    }
    auto fontCollection = test_font_collection();
    const char* text = "DynamicTrackingTest Hello World";
    struct Cupertino {
        const char* family;
        SkScalar size;
        SkScalar letterSpacing;
    };
    for (const Cupertino& c : {Cupertino{"CupertinoSystemText", 17.0f, -0.41f},
                               Cupertino{"CupertinoSystemDisplay", 28.0f, 0.36f}}) {
        for (TextShapingEngine engine : kEngines) {
            skiatest::ReporterContext context(
                    reporter, SkStringPrintf("%s %s %gpt", engine_name(engine), c.family, c.size));
            auto unspaced = layout_font(
                    reporter, fontCollection, engine, text, c.family, c.size, 0, 2000.0f);
            auto spaced = layout_font(reporter,
                                      fontCollection,
                                      engine,
                                      text,
                                      c.family,
                                      c.size,
                                      c.letterSpacing,
                                      2000.0f);
            const SkScalar delta = spaced->getLongestLine() - unspaced->getLongestLine();
            const int glyphs = static_cast<int>(drawn_glyphs(spaced.get()).size());
            const NativeTrakWidths native = native_system_trak_widths(text, c.size);
            const SkScalar width = unspaced->getLongestLine();
            if (engine != TextShapingEngine::kCoreText) {
                // HarfBuzz is printed, not asserted: other changes may fix it
                INFOF(reporter,
                      "HarfBuzz: letterSpacing delta %f over %d glyphs, width %f, native %f, "
                      "native without trak %f\n",
                      delta,
                      glyphs,
                      width,
                      native.tracked,
                      native.untracked);
                continue;
            }
            REPORTER_ASSERT(reporter,
                            SkScalarNearlyEqual(delta, glyphs * c.letterSpacing, 0.01f),
                            "letterSpacing delta: got %f, want %f",
                            delta,
                            glyphs * c.letterSpacing);
            REPORTER_ASSERT(reporter,
                            SkScalarAbs(native.tracked - native.untracked) > 0.01f,
                            "trak moves the native line by %f",
                            native.tracked - native.untracked);
            REPORTER_ASSERT(reporter,
                            SkScalarNearlyEqual(width, native.tracked, 1.0f / 4096.0f),
                            "got %g, want %g (untracked %g)",
                            width,
                            native.tracked,
                            native.untracked);
        }
    }
}

// No tracked issue (related: Flutter 36062): on keycap emoji sequences ("1️⃣2️⃣3️⃣"), CoreText
// collapses each keycap into 1 Apple Color Emoji glyph and 1 cluster, so letterSpacing=16 adds
// 48px; HarfBuzz was measured to leave a phantom zero-advance glyph per U+FE0F selector, which
// Run::addLetterSpacesEvenly spaces as well
DEF_TEST(SkParagraph_KeycapEmojiClusterLetterSpacing, reporter) {
    if (!has_font_families({".AppleSystemUIFont", "Apple Color Emoji"})) {
        return;
    }
    auto fontCollection = test_font_collection();
    fontCollection->enableFontFallback();
    const char* text =
            "1\xEF\xB8\x8F\xE2\x83\xA3"
            "2\xEF\xB8\x8F\xE2\x83\xA3"
            "3\xEF\xB8\x8F\xE2\x83\xA3";
    constexpr int kKeycaps = 3;
    constexpr TextIndex kKeycapBytes = 7;

    for (TextShapingEngine engine : kEngines) {
        skiatest::ReporterContext context(reporter, SkString(engine_name(engine)));
        auto p0 = layout_font(
                reporter, fontCollection, engine, text, ".AppleSystemUIFont", 28.0f, 0.0f, 500.0f);
        auto p16 = layout_font(
                reporter, fontCollection, engine, text, ".AppleSystemUIFont", 28.0f, 16.0f, 500.0f);
        REPORTER_ASSERT(reporter, p0->unresolvedGlyphs() == 0, "got %d", p0->unresolvedGlyphs());
        REPORTER_ASSERT(reporter, p16->unresolvedGlyphs() == 0, "got %d", p16->unresolvedGlyphs());

        const SkScalar widthDelta = p16->getLongestLine() - p0->getLongestLine();
        const int glyphs16 = static_cast<int>(drawn_glyphs(p16.get()).size());
        if (engine != TextShapingEngine::kCoreText) {
            // HarfBuzz is printed, not asserted: other changes may fix it, and on macOS 14 with
            // Fontations it splits the keycaps across runs
            INFOF(reporter,
                  "HarfBuzz: %zu runs, %d glyphs, 0->16px delta %f\n",
                  p16->getFonts().size(),
                  glyphs16,
                  widthDelta);
            continue;
        }
        const int glyphs0 = static_cast<int>(drawn_glyphs(p0.get()).size());
        check_all_runs_in_emoji_font(reporter, *p0);
        check_all_runs_in_emoji_font(reporter, *p16);
        REPORTER_ASSERT(reporter, glyphs0 == kKeycaps, "got %d, want %d", glyphs0, kKeycaps);
        REPORTER_ASSERT(reporter, glyphs16 == kKeycaps, "got %d, want %d", glyphs16, kKeycaps);
        for (TextIndex start = 0; start < kKeycaps * kKeycapBytes; start += kKeycapBytes) {
            skiatest::ReporterContext keycap(reporter, SkStringPrintf("keycap at %zu", start));
            // The U+20E3 byte at start + 4 must belong to the cluster of its digit
            Paragraph::GlyphClusterInfo cluster;
            REPORTER_ASSERT(reporter, p0->getGlyphClusterAt(start + 4, &cluster));
            REPORTER_ASSERT(reporter,
                            cluster.fClusterTextRange ==
                                    skia::textlayout::TextRange(start, start + kKeycapBytes),
                            "got [%zu, %zu), want [%zu, %zu)",
                            cluster.fClusterTextRange.start,
                            cluster.fClusterTextRange.end,
                            start,
                            start + kKeycapBytes);
        }
        REPORTER_ASSERT(reporter,
                        SkScalarNearlyEqual(widthDelta, kKeycaps * 16.0f, 0.1f),
                        "got %f, want %f",
                        widthDelta,
                        kKeycaps * 16.0f);
    }
}

// Flutter 36062: keycap sequences render as digit + box when the primary font maps both the
// digit and U+20E3 (Lucida Grande does). CoreText sends each keycap to Apple Color Emoji as one
// glyph; HarfBuzz was measured to keep the digit and U+20E3 in the primary font
DEF_TEST(SkParagraph_Flutter_36062_KeycapPrimaryFontMapsU20E3, reporter) {
    if (!has_font_families({"Lucida Grande", "Apple Color Emoji"})) {
        return;
    }
    auto fontCollection = test_font_collection();
    fontCollection->enableFontFallback();
    const char* text =
            "#\xEF\xB8\x8F\xE2\x83\xA3 *\xEF\xB8\x8F\xE2\x83\xA3 0\xEF\xB8\x8F\xE2\x83\xA3 "
            "1\xEF\xB8\x8F\xE2\x83\xA3 9\xEF\xB8\x8F\xE2\x83\xA3";
    constexpr int kKeycaps = 5;

    for (TextShapingEngine engine : kEngines) {
        skiatest::ReporterContext context(reporter, SkString(engine_name(engine)));
        auto paragraph = layout_font(
                reporter, fontCollection, engine, text, "Lucida Grande", 20.0f, 0, 2000);
        int emojiGlyphs = 0;
        int otherKeycapGlyphs = 0;
        for (const DrawnGlyph& glyph : drawn_glyphs(paragraph.get())) {
            // Spaces may stay in the primary font; every other glyph belongs to a keycap
            if (text[glyph.utf8Start] == ' ') {
                continue;
            }
            ++(family_name(glyph.font).contains("Emoji") ? emojiGlyphs : otherKeycapGlyphs);
        }
        if (engine != TextShapingEngine::kCoreText) {
            // HarfBuzz is printed, not asserted: other changes may fix it
            INFOF(reporter,
                  "HarfBuzz: %d keycap glyphs in Apple Color Emoji, %d elsewhere\n",
                  emojiGlyphs,
                  otherKeycapGlyphs);
            continue;
        }
        REPORTER_ASSERT(reporter,
                        otherKeycapGlyphs == 0,
                        "got %d keycap glyphs outside Apple Color Emoji",
                        otherKeycapGlyphs);
        REPORTER_ASSERT(reporter,
                        emojiGlyphs == kKeycaps,
                        "got %d emoji glyphs, want %d",
                        emojiGlyphs,
                        kKeycaps);
    }
}

// No tracked issue: on ZWJ emoji sequences containing U+FE0F variation selectors
// ("🏳️‍🌈❤️‍🔥👩‍💻"), CoreText emits 3 fused glyphs (+48px under letterSpacing=16); HarfBuzz was
// measured to emit 5 glyphs (phantom U+FE0F glyphs in 🏳️‍🌈 and ❤️‍🔥) that all receive spacing
DEF_TEST(SkParagraph_ZWJEmojiVariationSelectorLetterSpacing, reporter) {
    if (!has_font_families({".AppleSystemUIFont", "Apple Color Emoji"})) {
        return;
    }
    auto fontCollection = test_font_collection();
    fontCollection->enableFontFallback();
    const char* zwjText =
            "\xF0\x9F\x8F\xB3\xEF\xB8\x8F\xE2\x80\x8D\xF0\x9F\x8C\x88"
            "\xE2\x9D\xA4\xEF\xB8\x8F\xE2\x80\x8D\xF0\x9F\x94\xA5"
            "\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x92\xBB";

    for (TextShapingEngine engine : kEngines) {
        skiatest::ReporterContext context(reporter, SkString(engine_name(engine)));
        auto p0 = layout_font(reporter,
                              fontCollection,
                              engine,
                              zwjText,
                              ".AppleSystemUIFont",
                              28.0f,
                              0.0f,
                              500.0f);
        auto p16 = layout_font(reporter,
                               fontCollection,
                               engine,
                               zwjText,
                               ".AppleSystemUIFont",
                               28.0f,
                               16.0f,
                               500.0f);
        REPORTER_ASSERT(reporter, p0->unresolvedGlyphs() == 0, "got %d", p0->unresolvedGlyphs());
        REPORTER_ASSERT(reporter, p16->unresolvedGlyphs() == 0, "got %d", p16->unresolvedGlyphs());

        const SkScalar widthDelta = p16->getLongestLine() - p0->getLongestLine();
        const int glyphs16 = static_cast<int>(drawn_glyphs(p16.get()).size());
        if (engine != TextShapingEngine::kCoreText) {
            // HarfBuzz is printed, not asserted: other changes may fix it, and on macOS 14 with
            // Fontations it splits the sequences across runs
            INFOF(reporter,
                  "HarfBuzz: %zu runs, %d glyphs, 0->16px delta %f\n",
                  p16->getFonts().size(),
                  glyphs16,
                  widthDelta);
            continue;
        }
        const int glyphs0 = static_cast<int>(drawn_glyphs(p0.get()).size());
        check_all_runs_in_emoji_font(reporter, *p0);
        check_all_runs_in_emoji_font(reporter, *p16);
        REPORTER_ASSERT(reporter, glyphs0 == 3, "got %d", glyphs0);
        REPORTER_ASSERT(reporter, glyphs16 == 3, "got %d", glyphs16);
        REPORTER_ASSERT(
                reporter, SkScalarNearlyEqual(widthDelta, 48.0f, 0.1f), "got %f", widthDelta);
    }
}

// Flutter 118127: on decomposed "ax\u0301b" (which has no precomposed NFC form), CoreText's
// tracking keeps the combining acute anchored to 'x' under letterSpacing=10 and expands width by
// 3 clusters (30px); HarfBuzz was measured to shift the combining mark away from its base too
DEF_TEST(SkParagraph_Flutter_118127_CombiningDiacriticsLetterSpacing, reporter) {
    if (!has_font_families({"Helvetica"})) {
        return;
    }
    auto fontCollection = test_font_collection();
    const char* text =
            "ax\xCC\x81"
            "b";

    for (TextShapingEngine engine : kEngines) {
        skiatest::ReporterContext context(reporter, SkString(engine_name(engine)));
        auto p0 = layout_font(
                reporter, fontCollection, engine, text, "Helvetica", 20.0f, 0.0f, 500.0f);
        auto p10 = layout_font(
                reporter, fontCollection, engine, text, "Helvetica", 20.0f, 10.0f, 500.0f);
        const std::vector<DrawnGlyph> g0 = drawn_glyphs(p0.get());
        const std::vector<DrawnGlyph> g10 = drawn_glyphs(p10.get());
        const SkScalar widthDelta = p10->getLongestLine() - p0->getLongestLine();
        if (engine != TextShapingEngine::kCoreText) {
            // HarfBuzz is printed, not asserted: other changes may fix it
            INFOF(reporter,
                  "HarfBuzz: %zu glyphs, mark offset ls0=%f ls10=%f, 0->10px delta %f\n",
                  g10.size(),
                  g0.size() == 4 ? g0[2].position.fX - g0[1].position.fX : 0,
                  g10.size() == 4 ? g10[2].position.fX - g10[1].position.fX : 0,
                  widthDelta);
            continue;
        }
        // One glyph each for 'a', 'x', U+0301, 'b', so glyphs 1 and 2 are the base and its mark
        REPORTER_ASSERT(reporter, g0.size() == 4, "got %zu glyphs", g0.size());
        REPORTER_ASSERT(reporter, g10.size() == 4, "got %zu glyphs", g10.size());
        if (g0.size() != 4 || g10.size() != 4) {
            continue;
        }
        const SkScalar markDelta0 = g0[2].position.fX - g0[1].position.fX;
        const SkScalar markDelta10 = g10[2].position.fX - g10[1].position.fX;
        REPORTER_ASSERT(reporter,
                        SkScalarNearlyEqual(markDelta10, markDelta0, 0.05f),
                        "mark offset from its base: got %f, want %f as unspaced",
                        markDelta10,
                        markDelta0);
        REPORTER_ASSERT(
                reporter, SkScalarNearlyEqual(widthDelta, 30.0f, 0.05f), "got %f", widthDelta);
    }
}

// Flutter 144528: on Devanagari "हिन्दी" (2 orthographic syllables, 4 glyphs: pre-base matra ि,
// base ह, conjunct न्द्, post-base matra ी), CoreText's tracking spaces the 2 syllables as intact
// units (+32px at letterSpacing=16); HarfBuzz was measured to space every glyph, detaching both
// matras
DEF_TEST(SkParagraph_Flutter_144528_IndicConjunctLetterSpacing, reporter) {
    if (!has_font_families({"Kohinoor Devanagari"})) {
        return;
    }
    auto fontCollection = test_font_collection();
    const char* hindi = "\xE0\xA4\xB9\xE0\xA4\xBF\xE0\xA4\xA8\xE0\xA5\x8D\xE0\xA4\xA6\xE0\xA5\x80";

    for (TextShapingEngine engine : kEngines) {
        skiatest::ReporterContext context(reporter, SkString(engine_name(engine)));
        auto p0 = layout_font(reporter,
                              fontCollection,
                              engine,
                              hindi,
                              "Kohinoor Devanagari",
                              24.0f,
                              0.0f,
                              500.0f);
        auto p16 = layout_font(reporter,
                               fontCollection,
                               engine,
                               hindi,
                               "Kohinoor Devanagari",
                               24.0f,
                               16.0f,
                               500.0f);
        const std::vector<DrawnGlyph> g0 = drawn_glyphs(p0.get());
        const std::vector<DrawnGlyph> g16 = drawn_glyphs(p16.get());
        const SkScalar widthDelta = p16->getLongestLine() - p0->getLongestLine();
        if (engine != TextShapingEngine::kCoreText) {
            // HarfBuzz is printed, not asserted: other changes may fix it
            INFOF(reporter,
                  "HarfBuzz: %zu glyphs, syllable 1 span ls0=%f ls16=%f, 0->16px delta %f\n",
                  g16.size(),
                  g0.size() == 4 ? g0[1].position.fX - g0[0].position.fX : 0,
                  g16.size() == 4 ? g16[1].position.fX - g16[0].position.fX : 0,
                  widthDelta);
            continue;
        }
        REPORTER_ASSERT(reporter, p0->unresolvedGlyphs() == 0, "got %d", p0->unresolvedGlyphs());
        REPORTER_ASSERT(reporter, p16->unresolvedGlyphs() == 0, "got %d", p16->unresolvedGlyphs());
        REPORTER_ASSERT(reporter, g0.size() == 4, "got %zu glyphs", g0.size());
        REPORTER_ASSERT(reporter, g16.size() == 4, "got %zu glyphs", g16.size());
        if (g0.size() != 4 || g16.size() != 4) {
            continue;
        }
        // Glyphs 0 and 1 are the pre-base matra and its base ह, the first syllable
        const SkScalar syllable1Span0 = g0[1].position.fX - g0[0].position.fX;
        const SkScalar syllable1Span16 = g16[1].position.fX - g16[0].position.fX;
        REPORTER_ASSERT(reporter,
                        SkScalarNearlyEqual(syllable1Span16, syllable1Span0, 0.05f),
                        "syllable 1 span: got %f, want %f as unspaced",
                        syllable1Span16,
                        syllable1Span0);
        REPORTER_ASSERT(
                reporter, SkScalarNearlyEqual(widthDelta, 32.0f, 0.1f), "got %f", widthDelta);
    }
}

// Flutter 147708: CupertinoSystemText and CupertinoSystemDisplay followed by a fontFamilyFallback
// list resolve to the system UI face (keeping the 28pt/14pt opsz contraction) before the list is
// consulted, while look-alike names ("SF Pro", "system-ui") never resolve to it. HarfBuzz was
// measured to miss both aliases and fall through to the first fallback family (Helvetica)
DEF_TEST(SkParagraph_Flutter_147708_CupertinoAliasesBypassFallback, reporter) {
    if (!has_font_families({".AppleSystemUIFont", "Helvetica"})) {
        return;
    }
    auto fontCollection = test_font_collection();
    fontCollection->enableFontFallback();
    const char* text = "DynamicTrackingTest Hello World";

    for (const char* alias : {"CupertinoSystemText", "CupertinoSystemDisplay"}) {
        for (TextShapingEngine engine : kEngines) {
            skiatest::ReporterContext context(reporter,
                                              SkStringPrintf("%s %s", engine_name(engine), alias));
            auto p14 = layout_styled(reporter,
                                     fontCollection,
                                     engine,
                                     style_with_helvetica_after(alias, 14.0f),
                                     text,
                                     2000.0f);
            auto p28 = layout_styled(reporter,
                                     fontCollection,
                                     engine,
                                     style_with_helvetica_after(alias, 28.0f),
                                     text,
                                     2000.0f);
            const std::vector<Paragraph::FontInfo> fonts = p28->getFonts();
            const SkScalar ratio = p28->getLongestLine() / p14->getLongestLine();
            if (engine != TextShapingEngine::kCoreText) {
                // HarfBuzz is printed, not asserted: other changes may fix it
                INFOF(reporter,
                      "HarfBuzz %s: %s, 28/14pt %f\n",
                      alias,
                      fonts.empty() ? "no runs"
                                    : postscript_name(*fonts[0].fFont.getTypeface()).c_str(),
                      ratio);
                continue;
            }
            REPORTER_ASSERT(reporter, fonts.size() == 1, "got %zu runs", fonts.size());
            if (fonts.size() != 1) {
                continue;
            }
            const SkString psName = postscript_name(*fonts[0].fFont.getTypeface());
            REPORTER_ASSERT(reporter,
                            psName.startsWith(".AppleSystemUIFont") || psName.startsWith(".SF"),
                            "fell through to %s",
                            psName.c_str());
            REPORTER_ASSERT(reporter, ratio < 1.88f, "28pt/14pt: got %f", ratio);
        }
    }

    // Only the system UI names take the system descriptor path: a look-alike resolves as an
    // installed family or falls through to Helvetica, never to a hidden '.' system face
    for (const char* lookAlike : {"SF Pro", "system-ui", "-apple-system", "CourierNewCustom"}) {
        skiatest::ReporterContext context(reporter, SkString(lookAlike));
        auto paragraph = layout_styled(reporter,
                                       fontCollection,
                                       TextShapingEngine::kCoreText,
                                       style_with_helvetica_after(lookAlike, 14.0f),
                                       text,
                                       2000.0f);
        const std::vector<Paragraph::FontInfo> fonts = paragraph->getFonts();
        REPORTER_ASSERT(reporter, !fonts.empty());
        const bool installed = has_font_families({lookAlike});
        for (const Paragraph::FontInfo& info : fonts) {
            const SkString psName = postscript_name(*info.fFont.getTypeface());
            REPORTER_ASSERT(reporter,
                            !psName.startsWith(".") && (installed || psName.equals("Helvetica")),
                            "got %s",
                            psName.c_str());
        }
    }
}

// Flutter 193812: .AppleSystemUIFontRounded, .AppleSystemUIFontSerif and
// .AppleSystemUIFontMonospaced, Regular and Bold, resolve to Apple's Rounded, NewYork and SF Mono
// UI design faces on CoreText even with a fallback family after them
DEF_TEST(SkParagraph_Flutter_193812_SystemDesignFaces, reporter) {
    if (!has_font_families({".AppleSystemUIFont", "Helvetica"})) {
        return;
    }
    auto fontCollection = test_font_collection();
    fontCollection->enableFontFallback();
    constexpr struct {
        const char* fFamily;
        const char* fRegularPs;
        const char* fBoldPs;
    } kDesigns[] = {
            {".AppleSystemUIFontRounded", "Rounded-Regular", "Rounded-Bold"},
            {".AppleSystemUIFontSerif", "NewYork-Regular", "NewYork-Bold"},
            {".AppleSystemUIFontMonospaced", "Monospaced-Regular", "Monospaced-Bold"},
    };
    for (const auto& d : kDesigns) {
        for (bool bold : {false, true}) {
            for (TextShapingEngine engine : kEngines) {
                skiatest::ReporterContext context(reporter,
                                                  SkStringPrintf("%s %s %s",
                                                                 engine_name(engine),
                                                                 d.fFamily,
                                                                 bold ? "Bold" : "Regular"));
                const SkFontStyle fontStyle = bold ? SkFontStyle::Bold() : SkFontStyle::Normal();
                auto paragraph =
                        layout_styled(reporter,
                                      fontCollection,
                                      engine,
                                      style_with_helvetica_after(d.fFamily, 24.0f, fontStyle),
                                      "System UI Font Design Variants",
                                      1000.0f);
                const std::vector<Paragraph::FontInfo> fonts = paragraph->getFonts();
                if (engine != TextShapingEngine::kCoreText) {
                    // HarfBuzz is printed, not asserted: other changes may fix it
                    INFOF(reporter,
                          "HarfBuzz: %s\n",
                          fonts.empty() ? "no runs"
                                        : postscript_name(*fonts[0].fFont.getTypeface()).c_str());
                    continue;
                }
                REPORTER_ASSERT(reporter, fonts.size() == 1, "got %zu runs", fonts.size());
                if (fonts.size() != 1) {
                    continue;
                }
                const SkString psName = postscript_name(*fonts[0].fFont.getTypeface());
                const char* wantPs = bold ? d.fBoldPs : d.fRegularPs;
                REPORTER_ASSERT(reporter,
                                psName.contains(wantPs),
                                "got '%s', want '%s'",
                                psName.c_str(),
                                wantPs);
            }
        }
    }
}

// Flutter 193812: CoreText instantiates the Rounded and Serif designs at the layout size, where
// their trak (and NewYork's opsz) makes the width per point change with the size; HarfBuzz was
// measured to use size-0 CTFonts without trak. SF Mono has neither and stays linear
DEF_TEST(SkParagraph_Flutter_193812_DesignsTrackAtLayoutSize, reporter) {
    if (!has_font_families({".AppleSystemUIFont"})) {
        return;
    }
    auto fontCollection = test_font_collection();
    const char* text = "System UI Font Design Variants";
    constexpr struct {
        const char* fFamily;
        bool fTracked;
    } kDesigns[] = {
            {".AppleSystemUIFontRounded", true},
            {".AppleSystemUIFontSerif", true},
            {".AppleSystemUIFontMonospaced", false},
    };
    for (const auto& d : kDesigns) {
        for (TextShapingEngine engine : kEngines) {
            skiatest::ReporterContext context(
                    reporter, SkStringPrintf("%s %s", engine_name(engine), d.fFamily));
            const SkScalar perPoint12 =
                    longest_line(reporter, fontCollection, engine, text, d.fFamily, 12.0f, 0) / 12;
            const SkScalar perPoint48 =
                    longest_line(reporter, fontCollection, engine, text, d.fFamily, 48.0f, 0) / 48;
            if (engine != TextShapingEngine::kCoreText) {
                // HarfBuzz is printed, not asserted: other changes may fix it
                INFOF(reporter,
                      "HarfBuzz: width per point 12pt %f, 48pt %f\n",
                      perPoint12,
                      perPoint48);
                continue;
            }
            if (d.fTracked) {
                REPORTER_ASSERT(reporter,
                                perPoint12 - perPoint48 > 0.5f,
                                "12pt - 48pt per point: got %f",
                                perPoint12 - perPoint48);
            } else {
                REPORTER_ASSERT(reporter,
                                SkScalarNearlyEqual(perPoint12, perPoint48, 0.01f),
                                "width per point: got %f at 48pt, want %f as at 12pt",
                                perPoint48,
                                perPoint12);
            }
        }
    }
}

// Flutter 193812: .AppleSystemUIFontMonospaced gives narrow and wide letters the same advance on
// CoreText, as SF Mono does natively
DEF_TEST(SkParagraph_Flutter_193812_MonospacedEqualAdvances, reporter) {
    if (!has_font_families({".AppleSystemUIFont"})) {
        return;
    }
    auto fontCollection = test_font_collection();
    for (TextShapingEngine engine : kEngines) {
        skiatest::ReporterContext context(reporter, SkString(engine_name(engine)));
        const SkScalar narrow = longest_line(reporter,
                                             fontCollection,
                                             engine,
                                             "iiiiiiii",
                                             ".AppleSystemUIFontMonospaced",
                                             24,
                                             0);
        const SkScalar wide = longest_line(reporter,
                                           fontCollection,
                                           engine,
                                           "WWWWWWWW",
                                           ".AppleSystemUIFontMonospaced",
                                           24,
                                           0);
        if (engine != TextShapingEngine::kCoreText) {
            // HarfBuzz is printed, not asserted: other changes may fix it
            INFOF(reporter, "HarfBuzz: 'iiiiiiii' %f, 'WWWWWWWW' %f\n", narrow, wide);
            continue;
        }
        REPORTER_ASSERT(reporter,
                        SkScalarNearlyEqual(narrow, wide, 0.05f),
                        "'iiiiiiii': got %f, want %f as 'WWWWWWWW'",
                        narrow,
                        wide);
    }
}

// Flutter 109308 (partial, automatic optical size only): with no explicit opsz, CoreText sets the
// .SFNS opsz coordinate from the point size (17 at 12pt, 72 at 72pt) so glyph advances per em
// shrink at display sizes; HarfBuzz was measured to keep opsz 17 and identical advances per em at
// both sizes. Explicit axis values and the remaining variable font requests are out of scope
DEF_TEST(SkParagraph_Flutter_109308_AutoOpticalSize, reporter) {
    if (!has_font_families({".AppleSystemUIFont"})) {
        return;
    }
    auto fontCollection = test_font_collection();
    const char* text = "Variable fonts honor axes";
    const SkScalar sizes[2] = {12.0f, 72.0f};

    for (TextShapingEngine engine : kEngines) {
        skiatest::ReporterContext context(reporter, SkString(engine_name(engine)));
        const bool coreText = engine == TextShapingEngine::kCoreText;
        SkScalar advancePerEm[2] = {0, 0};
        SkScalar opsz[2] = {-1, -1};
        for (int i = 0; i < 2; ++i) {
            auto p = layout_font(reporter,
                                 fontCollection,
                                 engine,
                                 text,
                                 ".AppleSystemUIFont",
                                 sizes[i],
                                 0.0f,
                                 2000.0f);
            const std::vector<Paragraph::FontInfo> fonts = p->getFonts();
            if (coreText) {
                REPORTER_ASSERT(reporter,
                                p->unresolvedGlyphs() == 0,
                                "%gpt: got %d unresolved",
                                sizes[i],
                                p->unresolvedGlyphs());
                REPORTER_ASSERT(
                        reporter, fonts.size() == 1, "%gpt: got %zu runs", sizes[i], fonts.size());
            }
            // The font's own advances, so CoreText 'trak' does not count as optical sizing
            SkScalar sum = 0;
            for (const DrawnGlyph& glyph : drawn_glyphs(p.get())) {
                sum += glyph.font.getWidth(glyph.glyph);
            }
            advancePerEm[i] = sum / sizes[i];
            if (!fonts.empty()) {
                opsz[i] = axis_coordinate(fonts[0].fFont.getTypeface(),
                                          SkSetFourByteTag('o', 'p', 's', 'z'))
                                  .value_or(-1);
            }
        }
        if (!coreText) {
            // HarfBuzz is printed, not asserted: other changes may fix it
            INFOF(reporter,
                  "HarfBuzz: opsz %f -> %f, advance per em %f -> %f\n",
                  opsz[0],
                  opsz[1],
                  advancePerEm[0],
                  advancePerEm[1]);
            continue;
        }
        REPORTER_ASSERT(
                reporter, opsz[1] > opsz[0], "opsz: got %f at 72pt, %f at 12pt", opsz[1], opsz[0]);
        REPORTER_ASSERT(reporter,
                        advancePerEm[0] > advancePerEm[1] * 1.01f,
                        "advance per em: got %f at 72pt, %f at 12pt",
                        advancePerEm[1],
                        advancePerEm[0]);
    }
}

// Flutter 144099: Arabic text styled with CupertinoSystemText renders its Arabic runs in SF Arabic
// on CoreText, as native iOS text does; HarfBuzz was measured to miss the alias and fall back to
// Geeza Pro for Arabic and Helvetica for the space and colon
DEF_TEST(SkParagraph_Flutter_144099_CupertinoArabicSystemFont, reporter) {
    if (!has_font_families({".AppleSystemUIFont"})) {
        return;
    }
    auto fontCollection = test_font_collection();
    fontCollection->enableFontFallback();
    const char* text = "\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7 \xD9\xA1\xD9\xA1:\xD9\xA4\xD9\xA0";
    TextStyle ts;
    ts.setFontFamilies({SkString("CupertinoSystemText")});
    ts.setFontSize(20.0f);
    ts.setLocale(SkString("ar"));

    for (TextShapingEngine engine : kEngines) {
        skiatest::ReporterContext context(reporter, SkString(engine_name(engine)));
        auto paragraph = layout_styled(reporter, fontCollection, engine, ts, text, 1000.0f);
        const std::vector<Paragraph::FontInfo> fonts = paragraph->getFonts();
        if (engine != TextShapingEngine::kCoreText) {
            SkString runs;
            for (const Paragraph::FontInfo& info : fonts) {
                runs.appendf(" [%zu, %zu) %s",
                             info.fTextRange.start,
                             info.fTextRange.end,
                             family_name(info.fFont).c_str());
            }
            // HarfBuzz is printed, not asserted: other changes may fix it
            INFOF(reporter, "HarfBuzz runs:%s\n", runs.c_str());
            continue;
        }
        REPORTER_ASSERT(reporter,
                        paragraph->unresolvedGlyphs() == 0,
                        "got %d",
                        paragraph->unresolvedGlyphs());
        REPORTER_ASSERT(reporter, !fonts.empty());
        for (const Paragraph::FontInfo& info : fonts) {
            // Space and ':' are common script and may stay in the Latin face; every Arabic letter
            // and Arabic-Indic digit here is a 0xD8/0xD9-led UTF-8 sequence
            const unsigned char lead = text[info.fTextRange.start];
            if (lead != 0xD8 && lead != 0xD9) {
                continue;
            }
            const SkString family = family_name(info.fFont);
            REPORTER_ASSERT(reporter,
                            family.contains("SF Arabic"),
                            "Arabic run [%zu, %zu): got %s",
                            info.fTextRange.start,
                            info.fTextRange.end,
                            family.c_str());
        }
    }
}

// Flutter 130492: Italic and BoldItalic of the system UI font, its Cupertino alias and its Serif
// and Monospaced designs render with the face's own slant and weight alone on CoreText (skewX 0,
// no embolden): CTFontCopyFontDescriptor omits kCTFontSlantTrait from these italic faces, and
// HarfBuzz was measured to stack synthetic skewX = -0.25 on top
DEF_TEST(SkParagraph_Flutter_130492_SystemFontItalicSingleSlant, reporter) {
    if (!has_font_families({".AppleSystemUIFont"})) {
        return;
    }
    auto fontCollection = test_font_collection();
    const char* families[] = {
            ".AppleSystemUIFont",
            "CupertinoSystemText",
            ".AppleSystemUIFontSerif",
            ".AppleSystemUIFontMonospaced",
    };

    for (const char* family : families) {
        for (SkFontStyle fontStyle : {SkFontStyle::Italic(), SkFontStyle::BoldItalic()}) {
            for (TextShapingEngine engine : kEngines) {
                skiatest::ReporterContext context(reporter,
                                                  SkStringPrintf("%s %s weight %d",
                                                                 engine_name(engine),
                                                                 family,
                                                                 fontStyle.weight()));
                TextStyle ts;
                ts.setFontFamilies({SkString(family)});
                ts.setFontSize(24.0f);
                ts.setFontStyle(fontStyle);
                auto paragraph = layout_styled(
                        reporter, fontCollection, engine, ts, "The quick brown fox", 1000.0f);
                const std::vector<Paragraph::FontInfo> fonts = paragraph->getFonts();
                if (engine != TextShapingEngine::kCoreText) {
                    // HarfBuzz is printed, not asserted: other changes may fix it
                    INFOF(reporter,
                          "HarfBuzz: %s, skewX %g\n",
                          fonts.empty() ? "no runs"
                                        : postscript_name(*fonts[0].fFont.getTypeface()).c_str(),
                          fonts.empty() ? 0 : fonts[0].fFont.getSkewX());
                    continue;
                }
                REPORTER_ASSERT(reporter,
                                paragraph->unresolvedGlyphs() == 0,
                                "got %d unresolved",
                                paragraph->unresolvedGlyphs());
                REPORTER_ASSERT(reporter, fonts.size() == 1, "got %zu runs", fonts.size());
                if (fonts.size() != 1) {
                    continue;
                }
                const SkString psName = postscript_name(*fonts[0].fFont.getTypeface());
                REPORTER_ASSERT(reporter, psName.contains("Italic"), "got '%s'", psName.c_str());
                if (fontStyle == SkFontStyle::BoldItalic()) {
                    REPORTER_ASSERT(reporter, psName.contains("Bold"), "got '%s'", psName.c_str());
                }
                REPORTER_ASSERT(reporter,
                                fonts[0].fFont.getSkewX() == 0.0f,
                                "got synthetic skewX %g",
                                fonts[0].fFont.getSkewX());
                REPORTER_ASSERT(reporter, !fonts[0].fFont.isEmbolden());
            }
        }
    }
}

// Flutter 130492: the ellipsis of a truncated .AppleSystemUIFont Italic line is shaped
// separately from the line text, and keeps the face's own slant alone as well (skewX 0)
DEF_TEST(SkParagraph_Flutter_130492_EllipsisSingleSlant, reporter) {
    if (!has_font_families({".AppleSystemUIFont"})) {
        return;
    }
    auto fontCollection = test_font_collection();
    TextStyle ts;
    ts.setFontFamilies({SkString(".AppleSystemUIFont")});
    ts.setFontSize(20.0f);
    ts.setFontStyle(SkFontStyle::Italic());
    ParagraphStyle paragraphStyle;
    paragraphStyle.setMaxLines(1);
    paragraphStyle.setEllipsis(SkString("..."));
    paragraphStyle.setTextStyle(ts);
    auto paragraph = layout_styled(reporter,
                                   fontCollection,
                                   TextShapingEngine::kCoreText,
                                   ts,
                                   "System italic line that is long enough to truncate",
                                   120.0f,
                                   paragraphStyle);
    REPORTER_ASSERT(reporter, paragraph->didExceedMaxLines());
    int drawnRuns = 0;
    paragraph->visit([&](int, const Paragraph::VisitorInfo* info) {
        if (!info) {
            return;
        }
        ++drawnRuns;
        const SkString psName = postscript_name(*info->font.getTypeface());
        REPORTER_ASSERT(
                reporter, psName.contains("Italic"), "run %d: got '%s'", drawnRuns, psName.c_str());
        REPORTER_ASSERT(reporter,
                        info->font.getSkewX() == 0.0f,
                        "run %d: got synthetic skewX %g",
                        drawnRuns,
                        info->font.getSkewX());
    });
    // The ellipsis is a run of its own after the truncated text
    REPORTER_ASSERT(reporter, drawnRuns >= 2, "got %d drawn runs", drawnRuns);
}

// Flutter 130492: .AppleSystemUIFont Italic as the second family, behind Apple Color Emoji
// which has no Latin, still draws Latin text with the face's own slant alone (skewX 0); the
// fallback face must keep the requested italic instead of reading the upright-reported style
DEF_TEST(SkParagraph_Flutter_130492_CascadeItalicSingleSlant, reporter) {
    if (!has_font_families({".AppleSystemUIFont", "Apple Color Emoji"})) {
        return;
    }
    auto fontCollection = test_font_collection();
    TextStyle ts;
    ts.setFontFamilies({SkString("Apple Color Emoji"), SkString(".AppleSystemUIFont")});
    ts.setFontSize(24.0f);
    ts.setFontStyle(SkFontStyle::Italic());
    auto paragraph = layout_styled(
            reporter, fontCollection, TextShapingEngine::kCoreText, ts, "FallbackItalic", 1000.0f);
    const std::vector<Paragraph::FontInfo> fonts = paragraph->getFonts();
    REPORTER_ASSERT(reporter, fonts.size() == 1, "got %zu runs", fonts.size());
    if (fonts.size() != 1) {
        return;
    }
    const SkString psName = postscript_name(*fonts[0].fFont.getTypeface());
    REPORTER_ASSERT(reporter, psName.startsWith(".SF"), "got '%s'", psName.c_str());
    REPORTER_ASSERT(reporter, psName.contains("Italic"), "got '%s'", psName.c_str());
    REPORTER_ASSERT(reporter,
                    fonts[0].fFont.getSkewX() == 0.0f,
                    "got synthetic skewX %g",
                    fonts[0].fFont.getSkewX());
}

// Flutter 141080: in "ไก่" with a color per character, the tone mark U+0E48 must paint in its
// own color. CupertinoSystemText has no Thai, and CoreText falls back to a face that keeps the
// mark a separate glyph; HarfBuzz was measured to fall back to Thonburi, which ligates the base
// and the mark into one glyph that takes the base color
DEF_TEST(SkParagraph_Flutter_141080_CupertinoThaiToneMarkColor, reporter) {
    if (!has_font_families({".AppleSystemUIFont", "Thonburi"})) {
        return;
    }
    auto fontCollection = test_font_collection();
    fontCollection->enableFontFallback();
    const SkColor kRed = SkColorSetRGB(0xFF, 0, 0);
    const SkColor kAmber = SkColorSetRGB(0xFF, 0xC1, 0x07);
    const SkColor kGreen = SkColorSetRGB(0, 0xFF, 0);
    struct Span {
        const char* fText;
        SkColor fColor;
    };
    constexpr auto kSpans = std::to_array<Span>(
            {{"\xE0\xB9\x84", kRed}, {"\xE0\xB8\x81", kAmber}, {"\xE0\xB9\x88", kGreen}});

    for (TextShapingEngine engine : kEngines) {
        skiatest::ReporterContext context(reporter, SkString(engine_name(engine)));
        ParagraphStyle style;
        style.setTextShapingEngine(engine);
        ParagraphBuilderImpl builder(style, fontCollection, get_unicode(style));
        for (const Span& span : kSpans) {
            TextStyle ts;
            ts.setFontFamilies({SkString("CupertinoSystemText")});
            ts.setFontSize(96.0f);
            ts.setColor(span.fColor);
            builder.pushStyle(ts);
            builder.addText(span.fText);
            builder.pop();
        }
        auto paragraph = builder.Build();
        paragraph->layout(2000.0f);
        REPORTER_ASSERT(reporter,
                        static_cast<ParagraphImpl*>(paragraph.get())->isCoreTextShaped() ==
                                (engine == TextShapingEngine::kCoreText));

        SkBitmap bitmap;
        bitmap.allocN32Pixels(400, 200);
        bitmap.eraseColor(SK_ColorWHITE);
        SkCanvas canvas(bitmap);
        paragraph->paint(&canvas, 10, 10);
        int greenPixels = 0;
        for (int y = 0; y < bitmap.height(); ++y) {
            for (int x = 0; x < bitmap.width(); ++x) {
                greenPixels += bitmap.getColor(x, y) == kGreen;
            }
        }
        if (engine != TextShapingEngine::kCoreText) {
            // HarfBuzz is printed, not asserted: other changes may fix it
            INFOF(reporter, "HarfBuzz: %d tone mark pixels\n", greenPixels);
            continue;
        }
        REPORTER_ASSERT(reporter, greenPixels > 0, "got %d tone mark pixels", greenPixels);
    }
}

namespace {

// Captures solid decoration rectangles in paragraph coordinates during Paragraph::paint
class StrikeRectPainter final : public ParagraphPainter {
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

// flutter#165642 (DC1): .SFNS-RegularItalic stores unscaled 1000-UPEM OS/2 strikeout values
// (288/50) in a 2048-UPEM font, while native Apple TextKit draws strikethrough at CTFont
// xHeight / 2 with thickness 0.044 * (ascent + descent + leading), keeping pure and mixed italic
// aligned
DEF_TEST(SkParagraph_Flutter_165642_SystemItalicStrikethrough, reporter) {
    if (!has_font_families({".AppleSystemUIFont"})) {
        return;
    }
    auto fontCollection = test_font_collection();
    constexpr SkScalar kFontSize = 40.0f;

    auto makeStrikeStyle = [&](int weight, SkFontStyle::Slant slant) {
        TextStyle ts;
        ts.setFontFamilies({SkString(".AppleSystemUIFont")});
        ts.setFontSize(kFontSize);
        ts.setFontStyle(SkFontStyle(weight, SkFontStyle::kNormal_Width, slant));
        ts.setDecoration(TextDecoration::kLineThrough);
        ts.setDecorationStyle(TextDecorationStyle::kSolid);
        ts.setDecorationColor(SK_ColorBLACK);
        return ts;
    };

    for (int weight : {100, 400, 700, 900}) {
        for (TextShapingEngine engine : kEngines) {
            skiatest::ReporterContext context(
                    reporter, SkStringPrintf("%s weight %d", engine_name(engine), weight));
            const TextStyle uprightStyle = makeStrikeStyle(weight, SkFontStyle::kUpright_Slant);
            const TextStyle italicStyle = makeStrikeStyle(weight, SkFontStyle::kItalic_Slant);

            auto upright =
                    layout_styled(reporter, fontCollection, engine, uprightStyle, "Plain", 1000.0f);
            auto italic =
                    layout_styled(reporter, fontCollection, engine, italicStyle, "Italic", 1000.0f);

            ParagraphStyle paragraphStyle;
            paragraphStyle.setTextShapingEngine(engine);
            ParagraphBuilderImpl mixedBuilder(
                    paragraphStyle, fontCollection, get_unicode(paragraphStyle));
            mixedBuilder.pushStyle(uprightStyle);
            mixedBuilder.addText("Plain ");
            mixedBuilder.pop();
            mixedBuilder.pushStyle(italicStyle);
            mixedBuilder.addText("Italic");
            mixedBuilder.pop();
            mixedBuilder.pushStyle(uprightStyle);
            mixedBuilder.addText(" End");
            mixedBuilder.pop();
            auto mixed = mixedBuilder.Build();
            mixed->layout(1000.0f);
            REPORTER_ASSERT(reporter,
                            static_cast<ParagraphImpl*>(mixed.get())->isCoreTextShaped() ==
                                    (engine == TextShapingEngine::kCoreText));

            StrikeRectPainter uprightPaint, italicPaint, mixedPaint;
            upright->paint(&uprightPaint, 0, 0);
            italic->paint(&italicPaint, 0, 0);
            mixed->paint(&mixedPaint, 0, 0);

            if (engine != TextShapingEngine::kCoreText) {
                // HarfBuzz is printed, not asserted: SkScalerContext_mac_ct reads raw OS/2 fields
                if (!uprightPaint.fFilledRects.empty() && !italicPaint.fFilledRects.empty()) {
                    INFOF(reporter,
                          "HarfBuzz w=%d: upright y=%g h=%g, italic y=%g h=%g\n",
                          weight,
                          uprightPaint.fFilledRects[0].centerY(),
                          uprightPaint.fFilledRects[0].height(),
                          italicPaint.fFilledRects[0].centerY(),
                          italicPaint.fFilledRects[0].height());
                }
                continue;
            }

            REPORTER_ASSERT(reporter,
                            uprightPaint.fFilledRects.size() == 1,
                            "got %zu upright rects",
                            uprightPaint.fFilledRects.size());
            REPORTER_ASSERT(reporter,
                            italicPaint.fFilledRects.size() == 1,
                            "got %zu italic rects",
                            italicPaint.fFilledRects.size());
            REPORTER_ASSERT(reporter,
                            mixedPaint.fFilledRects.size() == 3,
                            "got %zu mixed rects",
                            mixedPaint.fFilledRects.size());
            if (uprightPaint.fFilledRects.size() != 1 || italicPaint.fFilledRects.size() != 1 ||
                mixedPaint.fFilledRects.size() != 3) {
                continue;
            }

            for (const auto& [label, para, rect] :
                 {std::tuple{"upright", upright.get(), uprightPaint.fFilledRects[0]},
                  std::tuple{"italic", italic.get(), italicPaint.fFilledRects[0]}}) {
                LineMetrics lm;
                REPORTER_ASSERT(reporter, para->getLineMetricsAt(0, &lm));
                const SkFont& font = para->getFontAt(0);
                CTFontRef ct = SkTypeface_GetCTFontRef(font.getTypeface());
                REPORTER_ASSERT(reporter, ct != nullptr);
                if (!ct) {
                    continue;
                }
                // SF metrics do not scale linearly with opsz, so the oracle is only native when
                // the run's CTFont is bound at the drawn point size
                REPORTER_ASSERT(reporter,
                                CTFontGetSize(ct) == font.getSize(),
                                "%s: CTFont size %g, font size %g",
                                label,
                                CTFontGetSize(ct),
                                font.getSize());
                const SkScalar scale = font.getSize() / static_cast<SkScalar>(CTFontGetSize(ct));
                const SkScalar wantY = static_cast<SkScalar>(lm.fBaseline) -
                                       static_cast<SkScalar>(CTFontGetXHeight(ct)) * 0.5f * scale;
                const SkScalar wantH =
                        0.044f *
                        static_cast<SkScalar>(CTFontGetAscent(ct) + CTFontGetDescent(ct) +
                                              CTFontGetLeading(ct)) *
                        scale;
                REPORTER_ASSERT(reporter,
                                SkScalarNearlyEqual(rect.centerY(), wantY, 0.01f) &&
                                        SkScalarNearlyEqual(rect.height(), wantH, 0.01f),
                                "%s: got y %g h %g, want y %g h %g",
                                label,
                                rect.centerY(),
                                rect.height(),
                                wantY,
                                wantH);
            }

            const SkRect& uRect = uprightPaint.fFilledRects[0];
            const SkRect& iRect = italicPaint.fFilledRects[0];
            REPORTER_ASSERT(reporter,
                            SkScalarNearlyEqual(iRect.centerY(), uRect.centerY(), 0.01f) &&
                                    SkScalarNearlyEqual(iRect.height(), uRect.height(), 0.01f),
                            "italic y %g h %g != upright y %g h %g",
                            iRect.centerY(),
                            iRect.height(),
                            uRect.centerY(),
                            uRect.height());

            for (size_t i = 0; i < mixedPaint.fFilledRects.size(); ++i) {
                const SkRect& r = mixedPaint.fFilledRects[i];
                REPORTER_ASSERT(reporter,
                                SkScalarNearlyEqual(r.centerY(), uRect.centerY(), 0.01f) &&
                                        SkScalarNearlyEqual(r.height(), uRect.height(), 0.01f),
                                "mixed[%zu]: got y %g h %g, want y %g h %g",
                                i,
                                r.centerY(),
                                r.height(),
                                uRect.centerY(),
                                uRect.height());
                if (i > 0) {
                    REPORTER_ASSERT(reporter,
                                    SkScalarNearlyEqual(
                                            mixedPaint.fFilledRects[i - 1].fRight, r.fLeft, 0.01f),
                                    "mixed[%zu] gap: prev right %g vs left %g",
                                    i,
                                    mixedPaint.fFilledRects[i - 1].fRight,
                                    r.fLeft);
                }
            }
        }
    }
}
