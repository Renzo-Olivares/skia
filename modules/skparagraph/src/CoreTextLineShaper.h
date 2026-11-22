// Copyright 2026 Google LLC
#ifndef CoreTextLineShaper_DEFINED
#define CoreTextLineShaper_DEFINED

#include "include/core/SkRefCnt.h"
#include "include/core/SkScalar.h"
#include "include/core/SkSpan.h"
#include "include/core/SkTypes.h"
#include "modules/skparagraph/include/DartTypes.h"
#include "modules/skparagraph/include/TextStyle.h"
#include "modules/skparagraph/src/CoreTextSpanFonts.h"
#include "src/core/SkTHash.h"
#include "src/utils/mac/SkUniqueCFRef.h"

#include <CoreText/CoreText.h>

#include <memory>
#include <string_view>

namespace skia {
namespace textlayout {

class ParagraphImpl;
class Run;

// What shaping learns about a CTFont CoreText bound to a CTRun: a requested span font or a
// cascade face CoreText chose. font retains the key: a fallback CTFont dies with its CTLine,
// and a later font at the same address must not hit this entry
struct CoreTextBoundFont {
    SkUniqueCFRef<CTFontRef> font;
    bool lastResort = false;
    // Built on the first run that renders with font, null until then
    sk_sp<SkTypeface> typeface;
};
using CoreTextBoundFonts = skia_private::THashMap<CTFontRef, CoreTextBoundFont>;

// Shapes a kCoreText paragraph into its Runs with one CTLine per hard-break segment, and its
// ellipses and hyphens. Every member here writes Run or ParagraphImpl private state, which both
// grant to this class as a friend; steps that need only public API live as free functions in
// CoreTextRunSteps.h and CoreTextHitTest.h
class CoreTextLineShaper {
public:
    // Binds the shaper to non-null paragraph; shape() populates its runs, font switches, and
    // unresolved glyph state in place
    explicit CoreTextLineShaper(ParagraphImpl* paragraph);

    // Replaces the paragraph's runs, font switches and unresolved code points with ones from
    // CoreText: one CTLine per hard-break segment, plus a run per placeholder. Returns false only
    // for empty text, and then leaves the runs, font switches and unresolved code points empty
    bool shape();

    size_t unresolvedGlyphs() const { return fUnresolvedGlyphs; }

    // Shapes an ellipsis or hyphen in one CTLine (style's font, cascade, trak, features) into one
    // Run of owner with clusterRun's line height, retrying with clusterRun's typeface; never uses
    // SkShaper or the default font manager. LastResort or disallowed fallback glyphs become glyph
    // 0, so the run reports itself unresolved
    static std::unique_ptr<Run> ShapeEllipsis(ParagraphImpl* owner,
                                              std::string_view text,
                                              const TextStyle& style,
                                              const Run& clusterRun,
                                              uint8_t bidiLevel);

    // Spaces runs that ParagraphImpl::runsDifferOnlyByHardBreaks accepted as one run, never inside
    // a grapheme; moves glyph positions (RTL also rebases fOffset.fX) and builds no clusters
    static void LetterSpaceRuns(SkSpan<Run> runs, SkScalar letterSpacing);

private:
    // Shapes one ellipsis or hyphen for ShapeEllipsis; defined in CoreTextEllipsisShaper.cpp
    class EllipsisShaper;
    // Shapes one hard-break segment in one CTLine; defined in CoreTextSegmentShaper.cpp
    class SegmentShaper;

    // Appends placeholder's single-glyph Run at fAdvanceX and advances fAdvanceX by its width
    void appendPlaceholderRun(const Placeholder& placeholder, size_t placeholderIndex);

    // Splits range after each '\n' and shapes every piece with a SegmentShaper
    void shapeTextRange(TextRange range);

    // Scans glyphRange in fParagraph->text() and records any non-control, non-Default_Ignorable
    // code points in fParagraph->fUnresolvedCodepoints while incrementing fUnresolvedGlyphs once
    void recordUnresolvedGlyph(TextRange glyphRange);

    ParagraphImpl* fParagraph;
    size_t fUnresolvedGlyphs = 0;
    // Pen x of the next run on the paragraph's single unwrapped line
    SkScalar fAdvanceX = 0;
    // Index in fParagraph->fPlaceholders of the next placeholder whose U+FFFC CTRun comes up
    size_t fNextPlaceholder = 0;
    // Requested span fonts, shared by the attributed strings of all '\n' segments
    CoreTextRequestedFonts fRequestedFonts;
    // Fonts CoreText bound to CTRuns, cascade faces included, shared by the CTLines of all
    // '\n' segments
    CoreTextBoundFonts fBoundFonts;
};

}  // namespace textlayout
}  // namespace skia

#endif  // CoreTextLineShaper_DEFINED
