/*
 * Copyright 2026 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkFontArguments.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkFontStyle.h"
#include "include/core/SkRefCnt.h"
#include "include/core/SkScalar.h"
#include "include/core/SkString.h"
#include "include/core/SkTypeface.h"
#include "include/core/SkTypes.h"
#include "modules/skparagraph/include/FontCollection.h"
#include "modules/skparagraph/include/Paragraph.h"
#include "modules/skparagraph/include/ParagraphStyle.h"
#include "modules/skparagraph/include/TextStyle.h"
#include "modules/skparagraph/src/ParagraphBuilderImpl.h"
#include "modules/skparagraph/src/ParagraphImpl.h"
#include "modules/skparagraph/src/Run.h"
#include "modules/skunicode/include/SkUnicode.h"
#if defined(SK_UNICODE_APPLE_IMPLEMENTATION)
#include "modules/skunicode/include/SkUnicode_apple.h"
#endif
#if defined(SK_UNICODE_ICU_IMPLEMENTATION)
#include "modules/skunicode/include/SkUnicode_icu.h"
#endif
#include "tests/Test.h"
#include "tools/fonts/FontToolUtils.h"
// Native CTLine widths are the oracle for the CoreText engine
#include "src/utils/mac/SkUniqueCFRef.h"

#include <CoreText/CoreText.h>

#include <algorithm>
#include <initializer_list>
#include <memory>
#include <vector>

using namespace skia::textlayout;

namespace {

// A HarfBuzz-free build aborts on kDefault, so only the CoreText half of each contract runs
constexpr TextShapingEngine kEngines[] = {
#if !defined(SK_PARAGRAPH_DISABLE_HARFBUZZ) && defined(SK_UNICODE_ICU_IMPLEMENTATION)
        TextShapingEngine::kDefault,
#endif
        TextShapingEngine::kCoreText};

// Each engine gets the SkUnicode its clients ship with: Apple's for CoreText, ICU for HarfBuzz.
// A build lacking it yields null, which ParagraphBuilder::Build rejects instead of silently
// laying out with another implementation
sk_sp<SkUnicode> make_unicode(TextShapingEngine engine) {
#if defined(SK_UNICODE_APPLE_IMPLEMENTATION)
    if (engine == TextShapingEngine::kCoreText) {
        return SkUnicodes::Apple::Make();
    }
#endif
#if defined(SK_UNICODE_ICU_IMPLEMENTATION)
    return SkUnicodes::ICU::Make();
#else
    return nullptr;
#endif
}

sk_sp<FontCollection> make_test_font_collection() {
    auto fontCollection = sk_make_sp<FontCollection>();
    fontCollection->setDefaultFontManager(ToolUtils::TestFontMgr());
    return fontCollection;
}

// The bugs are about specific Apple faces; the portable font manager (--nonativeFonts) has
// none of them, so the tests skip instead of measuring substitutes
bool has_font_families(std::initializer_list<const char*> families) {
    for (const char* family : families) {
        if (!ToolUtils::TestFontMgr()->matchFamilyStyle(family, SkFontStyle())) {
            return false;
        }
    }
    return true;
}

std::unique_ptr<Paragraph> layout_text(skiatest::Reporter* reporter,
                                       const sk_sp<FontCollection>& fontCollection,
                                       TextShapingEngine engine,
                                       const char* text,
                                       const char* family,
                                       SkScalar fontSize,
                                       SkScalar letterSpacing,
                                       SkScalar width) {
    ParagraphStyle style;
    style.setTextShapingEngine(engine);
    ParagraphBuilderImpl builder(style, fontCollection, make_unicode(engine));
    TextStyle ts;
    ts.setFontFamilies({SkString(family)});
    ts.setFontSize(fontSize);
    ts.setLetterSpacing(letterSpacing);
    builder.pushStyle(ts);
    builder.addText(text);
    builder.pop();
    auto paragraph = builder.Build();
    paragraph->layout(width);
    REPORTER_ASSERT(reporter,
                    static_cast<ParagraphImpl*>(paragraph.get())->isCoreTextShaped() ==
                            (engine == TextShapingEngine::kCoreText));
    return paragraph;
}

// SkParagraph rounds each advance to 16.16 like HarfBuzz where CoreText sums CGFloat advances.
// Each rounding moves the line by at most 2^-17pt, so lines of up to 32 glyphs stay within
// 1/4096pt; the drift measured on the lines below stays under 5e-5pt
constexpr SkScalar kNativeLineTolerance = 1.0f / 4096;

// A copy of font without its trak. The private attribute only builds the trak-free reference
// that proves a test can tell a line that keeps the trak from one that drops it
SkUniqueCFRef<CTFontRef> untracked_copy(CTFontRef font) {
    int zero = 0;
    SkUniqueCFRef<CFNumberRef> zeroNumber(CFNumberCreate(nullptr, kCFNumberIntType, &zero));
    const void* keys[] = {CFSTR("NSCTFontUnscaledTrackingAttribute")};
    const void* values[] = {zeroNumber.get()};
    SkUniqueCFRef<CFDictionaryRef> attributes(CFDictionaryCreate(nullptr,
                                                                 keys,
                                                                 values,
                                                                 1,
                                                                 &kCFTypeDictionaryKeyCallBacks,
                                                                 &kCFTypeDictionaryValueCallBacks));
    SkUniqueCFRef<CTFontDescriptorRef> descriptor(
            CTFontDescriptorCreateWithAttributes(attributes.get()));
    return SkUniqueCFRef<CTFontRef>(
            CTFontCreateCopyWithAttributes(font, 0, nullptr, descriptor.get()));
}

// Typographic width of text on one native CTLine in font, with spacingAttribute (tracking or
// kerning) set to value over the whole text unless it is null
SkScalar native_line_width(CTFontRef font,
                           const char* text,
                           CFStringRef spacingAttribute,
                           SkScalar value) {
    SkUniqueCFRef<CFStringRef> string(
            CFStringCreateWithCString(nullptr, text, kCFStringEncodingUTF8));
    SkUniqueCFRef<CFMutableAttributedStringRef> attrString(
            CFAttributedStringCreateMutable(nullptr, 0));
    CFAttributedStringReplaceString(attrString.get(), CFRangeMake(0, 0), string.get());
    const CFRange all = CFRangeMake(0, CFStringGetLength(string.get()));
    CFAttributedStringSetAttribute(attrString.get(), all, kCTFontAttributeName, font);
    if (spacingAttribute) {
        const double spacing = value;
        SkUniqueCFRef<CFNumberRef> number(
                CFNumberCreate(nullptr, kCFNumberDoubleType, &spacing));
        CFAttributedStringSetAttribute(attrString.get(), all, spacingAttribute, number.get());
    }
    SkUniqueCFRef<CTLineRef> line(CTLineCreateWithAttributedString(attrString.get()));
    return static_cast<SkScalar>(CTLineGetTypographicBounds(line.get(), nullptr, nullptr, nullptr));
}

}  // namespace

// Flutter 150824, 18915, 22572: .AppleSystemUIFont applies size-dependent optical sizing
// (opsz) and tracking (trak) when shaped via CoreText, whereas SkCTFontCreateExactCopy on the
// HarfBuzz path and non-system fonts (Helvetica) scale linearly with point size
DEF_TEST(SkParagraph_Flutter_150824_18915_SystemFontOpticalSizeAndTracking, reporter) {
    if (!has_font_families({"Helvetica", ".AppleSystemUIFont"})) {
        return;
    }
    auto fontCollection = make_test_font_collection();
    const char* text = "DynamicTrackingTest Hello World";

    auto layoutWidth = [&](TextShapingEngine engine, const char* family, SkScalar fontSize) {
        auto paragraph =
                layout_text(reporter, fontCollection, engine, text, family, fontSize, 0, 2000);
        REPORTER_ASSERT(reporter, paragraph->unresolvedGlyphs() == 0);
        return paragraph->getLongestLine();
    };

    for (TextShapingEngine engine : kEngines) {
        // Negative control: Helvetica has no opsz/trak table, so normalized width is size-invariant
        SkScalar helv11 = layoutWidth(engine, "Helvetica", 11.0f) / 11.0f;
        SkScalar helv48 = layoutWidth(engine, "Helvetica", 48.0f) / 48.0f;
        REPORTER_ASSERT(reporter,
                        SkScalarNearlyEqual(helv11, helv48, 0.01f),
                        "engine %d: Helvetica normalized width drifted (%f vs %f)",
                        static_cast<int>(engine),
                        helv11,
                        helv48);
    }

    const TextShapingEngine ct = TextShapingEngine::kCoreText;
    SkScalar sf11 = layoutWidth(ct, ".AppleSystemUIFont", 11.0f) / 11.0f;
    SkScalar sf14 = layoutWidth(ct, ".AppleSystemUIFont", 14.0f);
    SkScalar sf17 = layoutWidth(ct, ".AppleSystemUIFont", 17.0f) / 17.0f;
    SkScalar sf28 = layoutWidth(ct, ".AppleSystemUIFont", 28.0f);
    SkScalar sf48 = layoutWidth(ct, ".AppleSystemUIFont", 48.0f) / 48.0f;
    SkScalar ratio28to14 = sf28 / sf14;

    // SF Pro opsz + trak tighten 17pt (#22572), 28pt vs 14pt (#18915), and 48pt vs
    // 11pt (#150824)
    REPORTER_ASSERT(reporter,
                    sf11 - sf17 > 0.3f,
                    "CoreText 17pt HIG body tracking should tighten vs 11pt (%f vs %f)",
                    sf11,
                    sf17);
    REPORTER_ASSERT(reporter,
                    ratio28to14 < 1.88f,
                    "CoreText 28pt/14pt display ratio should contract (< 1.88x; got %f)",
                    ratio28to14);
    REPORTER_ASSERT(reporter,
                    sf11 - sf48 > 1.5f,
                    "CoreText .AppleSystemUIFont should tighten at 48pt (11pt=%f, 48pt=%f)",
                    sf11,
                    sf48);

    // Flutter 150824 at its own sizes and texts: with no letterSpacing both aliases resolve to the
    // system font and draw as wide as its native CTLine, which SwiftUI's .system(size:) also uses
    struct IssueSizes {
        const char* family;
        const char* label;
        std::initializer_list<int> sizes;
    };
    const IssueSizes issueSizes[] = {
            {"CupertinoSystemText",
             "CupertinoSystemText",
             {3, 4, 5, 7, 8, 9, 10, 11, 13, 15, 17, 19}},
            {"CupertinoSystemDisplay", "SystemDisplay", {20, 22, 25, 28, 32, 36}},
    };
    for (const IssueSizes& issue : issueSizes) {
        for (int size : issue.sizes) {
            const SkString line = SkStringPrintf("%s size %d", issue.label, size);
            const SkScalar fontSize = static_cast<SkScalar>(size);
            SkUniqueCFRef<CTFontRef> system(
                    CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, fontSize, nullptr));
            // Without trak at this size a line that drops it would match the native line as well
            SkUniqueCFRef<CTFontRef> untracked = untracked_copy(system.get());
            const SkScalar native = native_line_width(system.get(), line.c_str(), nullptr, 0);
            const SkScalar nativeUntracked =
                    native_line_width(untracked.get(), line.c_str(), nullptr, 0);
            REPORTER_ASSERT(reporter,
                            !SkScalarNearlyEqual(nativeUntracked, native, kNativeLineTolerance),
                            "'%s': system font has no trak, %f with and without",
                            line.c_str(),
                            native);
            auto paragraph = layout_text(
                    reporter, fontCollection, ct, line.c_str(), issue.family, fontSize, 0, 2000.0f);
            auto* impl = static_cast<ParagraphImpl*>(paragraph.get());
            REPORTER_ASSERT(reporter, impl->lines().size() == 1 && impl->runs().size() == 1);
            if (impl->lines().size() != 1 || impl->runs().size() != 1) {
                continue;
            }
            SkUniqueCFRef<CFStringRef> systemPsName(CTFontCopyPostScriptName(system.get()));
            char systemName[128] = {};
            CFStringGetCString(
                    systemPsName.get(), systemName, sizeof(systemName), kCFStringEncodingUTF8);
            SkString face;
            impl->runs()[0].font().getTypeface()->getPostScriptName(&face);
            REPORTER_ASSERT(reporter,
                            face.equals(systemName),
                            "'%s': %s resolves to %s, system font %s",
                            line.c_str(),
                            issue.family,
                            face.c_str(),
                            systemName);
            REPORTER_ASSERT(
                    reporter,
                    SkScalarNearlyEqual(impl->lines()[0].width(), native, kNativeLineTolerance),
                    "'%s': width %f, native %f",
                    line.c_str(),
                    impl->lines()[0].width(),
                    native);
        }
    }

    // CupertinoTextThemeData's explicit HIG letterSpacing (-0.41 at 17pt, +0.36 at 28pt) stacks
    // on .SFNS's built-in trak, so the line is as wide as the native CTLine of the system font
    // with kCTTrackingAttributeName or kCTKernAttributeName (NSTrackingAttributeName,
    // NSKernAttributeName) set to the letterSpacing
    struct Cupertino {
        const char* family;
        SkScalar size;
        SkScalar letterSpacing;
    };
    for (const Cupertino& c : {Cupertino{"CupertinoSystemText", 17.0f, -0.41f},
                               Cupertino{"CupertinoSystemDisplay", 28.0f, 0.36f}}) {
        SkUniqueCFRef<CTFontRef> system(
                CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, c.size, nullptr));
        // Without trak at this size a line that drops it would match the native line as well
        SkUniqueCFRef<CTFontRef> untracked = untracked_copy(system.get());
        const SkScalar trackedWidth = native_line_width(system.get(), text, nullptr, 0);
        REPORTER_ASSERT(reporter,
                        !SkScalarNearlyEqual(native_line_width(untracked.get(), text, nullptr, 0),
                                             trackedWidth,
                                             kNativeLineTolerance),
                        "%gpt: .SFNS has no trak, %f with and without",
                        c.size,
                        trackedWidth);
        auto paragraph = layout_text(
                reporter, fontCollection, ct, text, c.family, c.size, c.letterSpacing, 2000.0f);
        auto* impl = static_cast<ParagraphImpl*>(paragraph.get());
        REPORTER_ASSERT(reporter, impl->lines().size() == 1);
        if (impl->lines().size() != 1) {
            continue;
        }
        for (CFStringRef attribute : {kCTTrackingAttributeName, kCTKernAttributeName}) {
            const SkScalar native =
                    native_line_width(system.get(), text, attribute, c.letterSpacing);
            REPORTER_ASSERT(
                    reporter,
                    SkScalarNearlyEqual(impl->lines()[0].width(), native, kNativeLineTolerance),
                    "%gpt explicit %+g letterSpacing: %f, native %s %f",
                    c.size,
                    c.letterSpacing,
                    impl->lines()[0].width(),
                    attribute == kCTKernAttributeName ? "kerning" : "tracking",
                    native);
        }
    }

#if !defined(SK_PARAGRAPH_DISABLE_HARFBUZZ) && defined(SK_UNICODE_ICU_IMPLEMENTATION)
    // HarfBuzz is printed, not asserted: other changes may fix it
    const TextShapingEngine hb = TextShapingEngine::kDefault;
    auto hbWidth = [&](const char* family, SkScalar fontSize, SkScalar letterSpacing) {
        return layout_text(reporter, fontCollection, hb, text, family, fontSize, letterSpacing,
                           2000.0f)->getLongestLine();
    };
    SkScalar hb11 = hbWidth(".AppleSystemUIFont", 11.0f, 0) / 11.0f;
    SkScalar hb14 = hbWidth(".AppleSystemUIFont", 14.0f, 0);
    SkScalar hb17 = hbWidth(".AppleSystemUIFont", 17.0f, 0);
    SkScalar hb28 = hbWidth(".AppleSystemUIFont", 28.0f, 0);
    SkScalar hb48 = hbWidth(".AppleSystemUIFont", 48.0f, 0) / 48.0f;
    INFOF(reporter,
          "HarfBuzz: 11-17pt %f, 28/14pt %f, 11-48pt %f, |17pt-Cupertino| %f, |28pt-Cupertino| "
          "%f\n",
          hb11 - hb17 / 17.0f,
          hb28 / hb14,
          hb11 - hb48,
          SkScalarAbs(hbWidth("CupertinoSystemText", 17.0f, -0.41f) - hb17),
          SkScalarAbs(hbWidth("CupertinoSystemDisplay", 28.0f, 0.36f) - hb28));
#endif
}

// No tracked issue (related: Flutter 36062): on keycap emoji sequences ("1️⃣2️⃣3️⃣"), CoreText
// collapses each keycap into 1 glyph (3 total, +48px under letterSpacing=16); HarfBuzz was
// measured to leave a phantom zero-advance glyph per U+FE0F selector, which
// Run::addLetterSpacesEvenly spaces as well
DEF_TEST(SkParagraph_KeycapEmojiClusterLetterSpacing, reporter) {
    if (!has_font_families({".AppleSystemUIFont", "Apple Color Emoji"})) {
        return;
    }
    auto fontCollection = make_test_font_collection();
    fontCollection->enableFontFallback();
    const char* text =
            "1\xEF\xB8\x8F\xE2\x83\xA3"
            "2\xEF\xB8\x8F\xE2\x83\xA3"
            "3\xEF\xB8\x8F\xE2\x83\xA3";

    for (TextShapingEngine engine : kEngines) {
        const bool coreText = engine == TextShapingEngine::kCoreText;
        auto p0 = layout_text(
                reporter, fontCollection, engine, text, ".AppleSystemUIFont", 28.0f, 0.0f, 500.0f);
        auto p16 = layout_text(
                reporter, fontCollection, engine, text, ".AppleSystemUIFont", 28.0f, 16.0f, 500.0f);

        auto impl0 = static_cast<ParagraphImpl*>(p0.get());
        auto impl16 = static_cast<ParagraphImpl*>(p16.get());
        REPORTER_ASSERT(reporter, p0->unresolvedGlyphs() == 0 && p16->unresolvedGlyphs() == 0);
        REPORTER_ASSERT(reporter, impl0->runs().size() == 1 && impl16->runs().size() == 1);
        if (impl0->runs().size() != 1 || impl16->runs().size() != 1) {
            continue;
        }

        const Run& r0 = impl0->runs()[0];
        const Run& r16 = impl16->runs()[0];
        SkString emojiFamily;
        r0.font().getTypeface()->getFamilyName(&emojiFamily);
        REPORTER_ASSERT(reporter,
                        emojiFamily.contains("Emoji"),
                        "engine %d: expected Emoji family, got %s",
                        static_cast<int>(engine),
                        emojiFamily.c_str());

        const auto& clusters = impl0->clusters();
        REPORTER_ASSERT(reporter, clusters.size() == 4);
        if (clusters.size() == 4) {
            REPORTER_ASSERT(reporter, clusters[0].textRange() == TextRange(0, 7));
            REPORTER_ASSERT(reporter, clusters[1].textRange() == TextRange(7, 14));
            REPORTER_ASSERT(reporter, clusters[2].textRange() == TextRange(14, 21));
        }

        // Explicit letterSpacing stacks on the emoji font's own trak, so the spacing alone
        // separates a spaced and an unspaced layout
        SkScalar widthDelta = p16->getLongestLine() - p0->getLongestLine();
        if (coreText) {
            REPORTER_ASSERT(reporter, r0.size() == 3 && r16.size() == 3);
            REPORTER_ASSERT(reporter,
                            SkScalarNearlyEqual(widthDelta, 48.0f, 0.1f),
                            "CoreText expected 3 keycaps * 16px = 48px delta; got %f",
                            widthDelta);
        } else {
            // HarfBuzz is printed, not asserted: other changes may fix it
            INFOF(reporter, "HarfBuzz: %zu glyphs, 0->16px delta %f\n", r16.size(), widthDelta);
        }
    }
}

// Flutter 36062: keycap sequences render as digit + box when the primary font maps both the
// digit and U+20E3 (Lucida Grande does). CoreText sends each keycap to Apple Color Emoji as one
// glyph; HarfBuzz was measured to keep the digit and U+20E3 in the primary font
DEF_TEST(SkParagraph_Flutter_36062_KeycapPrimaryFontMapsU20E3, reporter) {
    if (!has_font_families({"Lucida Grande", "Apple Color Emoji"})) {
        return;
    }
    auto fontCollection = make_test_font_collection();
    fontCollection->enableFontFallback();
    const char* text =
            "#\xEF\xB8\x8F\xE2\x83\xA3 *\xEF\xB8\x8F\xE2\x83\xA3 0\xEF\xB8\x8F\xE2\x83\xA3 "
            "1\xEF\xB8\x8F\xE2\x83\xA3 9\xEF\xB8\x8F\xE2\x83\xA3";
    constexpr int kKeycaps = 5;

    for (TextShapingEngine engine : kEngines) {
        auto paragraph = layout_text(
                reporter, fontCollection, engine, text, "Lucida Grande", 20.0f, 0, 2000);
        int emojiGlyphs = 0;
        int otherKeycapGlyphs = 0;
        for (const Run& run : static_cast<ParagraphImpl*>(paragraph.get())->runs()) {
            SkString family;
            run.font().getTypeface()->getFamilyName(&family);
            const bool emoji = family.contains("Emoji");
            for (size_t i = 0; i < run.size(); ++i) {
                // Spaces may stay in the primary font; every other glyph belongs to a keycap
                if (text[run.globalClusterIndex(i)] == ' ') {
                    continue;
                }
                ++(emoji ? emojiGlyphs : otherKeycapGlyphs);
            }
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
                        "CoreText left %d keycap glyphs outside Apple Color Emoji",
                        otherKeycapGlyphs);
        REPORTER_ASSERT(reporter,
                        emojiGlyphs == kKeycaps,
                        "CoreText expected one emoji glyph per keycap (%d); got %d",
                        kKeycaps,
                        emojiGlyphs);
    }
}

// No tracked issue: on ZWJ emoji sequences containing U+FE0F variation selectors
// ("🏳️‍🌈❤️‍🔥👩‍💻"), CoreText emits 3 fused glyphs (+48px under letterSpacing=16); HarfBuzz was
// measured to emit 5 glyphs (phantom U+FE0F glyphs in 🏳️‍🌈 and ❤️‍🔥) that all receive spacing
DEF_TEST(SkParagraph_ZWJEmojiVariationSelectorLetterSpacing, reporter) {
    if (!has_font_families({".AppleSystemUIFont", "Apple Color Emoji"})) {
        return;
    }
    auto fontCollection = make_test_font_collection();
    fontCollection->enableFontFallback();
    const char* zwjText =
            "\xF0\x9F\x8F\xB3\xEF\xB8\x8F\xE2\x80\x8D\xF0\x9F\x8C\x88"
            "\xE2\x9D\xA4\xEF\xB8\x8F\xE2\x80\x8D\xF0\x9F\x94\xA5"
            "\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x92\xBB";

    for (TextShapingEngine engine : kEngines) {
        auto p0 = layout_text(reporter,
                              fontCollection,
                              engine,
                              zwjText,
                              ".AppleSystemUIFont",
                              28.0f,
                              0.0f,
                              500.0f);
        auto p16 = layout_text(reporter,
                               fontCollection,
                               engine,
                               zwjText,
                               ".AppleSystemUIFont",
                               28.0f,
                               16.0f,
                               500.0f);
        REPORTER_ASSERT(reporter, p0->unresolvedGlyphs() == 0 && p16->unresolvedGlyphs() == 0);

        auto impl0 = static_cast<ParagraphImpl*>(p0.get());
        auto impl16 = static_cast<ParagraphImpl*>(p16.get());
        REPORTER_ASSERT(reporter, impl0->runs().size() == 1 && impl16->runs().size() == 1);
        if (impl0->runs().size() != 1 || impl16->runs().size() != 1 ||
            engine != TextShapingEngine::kCoreText) {
            continue;
        }
        SkScalar widthDelta = p16->getLongestLine() - p0->getLongestLine();
        REPORTER_ASSERT(reporter, impl0->runs()[0].size() == 3 && impl16->runs()[0].size() == 3);
        REPORTER_ASSERT(reporter,
                        SkScalarNearlyEqual(widthDelta, 48.0f, 0.1f),
                        "CoreText expected 3 ZWJ emoji * 16px = 48px delta; got %f",
                        widthDelta);
    }
}

// Flutter 118127: on decomposed "ax\u0301b" (which has no precomposed NFC form), CoreText's
// tracking keeps the combining acute anchored to 'x' under letterSpacing=10 and expands width by
// 3 clusters (30px); HarfBuzz was measured to shift the combining mark away from its base too
DEF_TEST(SkParagraph_Flutter_118127_CombiningDiacriticsLetterSpacing, reporter) {
    if (!has_font_families({"Helvetica"})) {
        return;
    }
    auto fontCollection = make_test_font_collection();
    const char* text = "ax\xCC\x81" "b";

    auto visualX = [](const Run& run, size_t idx) -> SkScalar {
        // Run::offsets() is empty when every glyph offset is zero
        return run.positions()[idx].fX + (run.offsets().empty() ? 0 : run.offsets()[idx].fX);
    };

    for (TextShapingEngine engine : kEngines) {
        auto p0 = layout_text(
                reporter, fontCollection, engine, text, "Helvetica", 20.0f, 0.0f, 500.0f);
        auto p10 = layout_text(
                reporter, fontCollection, engine, text, "Helvetica", 20.0f, 10.0f, 500.0f);

        auto impl0 = static_cast<ParagraphImpl*>(p0.get());
        auto impl10 = static_cast<ParagraphImpl*>(p10.get());
        REPORTER_ASSERT(reporter, impl0->runs().size() == 1 && impl10->runs().size() == 1);
        if (impl0->runs().size() != 1 || impl10->runs().size() != 1) {
            continue;
        }

        const Run& r0 = impl0->runs()[0];
        const Run& r10 = impl10->runs()[0];
        REPORTER_ASSERT(reporter, r0.size() == 4 && r10.size() == 4);
        if (r0.size() != 4 || r10.size() != 4) {
            continue;
        }

        SkScalar markDelta0 = visualX(r0, 2) - visualX(r0, 1);
        SkScalar markDelta10 = visualX(r10, 2) - visualX(r10, 1);
        SkScalar widthDelta = p10->getLongestLine() - p0->getLongestLine();
        if (engine != TextShapingEngine::kCoreText) {
            // HarfBuzz is printed, not asserted: other changes may fix it
            INFOF(reporter, "HarfBuzz: mark offset ls0=%f ls10=%f, 0->10px delta %f\n",
                  markDelta0, markDelta10, widthDelta);
            continue;
        }
        REPORTER_ASSERT(reporter,
                        SkScalarNearlyEqual(markDelta10, markDelta0, 0.05f),
                        "CoreText mark detached under letterSpacing: ls0=%f, ls10=%f",
                        markDelta0,
                        markDelta10);
        REPORTER_ASSERT(reporter,
                        SkScalarNearlyEqual(widthDelta, 30.0f, 0.05f),
                        "CoreText widthDelta should be 3 clusters * 10px = 30px; got %f",
                        widthDelta);
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
    auto fontCollection = make_test_font_collection();
    const char* hindi = "\xE0\xA4\xB9\xE0\xA4\xBF\xE0\xA4\xA8\xE0\xA5\x8D\xE0\xA4\xA6\xE0\xA5\x80";

    for (TextShapingEngine engine : kEngines) {
        auto p0 = layout_text(reporter,
                              fontCollection,
                              engine,
                              hindi,
                              "Kohinoor Devanagari",
                              24.0f,
                              0.0f,
                              500.0f);
        auto p16 = layout_text(reporter,
                               fontCollection,
                               engine,
                               hindi,
                               "Kohinoor Devanagari",
                               24.0f,
                               16.0f,
                               500.0f);

        auto impl0 = static_cast<ParagraphImpl*>(p0.get());
        auto impl16 = static_cast<ParagraphImpl*>(p16.get());
        REPORTER_ASSERT(reporter, p0->unresolvedGlyphs() == 0 && p16->unresolvedGlyphs() == 0);
        REPORTER_ASSERT(reporter, impl0->runs().size() == 1 && impl16->runs().size() == 1);
        if (impl0->runs().size() != 1 || impl16->runs().size() != 1) {
            continue;
        }

        const Run& r0 = impl0->runs()[0];
        const Run& r16 = impl16->runs()[0];
        REPORTER_ASSERT(reporter, r0.size() == 4 && r16.size() == 4);
        if (r0.size() != 4 || r16.size() != 4) {
            continue;
        }

        SkScalar syl1Span0 = r0.positions()[1].fX - r0.positions()[0].fX;
        SkScalar syl1Span16 = r16.positions()[1].fX - r16.positions()[0].fX;
        SkScalar widthDelta = p16->getLongestLine() - p0->getLongestLine();
        if (engine != TextShapingEngine::kCoreText) {
            // HarfBuzz is printed, not asserted: other changes may fix it
            INFOF(reporter, "HarfBuzz: syllable 1 span ls0=%f ls16=%f, 0->16px delta %f\n",
                  syl1Span0, syl1Span16, widthDelta);
            continue;
        }
        REPORTER_ASSERT(reporter,
                        SkScalarNearlyEqual(syl1Span16, syl1Span0, 0.05f),
                        "CoreText matra detached inside syllable 1: ls0=%f, ls16=%f",
                        syl1Span0,
                        syl1Span16);
        REPORTER_ASSERT(reporter,
                        SkScalarNearlyEqual(widthDelta, 32.0f, 0.1f),
                        "CoreText expected 2 syllables * 16px = 32px delta; got %f",
                        widthDelta);
    }
}

// Flutter 147708: when CupertinoSystemText or CupertinoSystemDisplay is paired with a non-empty
// fontFamilyFallback list, CoreText resolves both aliases to .SFNS before consulting the fallback
// list and preserves the 28pt/14pt display optical-size contraction; HarfBuzz misses both aliases
// in matchFamilyStyle and falls through to the first fallback family (Helvetica)
DEF_TEST(SkParagraph_Flutter_147708_CupertinoAliasesBypassFallback, reporter) {
    if (!has_font_families({".AppleSystemUIFont", "Helvetica"})) {
        return;
    }
    auto fontCollection = make_test_font_collection();
    fontCollection->enableFontFallback();
    const char* text = "DynamicTrackingTest Hello World";

    auto layoutWithFallback = [&](TextShapingEngine engine, const char* alias, SkScalar fontSize) {
        ParagraphStyle style;
        style.setTextShapingEngine(engine);
        ParagraphBuilderImpl builder(style, fontCollection, make_unicode(engine));
        TextStyle ts;
        ts.setFontFamilies({SkString(alias), SkString("Helvetica")});
        ts.setFontSize(fontSize);
        builder.pushStyle(ts);
        builder.addText(text);
        builder.pop();
        auto paragraph = builder.Build();
        paragraph->layout(2000.0f);
        REPORTER_ASSERT(reporter,
                        static_cast<ParagraphImpl*>(paragraph.get())->isCoreTextShaped() ==
                                (engine == TextShapingEngine::kCoreText));
        return paragraph;
    };

    for (const char* alias : {"CupertinoSystemText", "CupertinoSystemDisplay"}) {
        for (TextShapingEngine engine : kEngines) {
            auto p14 = layoutWithFallback(engine, alias, 14.0f);
            auto p28 = layoutWithFallback(engine, alias, 28.0f);
            auto impl28 = static_cast<ParagraphImpl*>(p28.get());
            REPORTER_ASSERT(reporter, impl28->runs().size() == 1);
            if (impl28->runs().size() != 1) {
                continue;
            }
            SkString psName;
            impl28->runs()[0].font().getTypeface()->getPostScriptName(&psName);
            SkScalar ratio = p28->getLongestLine() / p14->getLongestLine();
            if (engine != TextShapingEngine::kCoreText) {
                // HarfBuzz is printed, not asserted: other changes may fix it
                INFOF(reporter, "HarfBuzz %s: %s, 28/14pt %f\n", alias, psName.c_str(), ratio);
                continue;
            }
            REPORTER_ASSERT(reporter,
                            psName.startsWith(".AppleSystemUIFont") || psName.startsWith(".SF"),
                            "CoreText %s fell through to %s",
                            alias,
                            psName.c_str());
            REPORTER_ASSERT(reporter,
                            ratio < 1.88f,
                            "CoreText %s 28pt/14pt ratio should contract (< 1.88x; got %f)",
                            alias,
                            ratio);
        }
    }
}

// Flutter 193812: .AppleSystemUIFontRounded, .AppleSystemUIFontSerif, and
// .AppleSystemUIFontMonospaced resolve to Apple's Rounded, NewYork, and SF Mono UI designs on
// CoreText at the layout point size (applying 'trak' on Rounded and Serif and setting .NewYork
// 'opsz' to the requested size), whereas HarfBuzz instantiates CTFonts at size 0 without 'trak'
DEF_TEST(SkParagraph_Flutter_193812_SystemDesigns, reporter) {
    if (!has_font_families({".AppleSystemUIFont", "Helvetica"})) {
        return;
    }
    auto fontCollection = make_test_font_collection();
    fontCollection->enableFontFallback();
    const char* text = "System UI Font Design Variants";

    // Rounded and Serif apply size-dependent tracking ('trak') at 24pt, whereas Monospaced has
    // fixed advances at both size 0 and 24pt
    constexpr struct {
        const char* fFamily;
        const char* fExpectedPs;
        bool fHasSizeTracking;
    } kDesigns[] = {
            {".AppleSystemUIFontRounded", "Rounded", true},
            {".AppleSystemUIFontSerif", "NewYork", true},
            {".AppleSystemUIFontMonospaced", "Monospaced", false},
    };

    // Compare CoreText at 24pt against HarfBuzz (which instantiates CTFonts at size 0 without
    // tracking)
    for (const auto& d : kDesigns) {
        auto pCt = layout_text(reporter,
                               fontCollection,
                               TextShapingEngine::kCoreText,
                               text,
                               d.fFamily,
                               24.0f,
                               0.0f,
                               1000.0f);
        auto implCt = static_cast<ParagraphImpl*>(pCt.get());
        REPORTER_ASSERT(reporter, implCt->runs().size() == 1);
        if (implCt->runs().size() == 1) {
            SkString psName;
            implCt->runs()[0].font().getTypeface()->getPostScriptName(&psName);
            REPORTER_ASSERT(reporter,
                            psName.contains(d.fExpectedPs),
                            "CoreText %s expected %s in PostScript name, got %s",
                            d.fFamily,
                            d.fExpectedPs,
                            psName.c_str());
        }
#if !defined(SK_PARAGRAPH_DISABLE_HARFBUZZ) && defined(SK_UNICODE_ICU_IMPLEMENTATION)
        // A HarfBuzz-free build aborts on kDefault, so only the PostScript name check runs there
        auto pHb = layout_text(reporter,
                               fontCollection,
                               TextShapingEngine::kDefault,
                               text,
                               d.fFamily,
                               24.0f,
                               0.0f,
                               1000.0f);
        SkString hbPsName;
        static_cast<ParagraphImpl*>(pHb.get())->runs()[0].font().getTypeface()->getPostScriptName(
                &hbPsName);
        // HarfBuzz is printed, not asserted: other changes may fix it
        INFOF(reporter, "HarfBuzz %s: %s, width %f\n", d.fFamily, hbPsName.c_str(),
              pHb->getLongestLine());
        // Size-instantiated CTFonts apply 'trak' (and .NewYork 'opsz') that narrow the line
        // compared to size-0 HarfBuzz CTFonts
        if (d.fHasSizeTracking) {
            REPORTER_ASSERT(reporter,
                            pCt->getLongestLine() + 5.0f < pHb->getLongestLine(),
                            "CoreText %s at 24pt (%f) should be narrower than size-0 HarfBuzz (%f)",
                            d.fFamily,
                            pCt->getLongestLine(),
                            pHb->getLongestLine());
        } else {
            REPORTER_ASSERT(
                    reporter,
                    SkScalarNearlyEqual(pCt->getLongestLine(), pHb->getLongestLine(), 0.05f),
                    "Monospaced %s width should match between CoreText (%f) and HarfBuzz (%f)",
                    d.fFamily,
                    pCt->getLongestLine(),
                    pHb->getLongestLine());
        }
#endif
    }

    // Verify narrow and wide strings have identical width in Monospaced
    for (TextShapingEngine engine : kEngines) {
        auto pNarrow = layout_text(reporter,
                                   fontCollection,
                                   engine,
                                   "iiiiiiii",
                                   ".AppleSystemUIFontMonospaced",
                                   24.0f,
                                   0.0f,
                                   1000.0f);
        auto pWide = layout_text(reporter,
                                 fontCollection,
                                 engine,
                                 "WWWWWWWW",
                                 ".AppleSystemUIFontMonospaced",
                                 24.0f,
                                 0.0f,
                                 1000.0f);
        if (engine == TextShapingEngine::kCoreText) {
            REPORTER_ASSERT(reporter,
                            SkScalarNearlyEqual(
                                    pNarrow->getLongestLine(), pWide->getLongestLine(), 0.05f),
                            "CoreText .AppleSystemUIFontMonospaced should have equal width for "
                            "'iiiiiiii' (%f) and 'WWWWWWWW' (%f)",
                            pNarrow->getLongestLine(),
                            pWide->getLongestLine());
        }
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
    auto fontCollection = make_test_font_collection();
    const char* text = "Variable fonts honor axes";
    const SkFourByteTag opszTag = SkSetFourByteTag('o', 'p', 's', 'z');
    const SkScalar sizes[2] = {12.0f, 72.0f};

    // Design coordinate of opsz on the face that renders the run; -1 when the face has no opsz
    auto opszOf = [&](const SkTypeface* typeface) -> SkScalar {
        int count = typeface->getVariationDesignPosition({});
        std::vector<SkFontArguments::VariationPosition::Coordinate> coords(std::max(count, 0));
        typeface->getVariationDesignPosition(coords);
        for (const auto& c : coords) {
            if (c.axis == opszTag) {
                return c.value;
            }
        }
        return -1;
    };

    for (TextShapingEngine engine : kEngines) {
        SkScalar advancePerEm[2] = {0, 0};
        SkScalar opsz[2] = {0, 0};
        bool singleRun = true;
        for (int i = 0; i < 2; ++i) {
            auto p = layout_text(reporter,
                                 fontCollection,
                                 engine,
                                 text,
                                 ".AppleSystemUIFont",
                                 sizes[i],
                                 0.0f,
                                 2000.0f);
            auto impl = static_cast<ParagraphImpl*>(p.get());
            REPORTER_ASSERT(reporter, p->unresolvedGlyphs() == 0 && impl->runs().size() == 1);
            if (impl->runs().size() != 1) {
                singleRun = false;
                break;
            }
            const Run& run = impl->runs()[0];
            // The font's own advances, so CoreText 'trak' does not count as optical sizing
            std::vector<SkScalar> widths(run.size());
            run.font().getWidths(run.glyphs(), widths);
            SkScalar sum = 0;
            for (SkScalar w : widths) {
                sum += w;
            }
            advancePerEm[i] = sum / sizes[i];
            opsz[i] = opszOf(run.font().getTypeface());
        }
        if (!singleRun || engine != TextShapingEngine::kCoreText) {
            continue;
        }
        REPORTER_ASSERT(reporter,
                        opsz[1] > opsz[0],
                        "CoreText opsz should follow the font size (12pt=%f, 72pt=%f)",
                        opsz[0],
                        opsz[1]);
        REPORTER_ASSERT(reporter,
                        advancePerEm[0] > advancePerEm[1] * 1.01f,
                        "CoreText advances per em should shrink at 72pt (12pt=%f, 72pt=%f)",
                        advancePerEm[0],
                        advancePerEm[1]);
    }
}

// Flutter 144099: Arabic text styled with CupertinoSystemText renders its Arabic runs in SF Arabic
// on CoreText, as native iOS text does; HarfBuzz was measured to miss the alias and fall back to
// Geeza Pro for Arabic and Helvetica for the space and colon
DEF_TEST(SkParagraph_Flutter_144099_CupertinoArabicSystemFont, reporter) {
    if (!has_font_families({".AppleSystemUIFont"})) {
        return;
    }
    auto fontCollection = make_test_font_collection();
    fontCollection->enableFontFallback();
    const char* text =
            "\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7 \xD9\xA1\xD9\xA1:\xD9\xA4\xD9\xA0";

    for (TextShapingEngine engine : kEngines) {
        ParagraphStyle style;
        style.setTextShapingEngine(engine);
        ParagraphBuilderImpl builder(style, fontCollection, make_unicode(engine));
        TextStyle ts;
        ts.setFontFamilies({SkString("CupertinoSystemText")});
        ts.setFontSize(20.0f);
        ts.setLocale(SkString("ar"));
        builder.pushStyle(ts);
        builder.addText(text);
        builder.pop();
        auto paragraph = builder.Build();
        paragraph->layout(1000.0f);
        auto impl = static_cast<ParagraphImpl*>(paragraph.get());
        REPORTER_ASSERT(reporter,
                        impl->isCoreTextShaped() == (engine == TextShapingEngine::kCoreText));
        REPORTER_ASSERT(reporter, paragraph->unresolvedGlyphs() == 0 && !impl->runs().empty());
        if (engine != TextShapingEngine::kCoreText) {
            SkString runs;
            for (const Run& run : impl->runs()) {
                SkString family;
                run.font().getTypeface()->getFamilyName(&family);
                runs.appendf(" [%zu, %zu) %s", run.textRange().start, run.textRange().end,
                             family.c_str());
            }
            // HarfBuzz is printed, not asserted: other changes may fix it
            INFOF(reporter, "HarfBuzz runs:%s\n", runs.c_str());
            continue;
        }
        for (const Run& run : impl->runs()) {
            // Space and ':' are common script and may stay in the Latin face; every Arabic letter
            // and Arabic-Indic digit here is a 0xD8/0xD9-led UTF-8 sequence
            const unsigned char lead = text[run.textRange().start];
            if (lead != 0xD8 && lead != 0xD9) {
                continue;
            }
            SkString family;
            run.font().getTypeface()->getFamilyName(&family);
            REPORTER_ASSERT(reporter,
                            family.contains("SF Arabic"),
                            "CoreText Arabic run [%zu, %zu) resolved to %s",
                            run.textRange().start,
                            run.textRange().end,
                            family.c_str());
        }
    }
}

// Flutter 130492: .AppleSystemUIFont and CupertinoSystemText Italic resolve to .SFNS-RegularItalic
// and render with the face's intrinsic italic slant alone (skewX == 0) on CoreText; HarfBuzz was
// measured to stack synthetic skewX = -0.25 on top because CTFontCopyFontDescriptor omits
// kCTFontSlantTrait from .SFNS-RegularItalic's traits dictionary
DEF_TEST(SkParagraph_Flutter_130492_SystemFontItalicSingleSlant, reporter) {
    if (!has_font_families({".AppleSystemUIFont"})) {
        return;
    }
    auto fontCollection = make_test_font_collection();
    const char* text = "The quick brown fox";

    for (const char* family : {".AppleSystemUIFont", "CupertinoSystemText"}) {
        for (TextShapingEngine engine : kEngines) {
            ParagraphStyle style;
            style.setTextShapingEngine(engine);
            ParagraphBuilderImpl builder(style, fontCollection, make_unicode(engine));
            TextStyle ts;
            ts.setFontFamilies({SkString(family)});
            ts.setFontSize(24.0f);
            ts.setFontStyle(SkFontStyle::Italic());
            builder.pushStyle(ts);
            builder.addText(text);
            builder.pop();
            auto paragraph = builder.Build();
            paragraph->layout(1000.0f);
            auto impl = static_cast<ParagraphImpl*>(paragraph.get());
            REPORTER_ASSERT(reporter,
                            impl->isCoreTextShaped() == (engine == TextShapingEngine::kCoreText));
            REPORTER_ASSERT(reporter,
                            paragraph->unresolvedGlyphs() == 0 && impl->runs().size() == 1);
            if (impl->runs().size() != 1) {
                continue;
            }
            const SkFont& font = impl->runs()[0].font();
            if (engine != TextShapingEngine::kCoreText) {
                SkString psName;
                font.getTypeface()->getPostScriptName(&psName);
                // HarfBuzz is printed, not asserted: other changes may fix it
                INFOF(reporter, "HarfBuzz %s Italic: %s, skewX %g\n", family, psName.c_str(),
                      font.getSkewX());
                continue;
            }
            REPORTER_ASSERT(reporter,
                            font.getSkewX() == 0.0f,
                            "CoreText %s Italic double-slanted with synthetic skewX=%g",
                            family,
                            font.getSkewX());
        }
    }
}

// Flutter 141080: in "ไก่" with a color per character, the tone mark U+0E48 must paint in its
// own color. CupertinoSystemText has no Thai, and CoreText falls back to a face that keeps the
// mark a separate glyph; HarfBuzz was measured to fall back to Thonburi, which ligates the base
// and the mark into one glyph that takes the base color
DEF_TEST(SkParagraph_Flutter_141080_CupertinoThaiToneMarkColor, reporter) {
    if (!has_font_families({".AppleSystemUIFont", "Thonburi"})) {
        return;
    }
    auto fontCollection = make_test_font_collection();
    fontCollection->enableFontFallback();
    const SkColor kRed = SkColorSetRGB(0xFF, 0, 0);
    const SkColor kAmber = SkColorSetRGB(0xFF, 0xC1, 0x07);
    const SkColor kGreen = SkColorSetRGB(0, 0xFF, 0);
    struct Span {
        const char* fText;
        SkColor fColor;
    };
    const Span spans[] = {
            {"\xE0\xB9\x84", kRed}, {"\xE0\xB8\x81", kAmber}, {"\xE0\xB9\x88", kGreen}};

    for (TextShapingEngine engine : kEngines) {
        ParagraphStyle style;
        style.setTextShapingEngine(engine);
        ParagraphBuilderImpl builder(style, fontCollection, make_unicode(engine));
        for (const Span& span : spans) {
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
        REPORTER_ASSERT(
                reporter, greenPixels > 0, "CoreText painted the tone mark in another color");
    }
}
