/*
 * Copyright 2026 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
#include "include/core/SkString.h"
#include "modules/skparagraph/include/ParagraphStyle.h"
#include "modules/skparagraph/include/TextStyle.h"
#include "modules/skparagraph/src/ParagraphBuilderImpl.h"
#include "modules/skparagraph/src/ParagraphImpl.h"
#include "modules/skparagraph/tests/SkParagraphTestUtils.h"
#include "tests/Test.h"

using namespace skia::textlayout;
using namespace skia::textlayout::test;

#if !defined(SK_PARAGRAPH_NATIVE_CORETEXT)
// Where CoreText is not built, a kCoreText request still lays out through HarfBuzz instead of
// leaving the paragraph without runs
DEF_TEST(CoreTextPipeline_DisabledFallbackToHarfBuzz, reporter) {
    sk_sp<ResourceFontCollection> fontCollection = sk_make_sp<ResourceFontCollection>();
    SKIP_IF_FONTS_NOT_FOUND(reporter, fontCollection)

    TextStyle style;
    style.setFontFamilies({SkString("Roboto")});
    style.setFontSize(16.0f);

    ParagraphStyle paragraphStyle;
    paragraphStyle.setTextShapingEngine(TextShapingEngine::kCoreText);
    ParagraphBuilderImpl builder(paragraphStyle, fontCollection, get_unicode());
    builder.pushStyle(style);
    builder.addText("Fallback to HarfBuzz when CoreText is disabled");
    builder.pop();
    auto paragraph = builder.Build();
    paragraph->layout(500.0f);

    // The engine flag is the contract; no public API reports which engine shaped the paragraph
    auto impl = static_cast<ParagraphImpl*>(paragraph.get());
    REPORTER_ASSERT(reporter, !impl->isCoreTextShaped());
    REPORTER_ASSERT(reporter,
                    paragraph->getMaxIntrinsicWidth() > 0,
                    "got %g",
                    paragraph->getMaxIntrinsicWidth());
}
#endif  // !defined(SK_PARAGRAPH_NATIVE_CORETEXT)
