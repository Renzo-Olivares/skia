/*
 * Copyright 2026 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
#include "modules/skparagraph/tests/coretext/CoreTextTestUtils.h"

#include "include/core/SkFontMgr.h"
#include "include/core/SkFontStyle.h"
#if defined(SK_UNICODE_APPLE_IMPLEMENTATION)
#include "modules/skunicode/include/SkUnicode_apple.h"
#endif
#if defined(SK_UNICODE_ICU_IMPLEMENTATION)
#include "modules/skunicode/include/SkUnicode_icu.h"
#endif
#include "tools/fonts/FontToolUtils.h"

namespace skia::textlayout::coretext_test {

sk_sp<FontCollection> test_font_collection() {
    auto fontCollection = sk_make_sp<FontCollection>();
    fontCollection->setDefaultFontManager(ToolUtils::TestFontMgr());
    return fontCollection;
}

SkUnicode* test_unicode() {
#if defined(SK_UNICODE_APPLE_IMPLEMENTATION)
    static SkUnicode* unicode = SkUnicodes::Apple::Make().release();
#else
    static SkUnicode* unicode = SkUnicodes::ICU::Make().release();
#endif
    SkASSERT(unicode);
    return unicode;
}

bool has_font_families(std::initializer_list<const char*> families) {
    for (const char* family : families) {
        if (!ToolUtils::TestFontMgr()->matchFamilyStyle(family, SkFontStyle())) {
            return false;
        }
    }
    return true;
}

SkString postscript_name(const SkTypeface& typeface) {
    SkString name;
    typeface.getPostScriptName(&name);
    return name;
}

}  // namespace skia::textlayout::coretext_test
