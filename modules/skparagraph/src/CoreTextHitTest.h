// Copyright 2026 Google LLC
#ifndef CoreTextHitTest_DEFINED
#define CoreTextHitTest_DEFINED

#include "include/core/SkScalar.h"
#include "modules/skparagraph/include/DartTypes.h"
#include "modules/skparagraph/src/TextLine.h"

namespace skia {
namespace textlayout {

class ParagraphImpl;

// Hit-tests dx against context's glyph slice on a CoreText-shaped line, snapping carets to
// grapheme boundaries in visual order and dropping folded Default_Ignorable code points;
// updates result and returns false once the containing or leftmost slice is hit
bool HitTestRunSlice(const ParagraphImpl& owner,
                     const TextLine::ClipContext& context,
                     SkScalar offsetX,
                     SkScalar dx,
                     PositionWithAffinity* result);

}  // namespace textlayout
}  // namespace skia

#endif  // CoreTextHitTest_DEFINED
