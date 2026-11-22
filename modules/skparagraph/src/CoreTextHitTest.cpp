// Copyright 2026 Google LLC
#include "modules/skparagraph/src/CoreTextHitTest.h"

#include "include/core/SkScalar.h"
#include "include/core/SkSpan.h"
#include "include/private/SkTArray.h"
#include "include/private/SkTo.h"
#include "modules/skparagraph/src/CoreTextUtf8.h"
#include "modules/skparagraph/src/ParagraphImpl.h"
#include "modules/skparagraph/src/Run.h"
#include "modules/skunicode/include/SkUnicode.h"

#include <algorithm>

namespace skia {
namespace textlayout {

namespace {

// Where dx falls against a slice: left of its clip, right of its clip (or anywhere on an empty
// hard-break slice), or strictly inside a non-empty slice
enum class EdgeHit { kLeftOfSlice, kRightOfSlice, kInside };

// Hit-tests dx against one TextLine clip slice [context.pos, context.pos + context.size) of a
// CoreText-shaped run; glyph indices are run-local, TextIndex values paragraph-global UTF-8
class SliceHitTester {
public:
    SliceHitTester(const ParagraphImpl& owner,
                   const TextLine::ClipContext& context,
                   SkScalar offsetX,
                   SkScalar dx)
            : fOwner(owner)
            , fContext(context)
            , fRun(*context.run)
            , fLtr(context.run->leftToRight())
            , fSliceStart(context.pos)
            , fSliceEnd(context.pos + context.size)
            , fOffsetX(offsetX)
            , fDx(dx) {
        SkASSERT(fSliceEnd <= fRun.size());
    }

    // Writes the caret for dx and returns the HitTestRunSlice continuation flag
    bool hitTest(PositionWithAffinity* result) const {
        // Every later slice starts right of a left-edge hit, so only a right-edge hit goes on
        if (const EdgeHit edge = this->hitEdges(result); edge != EdgeHit::kInside) {
            return edge == EdgeHit::kRightOfSlice;
        }
        const size_t found = this->anchorGlyph(this->findGlyph());
        const GlyphRange span = this->clusterGlyphs(found);
        TextIndex start8 = fRun.globalClusterIndex(found);
        TextIndex end8 = this->clusterEndUtf8(found);
        const SkScalar clusterLeft = this->glyphX(span.start);
        // Use calculateWidth so justified lines include the last slice glyph's justification
        // shift rather than subtracting the next wrapped line's unjustified glyph position
        const SkScalar clusterWidth = fRun.calculateWidth(span.start, span.end, false);
        auto graphemes = fOwner.countSurroundingGraphemes({start8, end8});
        // CoreText omits Default_Ignorable code points without emitting glyphs; drop them so they
        // take no share of the preceding visible glyph's width
        const bool droppedIgnorable = this->dropIgnorableGraphemes(&graphemes);

        SkScalar center = clusterLeft + clusterWidth / 2;
        if (graphemes.size() > 1 || droppedIgnorable) {
            const SkScalar graphemeWidth = clusterWidth / graphemes.size();
            int visualIndex = SkScalarNearlyZero(graphemeWidth)
                                      ? 0
                                      : SkScalarFloorToInt((fDx - clusterLeft) / graphemeWidth);
            visualIndex = std::clamp(visualIndex, 0, SkToInt(graphemes.size()) - 1);
            center = clusterLeft + visualIndex * graphemeWidth + graphemeWidth / 2;
            // countSurroundingGraphemes lists graphemes in logical order, which runs right-to-left
            // across an RTL ligature
            start8 = graphemes[fLtr ? SkToSizeT(visualIndex)
                                    : graphemes.size() - 1 - SkToSizeT(visualIndex)];
            // start8 itself is a kGraphemeStart byte, so the grapheme's end is searched from the
            // byte after it
            end8 = start8 + 1;
        }
        if ((fDx < center) == fLtr) {
            start8 = fOwner.findPreviousGraphemeBoundary(start8);
            *result = {SkToS32(fOwner.getUTF16Index(start8)), Affinity::kDownstream};
        } else {
            end8 = fOwner.findNextGraphemeBoundary(end8);
            *result = {SkToS32(fOwner.getUTF16Index(end8)), Affinity::kUpstream};
        }
        return false;
    }

private:
    SkScalar glyphX(size_t glyph) const {
        return fRun.positionX(glyph) + fContext.fTextShift + fOffsetX;
    }

