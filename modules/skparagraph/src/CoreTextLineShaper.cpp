// Copyright 2026 Google LLC
#include "modules/skparagraph/src/CoreTextLineShaper.h"

#include "include/core/SkFont.h"
#include "include/core/SkPoint.h"
#include "include/core/SkSpan.h"
#include "include/core/SkTypeface.h"
#include "include/private/SkTo.h"
#include "modules/skparagraph/src/CoreTextFontRegistry.h"
#include "modules/skparagraph/src/CoreTextRunSteps.h"
#include "modules/skparagraph/src/CoreTextUtf8.h"
#include "modules/skparagraph/src/ParagraphImpl.h"
#include "modules/skparagraph/src/Run.h"
#include "modules/skshaper/include/SkShaper.h"

#include <memory>
#include <unordered_set>
#include <utility>
#include <vector>

// Paragraph-wide state of CoreText shaping. Each hard-break segment is shaped by a SegmentShaper
// (CoreTextSegmentShaper.cpp), ellipses and hyphens by an EllipsisShaper
// (CoreTextEllipsisShaper.cpp); the CTRun steps both share live in CoreTextRunSteps.cpp
namespace skia {
namespace textlayout {

CoreTextLineShaper::CoreTextLineShaper(ParagraphImpl* paragraph)
        : fParagraph(paragraph), fRequestedFonts(paragraph->fontCollection()) {}

bool CoreTextLineShaper::shape() {
    fUnresolvedGlyphs = 0;
    fAdvanceX = 0;
    // Release the set, do not clear it: a ParagraphCache entry may share it
    fParagraph->fUnresolvedCodepoints.reset();
    fParagraph->fFontSwitches.clear();
    fParagraph->fRuns.clear();
    if (fParagraph->text().size() == 0) {
        return false;
    }

    // ParagraphBuilderImpl::Build always appends a trailing zero-width sentinel placeholder
    SkASSERT(!fParagraph->fPlaceholders.empty());

    // Same as OneLineShaper, which shapes text only inside BiDi regions: when SkUnicode rejects
    // the text (malformed UTF-8) there are none, and only the placeholders get laid out
    if (fParagraph->fBidiRegions.empty()) {
        for (int i = 0; i < fParagraph->fPlaceholders.size(); ++i) {
            if (fParagraph->fPlaceholders[i].fRange.width() > 0) {
                this->appendPlaceholderRun(fParagraph->fPlaceholders[i], SkToSizeT(i));
            }
        }
        return true;
    }

    // Each placeholder's U+FFFC stays in its segment's CTLine, as an inline attachment does
    // natively, so neutrals beside it resolve against the text across it as in fBidiRegions;
    // SegmentShaper lays out the placeholder in place of the U+FFFC CTRun
    fNextPlaceholder = 0;
    this->shapeTextRange(TextRange(0, fParagraph->text().size()));
    // Only the zero-width sentinel, which has no U+FFFC, is left
    SkASSERT(fNextPlaceholder == SkToSizeT(fParagraph->fPlaceholders.size() - 1));
    return true;
}

void CoreTextLineShaper::appendPlaceholderRun(const Placeholder& placeholder,
                                              size_t placeholderIndex) {
    // With empty fBidiRegions (getBidiRegions failed) an RTL paragraph keeps level 1 so its
    // placeholders still run right to left; LTR keeps OneLineShaper's default level 2
    const bool rtl = fParagraph->paragraphStyle().getTextDirection() == TextDirection::kRtl;
    const uint8_t placeholderLevel =
            find_bidi_level(fParagraph->fBidiRegions, placeholder.fRange.start, rtl ? 1 : 2);

    std::vector<sk_sp<SkTypeface>> typefaces = CoreTextFontRegistry::Instance().resolveTypefaces(
            *fParagraph->fFontCollection, placeholder.fTextStyle);
    sk_sp<SkTypeface> typeface = typefaces.empty() ? nullptr : std::move(typefaces.front());
    SkFont font(std::move(typeface), placeholder.fTextStyle.getFontSize());
    apply_font_style_props(&font, placeholder.fTextStyle, /*fakeMissingFontStyles=*/false);

    const SkShaper::RunHandler::RunInfo runInfo = {
            font,
            placeholderLevel,
            0,
            "",
            SkPoint::Make(placeholder.fStyle.fWidth, placeholder.fStyle.fHeight),
            1,
            SkShaper::RunHandler::Range(0, placeholder.fRange.width())};

    auto& run = fParagraph->fRuns.emplace_back(fParagraph,
                                               runInfo,
                                               placeholder.fRange.start,
                                               0.0f,
                                               false,
                                               0.0f,
                                               fParagraph->fRuns.size(),
                                               fAdvanceX);

    run.allocateGlyphData(1, /*hasOffsets=*/false);
    run.fGlyphData->glyphs()[0] = 0;
    run.fGlyphData->positions()[0] = {fAdvanceX, 0};
    // CoreText runs keep the end sentinel on the baseline
    run.fGlyphData->positions()[1] = {fAdvanceX + placeholder.fStyle.fWidth, 0};
    run.fGlyphData->clusterIndexes()[0] = 0;
    run.fPlaceholderIndex = placeholderIndex;
    fAdvanceX += placeholder.fStyle.fWidth;
}

void CoreTextLineShaper::recordUnresolvedGlyph(TextRange glyphRange) {
    const char* textStart = fParagraph->text().data();
    const char* p = textStart + glyphRange.start;
    const char* pEnd = textStart + glyphRange.end;
    SkUnicode* unicode = fParagraph->fUnicode.get();
    bool hasNonControl = false;
    while (p < pEnd) {
        const size_t bytePos = SkToSizeT(p - textStart);
        const SkUnichar u = next_utf8_scalar(&p, pEnd, -1);
        if (u < 0) {
            break;
        }
        // Skip controls and Default_Ignorable code points, same as HarfBuzz OneLineShaper; marks
        // count, so a mark no font has is reported. kControl is precomputed by ParagraphImpl
        if (!fParagraph->codeUnitHasProperty(bytePos, SkUnicode::CodeUnitFlags::kControl) &&
            !unicode->isDefaultIgnorable(u)) {
            hasNonControl = true;
            if (fParagraph->fUnresolvedCodepoints == nullptr) {
                fParagraph->fUnresolvedCodepoints =
                        std::make_shared<std::unordered_set<SkUnichar>>();
            }
            // No cache entry may see the set change
            SkASSERT(fParagraph->fUnresolvedCodepoints.use_count() == 1);
            fParagraph->fUnresolvedCodepoints->emplace(u);
        }
    }
    if (hasNonControl) {
        ++fUnresolvedGlyphs;
    }
}

void CoreTextLineShaper::LetterSpaceRuns(SkSpan<Run> runs, SkScalar letterSpacing) {
    // The only caller spaces runs only when shaping produced some
    SkASSERT(!runs.empty());
    SkScalar shift = 0;
    if (runs[0].leftToRight()) {
        // Carry the accumulated spacing shift across hard-break runs in text order
        for (Run& run : runs) {
            shift = run.addLetterSpacesAfterGraphemes(letterSpacing, shift);
        }
        return;
    }
    // HarfBuzz shapes a single-run RTL paragraph from the last line to the first;
    // rebase the visual pen and space segments in reverse order to match its floats
    SkScalar penX = 0;
    for (int i = SkToInt(runs.size()) - 1; i >= 0; --i) {
        Run& run = runs[i];
        const SkScalar delta = penX - run.posX(0);
        for (size_t k = 0; k <= run.size(); ++k) {
            run.addX(k, delta);
        }
        run.fOffset.fX = penX;
        penX = run.posX(run.size());
        shift = run.addLetterSpacesAfterGraphemes(letterSpacing, shift);
    }
}

}  // namespace textlayout
}  // namespace skia
