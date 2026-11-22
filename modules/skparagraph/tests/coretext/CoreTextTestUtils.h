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

#include "include/core/SkFont.h"
#include "include/core/SkFontArguments.h"
#include "include/core/SkRefCnt.h"
#include "include/core/SkScalar.h"
#include "include/core/SkString.h"
#include "include/core/SkTypeface.h"
#include "include/core/SkTypes.h"
#include "modules/skparagraph/include/DartTypes.h"
#include "modules/skparagraph/include/FontCollection.h"
#include "modules/skparagraph/include/ParagraphStyle.h"
#include "modules/skparagraph/src/CoreTextAttrStringBuilder.h"
#include "modules/skunicode/include/SkUnicode.h"
#include "src/utils/mac/SkUniqueCFRef.h"

#include <CoreText/CoreText.h>

#include <initializer_list>
#include <memory>
#include <optional>
#include <vector>

namespace skia::textlayout::coretext_test {

// Each engine gets the SkUnicode its clients ship with: Apple's for CoreText, ICU for HarfBuzz.
// A build lacking it yields null, which ParagraphBuilder::Build rejects instead of silently
// laying out with another implementation
sk_sp<SkUnicode> get_unicode(const ParagraphStyle& style);

// TestFontMgr as the default manager with font fallback on: system fonts under --nativeFonts,
// the portable test fonts otherwise, so system-font tests must skip via has_font_families
sk_sp<FontCollection> test_font_collection();

// Apple's SkUnicode, which CoreText clients ship with, or ICU, for tests that drive
// CoreTextAttrStringBuilder directly; never freed, like the SkUnicode a client keeps for all of
// its paragraphs
SkUnicode* test_unicode();

// The expectations name Apple system faces; the portable font manager (--nonativeFonts) has
// none of them, so such tests skip instead of measuring substitutes
bool has_font_families(std::initializer_list<const char*> families);

// PostScript name of typeface: names the exact face (weight, width, slant), unlike the family
// name, so tests can tell which face CoreText or fallback picked
SkString postscript_name(const SkTypeface& typeface);

using Coordinate = SkFontArguments::VariationPosition::Coordinate;

// regular as asset family "Roboto", with the default TestFontMgr after it, which decides the
// fallback order for code points Roboto lacks
sk_sp<FontCollection> make_roboto_collection(const sk_sp<SkTypeface>& regular);

// Painted x of each glyph of the last blob of paragraph; on a truncated line that is the ellipsis,
// which visit() walks after the runs of its line
std::vector<SkScalar> last_blob_x(Paragraph* paragraph);

// A CTLine with the x and advance of each glyph in CTLine visual order, and its typographic width
struct PlainLine {
    SkUniqueCFRef<CTLineRef> line;
    std::vector<SkScalar> x;
    std::vector<SkScalar> advances;
    SkScalar width = 0;
};

// Glyph x and width CoreText itself draws for text on one CTLine in font; without a direction
// the line takes the paragraph direction of its first strong character
PlainLine coretext_line(CTFontRef font,
                        const char* text,
                        std::optional<TextDirection> direction = std::nullopt);

// A copy of font without its trak. The private attribute only builds the trak-free
// references that prove a test can tell a line that keeps the trak from one that
// drops it
SkUniqueCFRef<CTFontRef> untracked_copy(CTFontRef font);

// Ahem has square glyphs (advance = font size), like the FlutterTest font of the Flutter tests
sk_sp<FontCollection> make_ahem_collection();

// text in 14pt Ahem with letterSpacing, laid out at width with the rounding hack off
std::unique_ptr<Paragraph> layout_ahem(const sk_sp<FontCollection>& fontCollection,
                                       const char* text,
                                       TextDirection direction,
                                       SkScalar letterSpacing,
                                       SkScalar width);

// One-style kCoreText paragraph of text on one unwrapped line, without the rounding hack
std::unique_ptr<Paragraph> layout_single_style(const sk_sp<FontCollection>& fontCollection,
                                               const char* family,
                                               const char* text,
                                               SkScalar fontSize,
                                               bool subpixel);

struct PaintedGlyph {
    SkGlyphID id;
    SkScalar drawnX;
    SkScalar nominal;
};

// Glyphs paragraph draws, in paint order, at their drawn x with their nominal advances from
// CTFontGetAdvancesForGlyphs (font data including optical size and trak, which SkFont::getWidths
// leaves out); when non-null, font receives the last glyph run's SkFont
std::vector<PaintedGlyph> painted_glyphs(Paragraph* paragraph, SkFont* font = nullptr);

}  // namespace skia::textlayout::coretext_test

#endif  // CoreTextTestUtils_DEFINED