    // Moves glyph boundary edge toward limit past glyphs of cluster start8: glyphs at or before
    // start8 in text order and combining marks skipped by an IgnoreMarks ligature (not a grapheme
    // start). Moving down probes the glyph below edge, so the result never crosses limit
    size_t extendClusterEdge(TextIndex start8, size_t edge, size_t limit) const {
        const bool up = edge < limit;
        while (edge != limit) {
            const TextIndex next8 = fRun.globalClusterIndex(up ? edge : edge - 1);
            if (next8 > start8 &&
                fOwner.codeUnitHasProperty(next8, SkUnicode::CodeUnitFlags::kGraphemeStart)) {
                break;
            }
            edge = up ? edge + 1 : edge - 1;
        }
        return edge;
    }

    // Returns the UTF-8 end index of glyph pos's cluster, stepping past adjacent glyphs that
    // share pos's cluster index or belong to combining marks skipped by an IgnoreMarks ligature
    TextIndex clusterEndUtf8(size_t pos) const {
        SkASSERT(pos <= fRun.size());
        const TextIndex clusterStart8 = fRun.globalClusterIndex(pos);
        if (fLtr) {
            // LTR callers only pass glyph indices inside the slice, which ends at or before
            // fRun.size()
            SkASSERT(pos < fRun.size());
            return fRun.globalClusterIndex(
                    this->extendClusterEdge(clusterStart8, pos + 1, fRun.size()));
        }
        // RTL glyphs sit in reverse logical order, so the next logical cluster lies at a lower
        // glyph index and glyph 0 ends at fRun.textRange().end
        const size_t endGlyph = this->extendClusterEdge(clusterStart8, pos, 0);
        return endGlyph == 0 ? fRun.textRange().end : fRun.globalClusterIndex(endGlyph - 1);
    }

    // Steps slice glyph found onto its owning grapheme start: a detached combining-mark glyph
    // sits right of its base in LTR and left of its base or IgnoreMarks ligature in RTL
    size_t anchorGlyph(size_t found) const {
        SkASSERT(fSliceStart <= found && found < fSliceEnd);
        while (!fOwner.codeUnitHasProperty(fRun.globalClusterIndex(found),
                                           SkUnicode::CodeUnitFlags::kGraphemeStart)) {
            if (fLtr) {
                // Stop at the slice edge when a style or line break split a mark from its base
                if (found == fSliceStart) {
                    break;
                }
                --found;
            } else {
                if (found + 1 >= fSliceEnd) {
                    break;
                }
                ++found;
            }
        }
        return found;
    }

    // Returns the visual glyph span inside the slice of anchored glyph found's cluster, including
    // co-clustered glyphs and detached combining marks so it covers the full cluster width in
    // both LTR (marks to the right) and RTL (marks and later ligature components to the left)
    GlyphRange clusterGlyphs(size_t found) const {
        const TextIndex start8 = fRun.globalClusterIndex(found);
        size_t startGlyph = found;
        size_t endGlyph = found + 1;
        if (fLtr) {
            while (startGlyph > fSliceStart && fRun.globalClusterIndex(startGlyph - 1) == start8) {
                --startGlyph;
            }
            endGlyph = this->extendClusterEdge(start8, endGlyph, fSliceEnd);
        } else {
            startGlyph = this->extendClusterEdge(start8, startGlyph, fSliceStart);
            while (endGlyph < fSliceEnd && fRun.globalClusterIndex(endGlyph) == start8) {
                ++endGlyph;
            }
        }
        return GlyphRange(startGlyph, endGlyph);
    }

    // Filters graphemes in place to keep only visible graphemes (not solely Default_Ignorable)
    // unless every grapheme in the cluster is ignorable; returns true when graphemes shrank
    bool dropIgnorableGraphemes(skia_private::TArray<TextIndex>* graphemes) const {
        // ParagraphImpl constructors assert fUnicode and layout returns before shaping without it
        SkUnicode* unicode = fOwner.getUnicode().get();
        SkASSERT(unicode);
        if (graphemes->size() <= 1) {
            return false;
        }
        const SkSpan<const char> text = fOwner.text();
        int kept = 0;
        for (int i = 0; i < graphemes->size(); ++i) {
            const TextIndex start = (*graphemes)[i];
            SkASSERT(start < text.size());
            const TextIndex end = fOwner.findNextGraphemeBoundary(start + 1);
            const char* ptr = text.data() + start;
            const char* endPtr = text.data() + end;
            bool ignorable = ptr < endPtr;
            // Any non-ignorable scalar (base letter or visible mark) makes the grapheme visible
            while (ptr < endPtr) {
                const SkUnichar u = next_utf8_scalar(&ptr, endPtr, -1);
                if (u < 0 || !unicode->isDefaultIgnorable(u)) {
                    ignorable = false;
                    break;
                }
            }
            if (!ignorable) {
                (*graphemes)[kept++] = start;
            }
        }
        // Keep an all-ignorable cluster intact so a standalone ignorable glyph still hit-tests
        if (kept == 0 || kept == graphemes->size()) {
            return false;
        }
        graphemes->pop_back_n(graphemes->size() - kept);
        return true;
    }

