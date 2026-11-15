/*
 * Copyright 2026 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

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
#include "modules/skparagraph/tests/coretext/CoreTextTestUtils.h"
#include "tests/Test.h"

using namespace skia::textlayout;
using namespace skia::textlayout::coretext_test;

// No tracked issue: when .AppleSystemUIFont meets supplementary Private Use Area codepoints
// (U+100000..U+100002) that no font maps, alongside Latin, CJK and Arabic, CoreText keeps the
// cascade faces for the scripts and draws the PUA codepoints as unresolved primary-font .notdef
// boxes instead of LastResort glyphs; HarfBuzz was measured to fall back to LastResort
DEF_TEST(SkParagraph_AppleSystemUIFontCascade, reporter) {
    // The portable font manager (--nonativeFonts) has none of the Apple faces the cascade names
    if (!has_font_families({".AppleSystemUIFont", "PingFang SC", "Geeza Pro"})) {
        return;
    }
    auto fontCollection = test_font_collection();
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
        skiatest::ReporterContext context(reporter, SkString(coreText ? "kCoreText" : "kDefault"));
        ParagraphStyle style;
        style.setTextShapingEngine(engine);
        ParagraphBuilderImpl builder(style, fontCollection, get_unicode(style));
        TextStyle ts;
        ts.setFontFamilies({SkString(".AppleSystemUIFont")});
        ts.setFontSize(24.0f);
        builder.pushStyle(ts);
        builder.addText(cascadeText);
        builder.pop();

        auto paragraph = builder.Build();
        paragraph->layout(1000.0f);
        // No public API reports the engine, and a silent HarfBuzz fallback passes the rest
        REPORTER_ASSERT(
                reporter,
                static_cast<ParagraphImpl*>(paragraph.get())->isCoreTextShaped() == coreText);

        if (!coreText) {
            // HarfBuzz is printed, not asserted: other changes may fix it
            SkString runs;
            for (const Paragraph::FontInfo& info : paragraph->getFonts()) {
                SkString ps;
                info.fFont.getTypeface()->getPostScriptName(&ps);
                runs.appendf(" %s", ps.c_str());
            }
            INFOF(reporter,
                  "HarfBuzz runs:%s, %d unresolved\n",
                  runs.c_str(),
                  paragraph->unresolvedGlyphs());
            continue;
        }
        bool hasSystemFontRun = false;
        bool hasChineseRun = false;
        bool hasArabicRun = false;
        bool hasLastResortRun = false;
        SkString runs;
        for (const Paragraph::FontInfo& info : paragraph->getFonts()) {
            SkString ps;
            info.fFont.getTypeface()->getPostScriptName(&ps);
            runs.appendf(" %s", ps.c_str());
            hasLastResortRun |= ps.contains("LastResort");
            hasSystemFontRun |= ps.contains("SF") || ps.contains("AppleSystemUIFont");
            hasChineseRun |= ps.contains("PingFang");
            hasArabicRun |= ps.contains("Arabic") || ps.contains("Geeza");
        }
        REPORTER_ASSERT(reporter, hasSystemFontRun, "runs:%s", runs.c_str());
        REPORTER_ASSERT(reporter, hasChineseRun, "runs:%s", runs.c_str());
        REPORTER_ASSERT(reporter, hasArabicRun, "runs:%s", runs.c_str());
        REPORTER_ASSERT(reporter, !hasLastResortRun, "runs:%s", runs.c_str());
        REPORTER_ASSERT(reporter,
                        paragraph->unresolvedGlyphs() == 3,
                        "got %d unresolved",
                        paragraph->unresolvedGlyphs());
    }
}
