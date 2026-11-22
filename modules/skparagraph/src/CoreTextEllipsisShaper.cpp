// Copyright 2026 Google LLC
#include "modules/skparagraph/src/CoreTextLineShaper.h"

#include "include/core/SkFont.h"
#include "include/core/SkPoint.h"
#include "include/core/SkString.h"
#include "include/core/SkTypeface.h"
#include "include/ports/SkTypeface_mac.h"
#include "include/private/SkTemplates.h"
#include "include/private/SkTo.h"
#include "modules/skparagraph/include/FontCollection.h"
#include "modules/skparagraph/src/CoreTextAttrStringBuilder.h"
#include "modules/skparagraph/src/CoreTextFontRegistry.h"
#include "modules/skparagraph/src/CoreTextRunSteps.h"
#include "modules/skparagraph/src/ParagraphImpl.h"
#include "modules/skparagraph/src/Run.h"
#include "modules/skshaper/include/SkShaper.h"
#include "src/utils/mac/SkUniqueCFRef.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

// The ellipsis pipeline: typeset the ellipsis or hyphen in one CTLine, retry with the cluster
// typeface when that resolves more text, then coalesce every CTRun into one Run drawn with the
// primary CTRun's font
namespace skia {
namespace textlayout {

namespace {

// Returns true when ctFont draws the same face as primary. A cursive CTRun of a letter-spaced
// affix gets the primary face without liga=0, and CFEqual on CTFontRef also compares feature
// settings, so the glyph IDs are checked against the underlying CGFont as well
bool shares_primary_face(CTFontRef ctFont, CTFontRef primary) {
    if (CFEqual(ctFont, primary)) {
        return true;
    }
    SkUniqueCFRef<CGFontRef> face(CTFontCopyGraphicsFont(ctFont, nullptr));
    SkUniqueCFRef<CGFontRef> primaryFace(CTFontCopyGraphicsFont(primary, nullptr));
    return face && primaryFace && CFEqual(face.get(), primaryFace.get());
}

}  // namespace

// Shapes one ellipsis or hyphen for CoreTextLineShaper::ShapeEllipsis. The style, allowed faces
// and primary typeface change only when the retry with the cluster typeface resolves more text
class CoreTextLineShaper::EllipsisShaper {
public:
    EllipsisShaper(std::string_view text,
                   const TextStyle& style,
                   sk_sp<SkTypeface> clusterTypeface,
                   uint8_t bidiLevel,
                   sk_sp<FontCollection> fontCollection,
                   SkUnicode* unicode);

    // Typesets the ellipsis (retrying with the cluster typeface) into one Run owned by owner,
    // with clusterRun's line height, half leading and baseline shift
    std::unique_ptr<Run> shape(ParagraphImpl* owner, const Run& clusterRun);

private:
    // Restyles all of fBuilder's text with style alone and typesets it as a single-span ellipsis
    // CTLine
    SkUniqueCFRef<CTLineRef> typeset(const TextStyle& style);

    // Reports whether ctFont is LastResort or an unrequested fallback when fallback is disabled;
    // a non-null extraAllowed counts as one more allowed face
    bool isDisallowed(CTFontRef ctFont, const SkTypeface* extraAllowed = nullptr) const;

    // Returns the first non-empty CTRun whose font is allowed, else the first non-empty CTRun; the
    // ellipsis's single Run takes its font. Requires a line with at least one glyph
    CTRunRef selectPrimary(CFArrayRef runs, const SkTypeface* extraAllowed = nullptr) const;

    // The single Run draws every glyph with primaryFont, so glyphs of another face or of a
    // disallowed font are marked unresolved. A disallowed primary means every CTRun is disallowed
    // (selectPrimary prefers an allowed one), so isDisallowed(ctFont) covers it
    bool isUnresolved(CTFontRef ctFont,
                      CTFontRef primaryFont,
                      const SkTypeface* extraAllowed = nullptr) const;

    // Counts the UTF-16 units of line whose non-empty CTRuns collectGlyphs would mark unresolved
    // with extraAllowed also allowed
    int countUnresolved(CTLineRef line, const SkTypeface* extraAllowed = nullptr) const;

    // Retypesets line with fClusterTypeface through fBuilder when line has unresolved text, as
    // kDefault retries for data-backed or split faces. Moves fStyle and the front of
    // fAllowedTypefaces to fClusterTypeface only when the retry leaves fewer unresolved UTF-16
    // units
    SkUniqueCFRef<CTLineRef> retryWithClusterTypeface(SkUniqueCFRef<CTLineRef> line);

