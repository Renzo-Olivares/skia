// Copyright 2026 Google LLC
#ifndef CoreTextLineShaper_DEFINED
#define CoreTextLineShaper_DEFINED

#include "include/core/SkFourByteTag.h"
#include "include/core/SkScalar.h"
#include "include/core/SkTypes.h"
#include "modules/skparagraph/include/DartTypes.h"
#include "modules/skparagraph/include/ParagraphStyle.h"
#include "modules/skparagraph/include/TextStyle.h"

#include "include/core/SkRefCnt.h"
#include "modules/skshaper/include/SkShaper.h"
#include <CoreText/CoreText.h>

class SkString;

namespace skia {
namespace textlayout {

class FontCollection;
class ParagraphImpl;

class CoreTextLineShaper {
public:
    explicit CoreTextLineShaper(ParagraphImpl* paragraph);

    // Replaces the paragraph's runs, font switches and unresolved code points with ones from
    // CoreText: one CTLine per hard-break segment, plus a run per placeholder. Returns false for
    // empty text or when CoreText fails, which leaves the runs of earlier segments behind
    bool shape();

    size_t unresolvedGlyphs() const { return fUnresolvedGlyphs; }

    // Identifies fallback to LastResort font when glyph cascades are exhausted
    static bool isLastResortFont(CTFontRef font);

    // Code points that add no width to a run that falls back to .notdef boxes
    static bool IsZeroWidthCodepoint(SkUnichar utf8);

    // Shapes an ellipsis or hyphen in one CTLine with the style's font, cascade, trak and features
    // (liga off when letter-spaced), without SkShaper or the default font manager. All CTRuns go
    // into one RunHandler buffer; LastResort or disallowed fallback glyphs become glyph 0 so the
    // run reports itself unresolved, as runs of the paragraph text do
    static void ShapeAffix(const SkString& text,
                           const TextStyle& style,
                           sk_sp<SkTypeface> clusterTypeface,
                           bool fakeMissingFontStyles,
                           uint8_t bidiLevel,
                           sk_sp<FontCollection> fontCollection,
                           SkShaper::RunHandler* handler);

private:
    // Splits range after each '\n' and shapes every piece with shapeTextRegion; false on the
    // first piece CoreText fails on
    bool shapeTextRange(TextRange range, SkScalar& advanceX);
    // Shapes regionRange in one CTLine with its blocks' styles and appends a Run and a font
    // switch per CTRun, starting at advanceX, which grows by the line's advance. Also counts
    // unresolved glyphs and records their code points. False if CoreText fails
    bool shapeTextRegion(TextRange regionRange, SkScalar& advanceX);

    ParagraphImpl* fParagraph;
    size_t fUnresolvedGlyphs;
};

}  // namespace textlayout
}  // namespace skia

#endif  // CoreTextLineShaper_DEFINED
