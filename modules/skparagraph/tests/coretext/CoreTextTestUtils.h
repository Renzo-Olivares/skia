/*
 * Copyright 2026 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
#ifndef CoreTextTestUtils_DEFINED
#define CoreTextTestUtils_DEFINED

// CoreText tests live in topic files next to this header (coretext/CoreText<Layer>Test.cpp,
// CoreTextLineShaper<Topic>Test.cpp, CoreTextContract<Topic>Test.cpp); helpers that more
// than one of them calls live here, the rest stay in their file. List a new file in
// tests/BUILD.bazel (coretext_tests) and run make -C bazel generate_gni

#include "include/core/SkString.h"
#include "include/core/SkTypeface.h"

#include <initializer_list>

namespace skia::textlayout::coretext_test {

// The expectations name Apple system faces; the portable font manager (--nonativeFonts) has
// none of them, so such tests skip instead of measuring substitutes
bool has_font_families(std::initializer_list<const char*> families);

// PostScript name of typeface: names the exact face (weight, width, slant), unlike the family
// name, so tests can tell which face CoreText or fallback picked
SkString postscript_name(const SkTypeface& typeface);

}  // namespace skia::textlayout::coretext_test

#endif  // CoreTextTestUtils_DEFINED
