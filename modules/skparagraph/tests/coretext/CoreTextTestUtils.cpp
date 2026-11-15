/*
 * Copyright 2026 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
#include "modules/skparagraph/tests/coretext/CoreTextTestUtils.h"

#include "include/core/SkFontMgr.h"
#include "include/core/SkFontStyle.h"
#include "tools/fonts/FontToolUtils.h"

namespace skia::textlayout::coretext_test {

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