    // Writes the glyphs, pen positions, draw offsets and UTF-8 clusters of all non-empty CTRuns
    // of runs into run, sized to their glyph count, zeroing glyph IDs of the CTRuns with
    // unresolved fonts; returns the summed advance
    SkScalar collectGlyphs(CFArrayRef runs, CTFontRef primaryFont, Run* run) const;

    const std::string_view fText;
    const uint8_t fBidiLevel;
    const sk_sp<FontCollection> fFontCollection;
    const sk_sp<SkTypeface> fClusterTypeface;
    TextStyle fStyle;
    // Its front is the face the Run falls back to for unresolved glyphs
    std::vector<sk_sp<SkTypeface>> fAllowedTypefaces;
    // Its index map and scripts depend only on fText, so they stay valid when the retry restyles
    // it and replaces the line
    CoreTextAttrStringBuilder fBuilder;
};

std::unique_ptr<Run> CoreTextLineShaper::ShapeEllipsis(ParagraphImpl* owner,
                                                       std::string_view text,
                                                       const TextStyle& style,
                                                       const Run& clusterRun,
                                                       uint8_t bidiLevel) {
    SkASSERT(owner);
    // TextLine shapes an ellipsis only when the style is ellipsized, which needs a non-empty
    // string, and every paragraph owns a font collection and a unicode
    SkASSERT(!text.empty());
    SkASSERT(owner->fontCollection());
    SkASSERT(owner->getUnicode());
    return EllipsisShaper(text,
                          style,
                          clusterRun.font().refTypeface(),
                          bidiLevel,
                          owner->fontCollection(),
                          owner->getUnicode().get())
            .shape(owner, clusterRun);
}

CoreTextLineShaper::EllipsisShaper::EllipsisShaper(std::string_view text,
                                                   const TextStyle& style,
                                                   sk_sp<SkTypeface> clusterTypeface,
                                                   uint8_t bidiLevel,
                                                   sk_sp<FontCollection> fontCollection,
                                                   SkUnicode* unicode)
        : fText(text)
        , fBidiLevel(bidiLevel)
        , fFontCollection(std::move(fontCollection))
        , fClusterTypeface(std::move(clusterTypeface))
        , fStyle(style)
        , fBuilder(text, (bidiLevel & 1) ? TextDirection::kRtl : TextDirection::kLtr, unicode) {
    // Inherit the line's resolved cluster typeface when the ellipsis style does not specify an
    // explicit typeface or font family list
    if (!fStyle.getTypeface() && fStyle.getFontFamilies().empty() && fClusterTypeface) {
        fStyle.setTypeface(fClusterTypeface);
        // Same as the retry: the cluster face is already varied and must not be cloned again
        fStyle.setFontArguments(std::nullopt);
    }
    fAllowedTypefaces = CoreTextFontRegistry::Instance().resolveTypefaces(*fFontCollection, fStyle);
}

std::unique_ptr<Run> CoreTextLineShaper::EllipsisShaper::shape(ParagraphImpl* owner,
                                                               const Run& clusterRun) {
    SkUniqueCFRef<CTLineRef> line = this->retryWithClusterTypeface(this->typeset(fStyle));
    CFArrayRef runs = CTLineGetGlyphRuns(line.get());
    CTRunRef primaryRun = this->selectPrimary(runs);
    CTFontRef primaryFont = ct_run_font(primaryRun);
    const size_t glyphCount = SkToSizeT(CTLineGetGlyphCount(line.get()));
    const uint8_t level = reconcile_bidi_level(fBidiLevel, primaryRun);

    // A disallowed primary draws .notdef boxes with the style's primary face
    sk_sp<SkTypeface> typeface =
            this->isDisallowed(primaryFont)
                    ? (fAllowedTypefaces.empty() ? nullptr : fAllowedTypefaces.front())
                    : SkMakeTypefaceFromCTFont(primaryFont);
    const SkFont font = make_run_font(std::move(typeface),
                                      primaryFont,
                                      fStyle,
                                      owner->paragraphStyle().fakeMissingFontStyles());
    const SkString language = fStyle.getLocale();
    // The width comes once the glyphs are measured; Run's constructor does not read it
    const SkShaper::RunHandler::RunInfo info = {font,
                                                level,
                                                fBuilder.scriptAt(0),
                                                language.c_str(),
                                                SkVector::Make(0, 0),
                                                glyphCount,
                                                SkShaper::RunHandler::Range(0, fText.size())};
    auto run = std::make_unique<Run>(owner,
                                     info,
                                     0,
                                     clusterRun.heightMultiplier(),
                                     clusterRun.useHalfLeading(),
                                     clusterRun.baselineShift(),
                                     0,
                                     0);
    // Also writes the cluster index sentinel; the end position needs the width below
    run->allocateGlyphData(glyphCount, /*hasOffsets=*/false);
    const SkScalar width = this->collectGlyphs(runs, primaryFont, run.get());
    normalize_cluster_indices({run->fGlyphData->clusterIndexes(), glyphCount}, level % 2 == 0);
    run->fGlyphData->positions()[glyphCount] = SkPoint::Make(width, 0);
    run->setWidth(width);
    run->setHeight(run->advance().fY);
    run->fEllipsis = true;
    return run;
}

SkUniqueCFRef<CTLineRef> CoreTextLineShaper::EllipsisShaper::typeset(const TextStyle& style) {
    fBuilder.clearStyles();
    fBuilder.addStyle(TextRange(0, fText.size()), style);
    CoreTextRequestedFonts fonts(fFontCollection);
    SkUniqueCFRef<CFAttributedStringRef> attrStr = fBuilder.build(&fonts);
    return typeset_line(attrStr.get());
}

bool CoreTextLineShaper::EllipsisShaper::isDisallowed(CTFontRef ctFont,
                                                      const SkTypeface* extraAllowed) const {
    return is_last_resort_font(ctFont) ||
           (!fFontCollection->fontFallbackEnabled() &&
            !is_allowed_font(ctFont, fAllowedTypefaces, fStyle.getFontFamilies(), extraAllowed));
}

CTRunRef CoreTextLineShaper::EllipsisShaper::selectPrimary(CFArrayRef runs,
                                                           const SkTypeface* extraAllowed) const {
    // CTLineGetGlyphRuns is declared nonnull and shape() passes a typeset line
    SkASSERT(runs);
    // TextLine keeps one Run per ellipsis or hyphen, so multi-run ellipses coalesce into one
    // Run with this CTRun's font and mark glyphs from other fonts unresolved. A disallowed
    // primary (LastResort, or a system font hidden by disabled fallback) zeroes every glyph of
    // the buffer, so the first allowed CTRun takes its place
    CTRunRef firstRun = nullptr;
    const CFIndex runCount = CFArrayGetCount(runs);
    for (CFIndex i = 0; i < runCount; ++i) {
        CTRunRef ctRun = (CTRunRef)CFArrayGetValueAtIndex(runs, i);
        if (CTRunGetGlyphCount(ctRun) <= 0) {
            continue;
        }
        if (!this->isDisallowed(ct_run_font(ctRun), extraAllowed)) {
            return ctRun;
        }
        firstRun = firstRun ? firstRun : ctRun;
    }
    SkASSERT(firstRun);
    return firstRun;
}

bool CoreTextLineShaper::EllipsisShaper::isUnresolved(CTFontRef ctFont,
                                                      CTFontRef primaryFont,
                                                      const SkTypeface* extraAllowed) const {
    return this->isDisallowed(ctFont, extraAllowed) || !shares_primary_face(ctFont, primaryFont);
}

int CoreTextLineShaper::EllipsisShaper::countUnresolved(CTLineRef line,
                                                        const SkTypeface* extraAllowed) const {
    CFArrayRef runs = CTLineGetGlyphRuns(line);
    CTFontRef primaryFont = ct_run_font(this->selectPrimary(runs, extraAllowed));
    int count = 0;
    const CFIndex runCount = CFArrayGetCount(runs);
    for (CFIndex i = 0; i < runCount; ++i) {
        CTRunRef ctRun = (CTRunRef)CFArrayGetValueAtIndex(runs, i);
        if (CTRunGetGlyphCount(ctRun) > 0 &&
            this->isUnresolved(ct_run_font(ctRun), primaryFont, extraAllowed)) {
            count += SkToInt(CTRunGetStringRange(ctRun).length);
        }
    }
    return count;
}

SkUniqueCFRef<CTLineRef> CoreTextLineShaper::EllipsisShaper::retryWithClusterTypeface(
        SkUniqueCFRef<CTLineRef> line) {
    if (!fClusterTypeface || fStyle.getTypeface() == fClusterTypeface.get()) {
        return line;
    }
    const int unresolved = this->countUnresolved(line.get());
    if (unresolved == 0) {
        return line;
    }
    TextStyle retryStyle = fStyle;
    retryStyle.setTypeface(fClusterTypeface);
    // The resolved cluster face already carries the style's variation; keeping the arguments would
    // clone it a second time
    retryStyle.setFontArguments(std::nullopt);
    SkUniqueCFRef<CTLineRef> retry = this->typeset(retryStyle);
    // Allow fClusterTypeface only when scoring retry, not line, so a stand-in system font on line
    // cannot match fClusterTypeface's design upfront and suppress the retry. A retry that
    // resolves no more text keeps the first face, which kDefault also tries last
    if (this->countUnresolved(retry.get(), fClusterTypeface.get()) >= unresolved) {
        return line;
    }
    fAllowedTypefaces.insert(fAllowedTypefaces.begin(), fClusterTypeface);
    fStyle = std::move(retryStyle);
    return retry;
}

SkScalar CoreTextLineShaper::EllipsisShaper::collectGlyphs(CFArrayRef runs,
                                                           CTFontRef primaryFont,
                                                           Run* run) const {
    SkASSERT(run);
    SkASSERT(runs);
    const int glyphCount = SkToInt(run->size());
    SkGlyphID* glyphs = run->fGlyphData->glyphs();
    uint32_t* clusters = run->fGlyphData->clusterIndexes();
    Run::ShapingBuffer buffer;
    buffer.fOffsets.resize_back(glyphCount);
    skia_private::AutoSTArray<16, SkScalar> advances(glyphCount);
    const bool zeroSize = has_non_positive_font_size(fStyle);
    const bool hasTab = memchr(fText.data(), '\t', fText.size()) != nullptr;
    const CFIndex runCount = CFArrayGetCount(runs);
    int runStart = 0;
    for (CFIndex i = 0; i < runCount; ++i) {
        CTRunRef ctRun = (CTRunRef)CFArrayGetValueAtIndex(runs, i);
        const int gc = SkToInt(CTRunGetGlyphCount(ctRun));
        if (gc <= 0) {
            continue;
        }
        const size_t n = SkToSizeT(gc);
        CTFontRef ctFont = ct_run_font(ctRun);
        // Left-to-right subpixel runs without tabs split pair kerning as paragraph runs do
        const bool splitKerning = !zeroSize && fStyle.getSubpixel() && !hasTab &&
                                  (CTRunGetStatus(ctRun) & kCTRunStatusRightToLeft) == 0;
        extract_ctrun_glyphs(ctRun,
                             ctFont,
                             advance_mode(zeroSize, fStyle.getSubpixel(), splitKerning),
                             {glyphs + runStart, n},
                             SkSpan(buffer.fOffsets).subspan(runStart, n),
                             SkSpan(advances.data(), glyphCount).subspan(runStart, n));
        if (this->isUnresolved(ctFont, primaryFont)) {
            // Keep kCGFontIndexInvalid on omitted Default_Ignorable and control slots so the pen
            // loop below maps them to an invisible space glyph instead of .notdef
            std::replace_if(
                    glyphs + runStart,
                    glyphs + runStart + gc,
                    [](CGGlyph g) { return g != kCGFontIndexInvalid; },
                    CGGlyph(0));
        }
        skia_private::AutoSTArray<16, CFIndex> indices(gc);
        CTRunGetStringIndices(ctRun, CFRangeMake(0, gc), indices.data());
        for (int k = 0; k < gc; ++k) {
            clusters[runStart + k] = SkToU32(fBuilder.indexMap().toUtf8Index(indices[k]));
        }
        runStart += gc;
    }

    // Same as paragraph runs: an ellipsis slot CoreText dropped (Default_Ignorable or control)
    // draws the primary font's space glyph, never a .notdef box, and the pen walks the base
    // advances with each origin apart in the offset, as SkShaper's HarfBuzz runs do
    const SkGlyphID invisibleGlyph = run->font().unicharToGlyph(' ');
    SkScalar penX = 0;
    for (int g = 0; g < glyphCount; ++g) {
        glyphs[g] = glyphs[g] == kCGFontIndexInvalid ? invisibleGlyph : glyphs[g];
        run->fGlyphData->positions()[g] = SkPoint::Make(penX, 0);
        penX += advances[g];
    }
    run->commit(buffer);
    return penX;
}

}  // namespace textlayout
}  // namespace skia
