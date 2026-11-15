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
#include "modules/skparagraph/include/Paragraph.h"
#include "modules/skparagraph/include/ParagraphStyle.h"
#include "modules/skparagraph/include/TextStyle.h"
#include "modules/skparagraph/src/CoreTextAttrStringBuilder.h"
#include "modules/skparagraph/src/ParagraphBuilderImpl.h"
#include "modules/skunicode/include/SkUnicode.h"
#include "src/utils/mac/SkUniqueCFRef.h"
#include "tests/Test.h"

#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>

#include <initializer_list>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
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

// Tests that name Apple system faces skip under the portable font manager
// (--nonativeFonts), which has none of them
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

// Apple's SkUnicode, which CoreText clients ship with; ICU only in builds without it
sk_sp<SkUnicode> contract_unicode();

// System fonts with fallback on, like a Flutter app without --use-test-fonts
sk_sp<FontCollection> system_collection();

// Only the resource fonts registered under their family names, fallback off, like a Flutter
// test run with --use-test-fonts; nullptr when a resource is missing
sk_sp<FontCollection> asset_collection(
        std::initializer_list<std::pair<const char*, const char*>> resourceAndFamily);

// ParagraphStyle on the CoreText engine with the rounding hack off, as Flutter sets it
ParagraphStyle contract_paragraph_style();

// Builder for multi-span paragraphs; finish them with layout_built
std::unique_ptr<ParagraphBuilderImpl> contract_builder(const sk_sp<FontCollection>& fontCollection,
                                                       const ParagraphStyle& style);

// Builds and lays out at width. isCoreTextShaped is internal, but the contracts are about the
// CoreText engine: a silent fallback to HarfBuzz would make every check below meaningless.
// Empty text skips shaping on both engines
std::unique_ptr<Paragraph> layout_built(skiatest::Reporter* reporter,
                                        ParagraphBuilderImpl* builder,
                                        SkScalar width);

std::unique_ptr<Paragraph> layout_text(skiatest::Reporter* reporter,
                                       const sk_sp<FontCollection>& fontCollection,
                                       const ParagraphStyle& paragraphStyle,
                                       const TextStyle& style,
                                       std::string_view text,
                                       SkScalar width);

// TextStyle with only the family and the font size set; every other field keeps its default
TextStyle contract_text_style(const char* family, SkScalar size);

// A glyph as Paragraph::visit draws it: id, absolute position (run origin plus glyph position)
// and the font of its run; lineNumber is the visit line, and utf8Start the UTF-8 index of its
// cluster, which tells marks from their bases
struct DrawnGlyph {
    SkGlyphID glyph;
    SkPoint position;
    SkFont font;
    int lineNumber;
    uint32_t utf8Start;
};

// Every drawn glyph in paint order, including the ellipsis run
std::vector<DrawnGlyph> drawn_glyphs(Paragraph* paragraph);

// Glyph ids of drawn_glyphs, in the same paint order
std::vector<SkGlyphID> drawn_glyph_ids(Paragraph* paragraph);

// Paragraph::getLineMetrics, one entry per line
std::vector<LineMetrics> line_metrics(Paragraph* paragraph);

// Tight boxes of the UTF-16 range [start, end)
std::vector<TextBox> tight_boxes(Paragraph* paragraph, size_t start, size_t end);

// The one tight box of [start, end); reports a failure and gives nullopt when the range comes
// back as several boxes or none, which the caller then skips
std::optional<TextBox> single_tight_box(skiatest::Reporter* reporter,
                                        Paragraph* paragraph,
                                        size_t start,
                                        size_t end);

// Family name of the typeface the font draws with
SkString family_name(const SkFont& font);

// Design coordinate of axis on typeface; nullopt when the face has no such axis
std::optional<SkScalar> axis_coordinate(const SkTypeface* typeface, SkFourByteTag axis);

// The native side of the contracts: CTFontCreateWithName of family at size
SkUniqueCFRef<CTFontRef> native_font(const char* family, SkScalar size);

// The native oracle for line widths, line metrics and caret offsets, which the contracts may
// compare for equality: one CTLine of text in font, with kCTTrackingAttributeName (as
// NSTrackingAttributeName) when tracking is not 0, and an explicit base writing direction when
// baseDirection is set; CoreText falls back on its own for characters the font lacks
SkUniqueCFRef<CTLineRef> native_line(CTFontRef font,
                                     std::string_view text,
                                     SkScalar tracking,
                                     std::optional<TextDirection> baseDirection = std::nullopt);

// Typographic width of line, trailing whitespace included
SkScalar native_line_width(CTLineRef line);

// Ahem squares are one em wide, so the expected widths of the spacing contracts are exact sums
constexpr SkScalar kAhemSize = 14;

// contract_text_style with letterSpacing set
TextStyle spaced_text_style(const char* family, SkScalar size, SkScalar letterSpacing);

// LineMetrics::fWidth of the first line: no trailing spaces and, unlike the intrinsic widths,
// not rounded to 0.01. -1 when the paragraph has no line, which no expected width matches
SkScalar first_line_width(Paragraph* paragraph);

// The font's own advance of a drawn glyph: SkFont::getWidths reads hmtx at the face's opsz and
// never applies trak, kerning or letterSpacing
SkScalar font_advance(const DrawnGlyph& glyph);

// CoreText sums CGFloat advances while SkParagraph walks the pen in float; 1/4096 pt absorbs
// that accumulation on these short lines
constexpr SkScalar kNativeWidthTolerance = 1.0f / 4096;

}  // namespace skia::textlayout::coretext_test

#endif  // CoreTextTestUtils_DEFINED