    // Finds the slice glyph whose left edge is closest at or to the left of fDx, matching
    // TextLine's 0.01 rounding hack and RTL exact-edge tie-break
    size_t findGlyph() const {
        size_t found = fSliceStart;
        for (size_t index = fSliceStart; index < fSliceEnd; ++index) {
            SkScalar left = this->glyphX(index);
            if (fOwner.getApplyRoundingHack()) {
                left = SkScalarRoundToScalar(left * 100.0f) / 100.0f;
            }
            if (left > fDx) {
                break;
            }
            // At an exact RTL glyph boundary, pick the glyph starting at fDx rather than scanning
            // right
            if (left == fDx && !fLtr) {
                return index;
            }
            found = index;
        }
        return found;
    }

    // Snaps fDx outside context.clip (or on an empty hard-break slice) to the slice's outer
    // grapheme boundary and reports which edge it hit; kInside, leaving result alone, when fDx
    // falls strictly inside a non-empty slice
    EdgeHit hitEdges(PositionWithAffinity* result) const {
        if (fDx <= fContext.clip.fLeft) {
            if (fLtr) {
                // A slice can start on a detached combining mark's glyph cluster
                const TextIndex start8 = fRun.globalClusterIndex(fSliceStart);
                const size_t utf16 =
                        fOwner.getUTF16Index(fOwner.findPreviousGraphemeBoundary(start8));
                *result = {SkToS32(utf16), Affinity::kDownstream};
                return EdgeHit::kLeftOfSlice;
            }
            // The leftmost RTL glyph is the slice's logical end and may be a composed grapheme or
            // multi-grapheme ligature, so land at its cluster's trailing grapheme boundary
            const TextIndex end8 = this->clusterEndUtf8(fSliceStart);
            const size_t utf16 = fOwner.getUTF16Index(fOwner.findNextGraphemeBoundary(end8));
            *result = {SkToS32(utf16), Affinity::kUpstream};
            // Visual runs go left to right, so every later slice starts right of fDx and would
            // overwrite this caret with an earlier logical position, even when this slice wraps
            return EdgeHit::kLeftOfSlice;
        }

        // Hard-break-only slices are empty, and NaN fDx skips both <= and >= edge checks
        if (fDx >= fContext.clip.fRight || fSliceStart == fSliceEnd) {
            // An RTL slice's right edge is its logical start, the cluster of its rightmost glyph:
            // glyph fSliceEnd lies right of it and, when a soft wrap splits the run, starts the
            // last grapheme of the previous line
            const size_t edgeGlyph = !fLtr && fSliceEnd > fSliceStart ? fSliceEnd - 1 : fSliceEnd;
            // Snap to the outer grapheme edge in case a slice edge splits a mark cluster
            const TextIndex edge8 = fRun.globalClusterIndex(edgeGlyph);
            const TextIndex snapped8 = fLtr ? fOwner.findNextGraphemeBoundary(edge8)
                                            : fOwner.findPreviousGraphemeBoundary(edge8);
            *result = {SkToS32(fOwner.getUTF16Index(snapped8)),
                       fLtr ? Affinity::kUpstream : Affinity::kDownstream};
            return EdgeHit::kRightOfSlice;
        }
        return EdgeHit::kInside;
    }

    const ParagraphImpl& fOwner;
    const TextLine::ClipContext& fContext;
    const Run& fRun;
    const bool fLtr;
    const size_t fSliceStart;
    const size_t fSliceEnd;
    const SkScalar fOffsetX;
    const SkScalar fDx;
};

}  // namespace

bool HitTestRunSlice(const ParagraphImpl& owner,
                     const TextLine::ClipContext& context,
                     SkScalar offsetX,
                     SkScalar dx,
                     PositionWithAffinity* result) {
    SkASSERT(context.run);
    SkASSERT(result);
    return SliceHitTester(owner, context, offsetX, dx).hitTest(result);
}

}  // namespace textlayout
}  // namespace skia
