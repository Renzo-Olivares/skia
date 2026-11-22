// Copyright 2026 Google LLC
#ifndef CoreTextRunSteps_DEFINED
#define CoreTextRunSteps_DEFINED

#include "include/core/SkFont.h"
#include "include/core/SkPoint.h"
#include "include/core/SkRefCnt.h"
#include "include/core/SkScalar.h"
#include "include/core/SkSpan.h"
#include "include/core/SkTypeface.h"
#include "modules/skparagraph/include/TextStyle.h"
#include "modules/skunicode/include/SkUnicode.h"
#include "src/utils/mac/SkUniqueCFRef.h"

#include <CoreText/CoreText.h>

#include <cstdint>
#include <vector>

// CTRun and style steps of the CoreTextLineShaper paragraph pipeline (CoreTextSegmentShaper.cpp)
namespace skia {
namespace textlayout {

// Checks whether ctFont shares the PostScript or family name of any entry in allowedTypefaces, or
// matches any requested family name in fontFamilies. Both names come from the CGFont (CoreText
// synthesizes a PostScript name for a nameless face), so a CTFont sharing a face shares its names
bool is_allowed_font(CTFontRef ctFont,
                     const std::vector<sk_sp<SkTypeface>>& allowedTypefaces,
                     const std::vector<SkString>& fontFamilies);

// Returns the CTFont CoreText bound to ctRun during typesetting and font substitution
CTFontRef ct_run_font(CTRunRef ctRun);

// Reports whether font is Apple's LastResort face, which CoreText binds once every cascade font
// lacks the code point; its glyphs stand for unresolved text
bool is_last_resort_font(CTFontRef font);

// Sets font's edging, hinting and subpixel from style. When fakeMissingFontStyles is true and font
// has a typeface, also sets embolden and skew for the weight or italic that style asks for and the
// face lacks
void apply_font_style_props(SkFont* font, const TextStyle& style, bool fakeMissingFontStyles);

// True for a zero, negative or NaN TextStyle font size, which the attributed string shapes at a
// stand-in size and the run collapses to zero advances
bool has_non_positive_font_size(const TextStyle& style);

// Builds a run's SkFont with typeface at ctFont's size, collapsing the stand-in size to zero when
// has_non_positive_font_size(style), and applies style's properties
SkFont make_run_font(sk_sp<SkTypeface> typeface,
                     CTFontRef ctFont,
                     const TextStyle& style,
                     bool fakeMissingFontStyles);

// Returns the BiDi embedding level covering utf8Pos, or defaultLevel when no region covers it
uint8_t find_bidi_level(SkSpan<const SkUnicode::BidiRegion> regions,
                        size_t utf8Pos,
                        uint8_t defaultLevel);

// Reconciles an ICU/Unicode BiDi embedding level's parity with ctRun's shaped direction to ensure
// downstream visual reordering matches
uint8_t reconcile_bidi_level(uint8_t level, CTRunRef ctRun);

// Reordered complex-script glyphs (kCTRunStatusNonMonotonic) can report out-of-order string
// indices; Run::iterateThroughClustersInTextOrder requires non-decreasing indices for LTR and
// non-increasing for RTL while keeping CoreText's glyph IDs and positions untouched
void normalize_cluster_indices(SkSpan<uint32_t> clusters, bool ltr);

// One CTLine for the whole of attrStr. Every font in it keeps its trak, fallback fonts too, so
// the letterSpacing SkParagraph adds later stacks on the trak as native tracking does, except
// in cursive scripts, which SkParagraph does not space
SkUniqueCFRef<CTLineRef> typeset_line(CFAttributedStringRef attrStr);

// Converts a CoreText glyph origin (y-up) into a Skia run glyph offset (y-down), pinning
// zero-size stand-in origins to (0, 0) and adding splitOffsetX to restore drawn x after moving
// half a kerning pair onto the glyph's advance
SkPoint glyph_draw_offset(CGPoint origin, bool zeroSize, SkScalar splitOffsetX);

// How extract_ctrun_glyphs turns CoreText's base advances into run advances
enum class AdvanceMode {
    // Zero-size stand-in run: zero advances, origins pinned to (0, 0)
    kZero,
    // Non-subpixel font: advances rounded to whole pixels
    kRounded,
    // Subpixel font: advances as CoreText gives them
    kSubpixel,
    // Subpixel font: half of each pair kerning moves into the next glyph's advance and offset
    kSplitKerning,
};

// Maps a run's size, SkFont::isSubpixel and kerning split onto its AdvanceMode; splitKerning
// requires a subpixel run with a real font size
AdvanceMode advance_mode(bool zeroSize, bool subpixel, bool splitKerning);

// Extracts ctRun's glyph IDs, Skia draw offsets, and horizontal glyph advances into the equally
// sized spans as mode says; ctFont gives the nominal advances for kSplitKerning
void extract_ctrun_glyphs(CTRunRef ctRun,
                          CTFontRef ctFont,
                          AdvanceMode mode,
                          SkSpan<SkGlyphID> glyphsOut,
                          SkSpan<SkPoint> offsetsOut,
                          SkSpan<SkScalar> advancesOut);

}  // namespace textlayout
}  // namespace skia

#endif  // CoreTextRunSteps_DEFINED
