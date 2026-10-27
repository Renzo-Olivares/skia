/*
 * Copyright 2026 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "include/core/SkFontMgr.h"
#include "include/core/SkFontStyle.h"
#include "include/core/SkRefCnt.h"
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

using namespace skia::textlayout;

namespace {

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

}  // namespace

// No tracked issue: when .AppleSystemUIFont encounters unmapped supplementary
// Private Use Area codepoints (U+100000..U+100002) alongside Latin, CJK, and Arabic runs,
// HarfBuzz's matchFamilyStyleCharacter falls back to Apple's LastResort font, whereas CoreText
// suppresses LastResort via isLastResortFont() and preserves primary-font .notdef (glyph 0)
DEF_TEST(SkParagraph_AppleSystemUIFontCascade, reporter) {
    // The portable font manager (--nonativeFonts) has none of the Apple faces the cascade names
    for (const char* family : {".AppleSystemUIFont", "PingFang SC", "Geeza Pro"}) {
        if (!ToolUtils::TestFontMgr()->matchFamilyStyle(family, SkFontStyle())) {
            return;
        }
    }
    auto fontCollection = sk_make_sp<FontCollection>();
    fontCollection->setDefaultFontManager(ToolUtils::TestFontMgr());
    fontCollection->enableFontFallback();

    const char* cascadeText =
            "SF Pro  \xE7\xAE\x80\xE4\xBD\x93  \xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7  "
            "\xF4\x80\x80\x80\xF4\x80\x80\x81\xF4\x80\x80\x82";

    // A HarfBuzz-free build aborts on kDefault, so only the CoreText half of the contract runs
    for (TextShapingEngine engine : {
#if !defined(SK_PARAGRAPH_DISABLE_HARFBUZZ) && defined(SK_UNICODE_ICU_IMPLEMENTATION)
                 TextShapingEngine::kDefault,
#endif
                 TextShapingEngine::kCoreText}) {
        const bool coreText = engine == TextShapingEngine::kCoreText;
        ParagraphStyle style;
        style.setTextShapingEngine(engine);
        ParagraphBuilderImpl builder(style, fontCollection, make_unicode(engine));
        TextStyle ts;
        ts.setFontFamilies({SkString(".AppleSystemUIFont")});
        ts.setFontSize(24.0f);
        builder.pushStyle(ts);
        builder.addText(cascadeText);
        builder.pop();

        auto paragraph = builder.Build();
        paragraph->layout(1000.0f);
        auto impl = static_cast<ParagraphImpl*>(paragraph.get());
        REPORTER_ASSERT(reporter, impl->isCoreTextShaped() == coreText);

        bool hasSystemFontRun = false;
        bool hasChineseRun = false;
        bool hasArabicRun = false;
        bool hasLastResortRun = false;
        for (const auto& run : impl->runs()) {
            SkString ps;
            run.font().getTypeface()->getPostScriptName(&ps);
            if (ps.contains("LastResort")) {
                hasLastResortRun = true;
            }
            if (ps.contains("SF") || ps.contains("AppleSystemUIFont")) {
                hasSystemFontRun = true;
            }
            if (ps.contains("PingFang")) {
                hasChineseRun = true;
            }
            if (ps.contains("Arabic") || ps.contains("Geeza")) {
                hasArabicRun = true;
            }
        }
        REPORTER_ASSERT(reporter, hasSystemFontRun);
        REPORTER_ASSERT(reporter, hasChineseRun);
        REPORTER_ASSERT(reporter, hasArabicRun);

        if (coreText) {
            REPORTER_ASSERT(reporter, !hasLastResortRun,
                            "CoreText must suppress LastResort on unmapped PUA codepoints");
            REPORTER_ASSERT(reporter, paragraph->unresolvedGlyphs() == 3,
                            "CoreText expected 3 unresolved PUA .notdef glyphs; got %d",
                            paragraph->unresolvedGlyphs());
        }
    }
}
