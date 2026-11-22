// Copyright 2020 Google LLC
#ifndef Decorations_DEFINED
#define Decorations_DEFINED

#include "include/core/SkPath.h"
#include "modules/skparagraph/include/ParagraphPainter.h"
#include "modules/skparagraph/include/TextStyle.h"
#include "modules/skparagraph/src/TextLine.h"

namespace skia {
namespace textlayout {

class Decorations {
    public:
    void paint(ParagraphPainter* painter, const TextStyle& textStyle, const TextLine::ClipContext& context, SkScalar baseline);

    private:
    // Populates fFontMetrics from run's typeface (native strikeout metrics when the run's
    // paragraph is CoreText-shaped) and sets fThickness scaled by decorationThicknessMultiplier
    void calculateThickness(TextStyle textStyle, const Run& run);
    void calculatePosition(TextDecoration decoration, SkScalar ascent);
    void calculatePaint(const TextStyle& textStyle);
    void calculateWaves(const TextStyle& textStyle, SkRect clip);
    void calculateGaps(const TextLine::ClipContext& context, const SkRect& rect, SkScalar baseline, SkScalar halo);

    SkScalar fThickness;
    SkScalar fPosition;

    SkFontMetrics fFontMetrics;
    ParagraphPainter::DecorationStyle fDecorStyle;
    SkPath fPath;
};
}  // namespace textlayout
}  // namespace skia
#endif
