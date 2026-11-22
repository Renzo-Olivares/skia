// Copyright 2026 Google LLC
#include "modules/skparagraph/src/CoreTextLineShaper.h"

#include "include/core/SkFont.h"
#include "include/core/SkFontMetrics.h"
#include "include/core/SkPoint.h"
#include "include/core/SkSpan.h"
#include "include/core/SkString.h"
#include "include/core/SkTypeface.h"
#include "include/ports/SkTypeface_mac.h"
#include "include/private/SkTArray.h"
#include "include/private/SkTemplates.h"
#include "include/private/SkTo.h"
#include "modules/skparagraph/include/FontCollection.h"
#include "modules/skparagraph/src/CoreTextAttrStringBuilder.h"
#include "modules/skparagraph/src/CoreTextFontRegistry.h"
#include "modules/skparagraph/src/CoreTextRunSteps.h"
#include "modules/skparagraph/src/CoreTextUtf8.h"
#include "modules/skparagraph/src/ParagraphImpl.h"
#include "modules/skparagraph/src/Run.h"
#include "modules/skshaper/include/SkShaper.h"
#include "src/core/SkTHash.h"
#include "src/utils/mac/SkUniqueCFRef.h"

#include <algorithm>
#include <cstring>
#include <optional>
#include <string_view>
#include <vector>

// The paragraph pipeline: each hard-break segment is styled block by block into one attributed
// string and typeset as one CTLine; its CTRuns, in logical order, become Runs (or placeholder
// runs) with fonts and BiDi levels picked per style Block, and a split trailing '\n' CTRun folds
// into the Run before it
namespace skia {
namespace textlayout {

namespace {

// Returns the style Block covering utf8Pos in sorted textStyles, or nullptr in unstyled gaps
const Block* find_style_block(SkSpan<const Block> textStyles, size_t utf8Pos) {
    // fTextStyles is sorted by start, so the candidate is the last block starting at or before
    // utf8Pos; a position in a gap between blocks keeps the paragraph default style
    const Block* nextBlock = std::upper_bound(
            textStyles.begin(), textStyles.end(), utf8Pos, [](size_t pos, const Block& block) {
                return pos < block.fRange.start;
            });
    if (nextBlock != textStyles.begin() && utf8Pos < nextBlock[-1].fRange.end) {
        return nextBlock - 1;
    }
    return nullptr;
}

// Returns glyph k's paragraph-global UTF-8 text range from run's normalized clusters
TextRange glyph_text_range(const Run& run, size_t k) {
    const TextRange runRange = run.textRange();
    const size_t cStart = run.clusterIndex(k);
    const size_t cEnd =
            run.leftToRight() ? ((k + 1 < run.size()) ? run.clusterIndex(k + 1) : runRange.width())
                              : ((k > 0) ? run.clusterIndex(k - 1) : runRange.width());
    // buildRunClusters normalizes the clusters to text order and keeps each below the width
    SkASSERT(cStart <= cEnd);
    return TextRange(runRange.start + cStart, runRange.start + cEnd);
}

// Returns true for the C0/C1 controls with Bidi_Class BN, NUL included, which a native CTLine
// drops without a glyph or advance; tab, the separators U+001C..U+001F and U+0085 are not BN
bool is_coretext_omitted_control(SkUnichar u) {
    return (u >= 0x0000 && u <= 0x0008) || (u >= 0x000E && u <= 0x001B) ||
           (u >= 0x007F && u <= 0x0084) || (u >= 0x0086 && u <= 0x009F);
}

}  // namespace

// Shapes one hard-break segment of the owner's paragraph in one CTLine. The owner keeps what all
// segments share (pen, placeholder cursor, unresolved glyphs, span and run font caches); the
// attributed string, CTLine, style-block fonts and shaping buffer here live for one segment
class CoreTextLineShaper::SegmentShaper {
public:
    // Styles and typesets the segment; shape() then walks the CTRuns of its CTLine
    SegmentShaper(CoreTextLineShaper* owner, TextRange segmentRange);

    // Appends the segment's Runs and font switches (a split trailing '\n' CTRun folds into the run
    // before it) and advances the owner's pen
    void shape();

private:
    // How shapeRun repairs a CTRun around its segment's only '\n'. Pinning needs the run itself to
    // be that '\n' and coalescing needs a '\n' CTRun after it, so one run gets at most one repair
    enum class NewlineFix { kNone, kPinnedStandalone, kCoalescedTrailing };

    // Font, BiDi level and '\n' repair chosen for one CTRun before its Run exists; utf8Range is
    // paragraph-global and also covers a folded trailing '\n'
    struct RunChoice {
        CTFontRef ctFont = nullptr;
        bool isUnresolved = false;
        NewlineFix fix = NewlineFix::kNone;
        // The glyph the fix puts in the '\n' slot: .notdef for a pinned one
        CGGlyph newlineGlyph = 0;
        uint8_t bidiLevel = 0;
        TextRange utf8Range;
    };

    // Where a Run of ctCount CTRun glyphs plus fix's '\n' keeps them: RTL runs store glyphs in
    // visual order, so a folded logical-end '\n' takes visual slot 0 and shifts the CTRun glyphs
    struct GlyphSlots {
        size_t ctStart;
        size_t newline;
    };
    static GlyphSlots NewlineGlyphSlots(NewlineFix fix, bool ltr, size_t ctCount);

    // The faces resolved for one style Block, with its allowlist verdicts memoized per CTFont
    // across neighboring CTRuns in the Block
    class BlockFonts {
    public:
        BlockFonts(const Block* block, const TextStyle& style, FontCollection* fonts)
                : fBlock(block)
                , fStyle(style)
                // Same clone the attributed string was built from, so LastResort runs and the
                // allowed-font check see the varied face (or default .AppleSystemUIFont) rather
                // than a null typeface
                , fAllowedTypefaces(
                          CoreTextFontRegistry::Instance().resolveTypefaces(*fonts, style))
                , fLanguage(style.getLocale()) {}

        // Reports whether ctFont is one of the Block's faces or requested families
        bool isAllowed(CTFontRef ctFont) {
            bool* allowed = fAllowedFontCache.find(ctFont);
            if (!allowed) {
                allowed = fAllowedFontCache.set(
                        ctFont,
                        is_allowed_font(ctFont, fAllowedTypefaces, fStyle.getFontFamilies()));
            }
            return *allowed;
        }

        // Null for an unstyled gap, which takes the paragraph default style
        const Block* const fBlock;
        const TextStyle& fStyle;
        const std::vector<sk_sp<SkTypeface>> fAllowedTypefaces;
        const SkString fLanguage;

    private:
        skia_private::THashMap<CTFontRef, bool> fAllowedFontCache;
    };

    // Styles the segment with each intersecting Block, filling unstyled gaps from the paragraph
    // default TextStyle so CoreText never falls back to 12pt Helvetica
    SkUniqueCFRef<CFAttributedStringRef> buildAttrString();

    // Collects non-empty CTRuns from line and sorts them into ascending UTF-16 string order while
    // preserving CoreText's relative order for runs sharing a start index
    static skia_private::STArray<16, CTRunRef> CollectSortedRuns(CTLineRef line);

    // Appends the Run for the CTRun at runIdx, folding in the split trailing '\n' CTRun after it
    // when present, and advances the owner's pen. Returns the CTRuns consumed, 1 or 2
    int shapeRun(int runIdx);

    // Picks the font (a pinned standalone '\n', disallowed fallback) and BiDi level of the
    // non-empty CTRun at runIdx in the entered Block, folding in a trailing '\n' CTRun after it
    // when present
    RunChoice chooseRunFont(int runIdx, TextRange runUtf8Range);

    // Returns the BiDi level covering utf8Pos (the paragraph direction's level in a gap) with its
    // parity reconciled to ctRun's shaped direction
    uint8_t ctRunLevel(size_t utf8Pos, CTRunRef ctRun) const;

    // Makes fBlockFonts cover block (nullptr for an unstyled gap), keeping it while neighboring
    // CTRuns stay in the same Block, and returns it
    BlockFonts& enterBlock(const Block* block);

    // Reports whether ctFont is LastResort or an unrequested fallback when font fallback is
    // disabled, memoizing LastResort per paragraph and allowlist checks per active Block
    bool isDisallowedRunFont(CTFontRef ctFont);

    // Returns the owner's resolved font entry for ctFont, adding one on first sight. Never hold the
    // result across another call: inserting can rehash the map and move every entry
    CoreTextBoundFont* boundFont(CTFontRef ctFont);

    // Returns ctFont's SkTypeface, made on the first call per CTFont and kept in its bound entry
    sk_sp<SkTypeface> boundTypeface(CTFontRef ctFont);

    // Paragraph-global UTF-8 range or index of a UTF-16 range or index in fAttrStr
    TextRange paragraphRange(CFRange utf16Range) const;
    size_t paragraphIndex(CFIndex utf16Index) const;

    // CoreText cascades U+000A to a fallback font when the primary face lacks a newline glyph.
    // Returns the block's primary CTFont to keep a standalone '\n' on as glyph 0, so empty-line
    // metrics and font equality match the primary face, or nullptr when no pin applies
    CTFontRef tryPinStandaloneNewline(TextRange runUtf8Range, CFIndex utf16Loc);

    // Reads kCTFontAttributeName at utf16Loc from fAttrStr to preserve exact descriptor attributes
    // (such as liga=0 or variations) for SkTypeface equality
    CTFontRef attrPrimaryCtFont(CFIndex utf16Loc) const;

    // CoreText splits a trailing '\n' into a separate CTRun when the primary face lacks U+000A or
    // on RTL lines. When the CTRun after runIdx is that '\n', returns ctFont's glyph for it (0 when
    // unmapped) so the caller folds it into this run and line-end caret hit-testing and multi-line
    // spacing see one run per segment; std::nullopt otherwise
    std::optional<CGGlyph> coalesceTrailingNewline(int runIdx,
                                                   CTFontRef ctFont,
                                                   uint8_t bidiLevel,
                                                   TextRange runUtf8Range) const;

    // Appends the Run for the CTRun at runIdx and choice at the owner's pen with block's style and
    // its glyph storage and clusters; the glyphs, positions and width come from populateShapedRun
    // or populateUnresolvedRun
    Run* appendRun(int runIdx, const RunChoice& choice, const BlockFonts& block);

    // Maps ctRun's UTF-16 string indices to UTF-8 byte offsets relative to the paragraph-global
    // runStart in clusters and normalizes them to monotonic cluster order
    void buildRunClusters(CTRunRef ctRun, size_t runStart, bool ltr, SkSpan<uint32_t> clusters);

    // Fills run with .notdef boxes for its clusters, records them unresolved, returns its width
    SkScalar populateUnresolvedRun(Run* run);

    // Fills run from ctRun and the '\n' fix of choice after its clusters, sized as block's style
    // says, commits it, returns its width
    SkScalar populateShapedRun(Run* run,
                               CTRunRef ctRun,
                               const RunChoice& choice,
                               const BlockFonts& block);

    CoreTextLineShaper* const fOwner;
    ParagraphImpl* const fParagraph;
    const SkSpan<const char> fText;
    const TextRange fSegmentRange;
    // Initialized before fAttrStr, which buildAttrString styles it into
    CoreTextAttrStringBuilder fBuilder;
    const SkUniqueCFRef<CFAttributedStringRef> fAttrStr;
    // Owns the CTRuns of fSortedRuns
    const SkUniqueCFRef<CTLineRef> fLine;
    const skia_private::STArray<16, CTRunRef> fSortedRuns;
    // Empty until the first CTRun picks its Block
    std::optional<BlockFonts> fBlockFonts;
    // Run::commit keeps the glyph offsets only when one is not zero, as for SkShaper runs
    Run::ShapingBuffer fShapingBuffer;
};

void CoreTextLineShaper::shapeTextRange(TextRange range) {
    // shape() returns early on empty text
    SkASSERT(range.width() > 0);

    const char* text = fParagraph->text().data();
    size_t segStart = range.start;

    while (segStart < range.end) {
        // The '\n' ends its segment, so each CTLine holds one hard-break paragraph for BiDi and
        // typesetting
        size_t segEnd = segStart;
        while (segEnd < range.end && text[segEnd++] != '\n') {
        }

        SegmentShaper(this, TextRange(segStart, segEnd)).shape();
        segStart = segEnd;
    }
}

CoreTextLineShaper::SegmentShaper::SegmentShaper(CoreTextLineShaper* owner, TextRange segmentRange)
        : fOwner(owner)
        , fParagraph(owner->fParagraph)
        , fText(fParagraph->text())
        , fSegmentRange(segmentRange)
        , fBuilder(std::string_view(fText.data() + segmentRange.start, segmentRange.width()),
                   fParagraph->paragraphStyle().getTextDirection(),
                   fParagraph->getUnicode().get())
        , fAttrStr(this->buildAttrString())
        , fLine(typeset_line(fAttrStr.get()))
        , fSortedRuns(CollectSortedRuns(fLine.get())) {
    // shapeTextRange advances segEnd past at least one byte before making a SegmentShaper
    SkASSERT(segmentRange.width() > 0);
    // next_utf8_scalar turns every byte, even a malformed one, into at least one UTF-16 unit
    SkASSERT(CFAttributedStringGetLength(fAttrStr.get()) > 0);
}

void CoreTextLineShaper::SegmentShaper::shape() {
    // shapeRun consumes one or two CTRuns, so the loop always moves forward
    for (int runIdx = 0; runIdx < fSortedRuns.size();) {
        runIdx += this->shapeRun(runIdx);
    }
}

SkUniqueCFRef<CFAttributedStringRef> CoreTextLineShaper::SegmentShaper::buildAttrString() {
    const TextStyle& defaultStyle = fParagraph->paragraphStyle().getTextStyle();
    size_t cursor = fSegmentRange.start;
    const BlockRange blockRange = fParagraph->findAllBlocks(fSegmentRange);
    if (!blockRange.empty()) {
        for (const auto& block : fParagraph->blocks(blockRange)) {
            const size_t start = std::max(block.fRange.start, cursor);
            const size_t end = std::min(block.fRange.end, fSegmentRange.end);
            if (start >= end) {
                continue;
            }
            if (cursor < start) {
                fBuilder.addStyle(
                        TextRange(cursor - fSegmentRange.start, start - fSegmentRange.start),
                        defaultStyle);
            }
            fBuilder.addStyle(TextRange(start - fSegmentRange.start, end - fSegmentRange.start),
                              block.fStyle);
            cursor = end;
        }
    }
    if (cursor < fSegmentRange.end) {
        fBuilder.addStyle(
                TextRange(cursor - fSegmentRange.start, fSegmentRange.end - fSegmentRange.start),
                defaultStyle);
    }
    return fBuilder.build(&fOwner->fRequestedFonts);
}

skia_private::STArray<16, CTRunRef> CoreTextLineShaper::SegmentShaper::CollectSortedRuns(
        CTLineRef line) {
    CFArrayRef runs = CTLineGetGlyphRuns(line);
    // CTLineGetGlyphRuns is declared nonnull; a line without glyphs gives an empty array
    SkASSERT(runs);
    skia_private::STArray<16, CTRunRef> sorted;
    const CFIndex runCount = CFArrayGetCount(runs);
    for (CFIndex i = 0; i < runCount; ++i) {
        CTRunRef ctRun = (CTRunRef)CFArrayGetValueAtIndex(runs, i);
        // Drop empty runs upfront so trailing-newline lookahead sees no empty CTRuns after '\n'
        if (CTRunGetGlyphCount(ctRun) == 0 || CTRunGetStringRange(ctRun).length == 0) {
            continue;
        }
        sorted.push_back(ctRun);
    }
    // CTRuns arrive in visual order from CTLineGetGlyphRuns; logical ascending sort prevents
    // BiDi inversion downstream in cluster layout
    std::stable_sort(sorted.begin(), sorted.end(), [](CTRunRef a, CTRunRef b) {
        return CTRunGetStringRange(a).location < CTRunGetStringRange(b).location;
    });
    return sorted;
}

int CoreTextLineShaper::SegmentShaper::shapeRun(int runIdx) {
    CTRunRef ctRun = fSortedRuns[runIdx];
    const TextRange runUtf8Range = this->paragraphRange(CTRunGetStringRange(ctRun));
    // Skip CTRuns that sit inside a single surrogate pair and map to a zero-byte UTF-8 range
    if (runUtf8Range.width() == 0) {
        return 1;
    }
    const auto& placeholders = fParagraph->fPlaceholders;
    const size_t nextPlaceholder = fOwner->fNextPlaceholder;
    if (placeholders[nextPlaceholder].fRange.start == runUtf8Range.start) {
        // CoreTextAttrStringBuilder never merges a placeholder span, so the U+FFFC is a CTRun of
        // its own; the sentinel at the end of the text starts after every CTRun
        SkASSERT(placeholders[nextPlaceholder].fRange == runUtf8Range);
        fOwner->appendPlaceholderRun(placeholders[nextPlaceholder], nextPlaceholder);
        ++fOwner->fNextPlaceholder;
        return 1;
    }

    const BlockFonts& block =
            this->enterBlock(find_style_block(fParagraph->styles(), runUtf8Range.start));
    const RunChoice choice = this->chooseRunFont(runIdx, runUtf8Range);
    Run* run = this->appendRun(runIdx, choice, block);
    const SkScalar runAdvanceX = choice.isUnresolved
                                         ? this->populateUnresolvedRun(run)
                                         : this->populateShapedRun(run, ctRun, choice, block);
    run->setWidth(runAdvanceX);
    // CoreText runs keep the end sentinel on the baseline, where allocateGlyphData puts it at
    // fOffset + fAdvance. commit() may have moved the glyph data, so read it again
    run->fGlyphData->positions()[run->size()] = SkPoint::Make(fOwner->fAdvanceX + runAdvanceX, 0);
    fParagraph->fFontSwitches.emplace_back(choice.utf8Range.start, run->font());
    fOwner->fAdvanceX += runAdvanceX;
    return choice.fix == NewlineFix::kCoalescedTrailing ? 2 : 1;
}

CoreTextLineShaper::SegmentShaper::RunChoice CoreTextLineShaper::SegmentShaper::chooseRunFont(
        int runIdx, TextRange runUtf8Range) {
    CTRunRef ctRun = fSortedRuns[runIdx];
    RunChoice choice;
    choice.ctFont = ct_run_font(ctRun);
    choice.utf8Range = runUtf8Range;
    choice.isUnresolved = this->isDisallowedRunFont(choice.ctFont);
    if (CTFontRef pinned =
                this->tryPinStandaloneNewline(runUtf8Range, CTRunGetStringRange(ctRun).location)) {
        choice.ctFont = pinned;
        choice.isUnresolved = false;
        choice.fix = NewlineFix::kPinnedStandalone;
    }

    choice.bidiLevel = this->ctRunLevel(runUtf8Range.start, ctRun);
    // Do not fold a trailing '\n' into a disallowed or LastResort run, where
    // populateUnresolvedRun would turn it into a .notdef box and count it as unresolved
    if (choice.isUnresolved) {
        return choice;
    }
    if (std::optional<CGGlyph> coalescedGlyph = this->coalesceTrailingNewline(
                runIdx, choice.ctFont, choice.bidiLevel, runUtf8Range)) {
        // A pinned run is the segment's only '\n', so no trailing '\n' CTRun follows it
        SkASSERT(choice.fix == NewlineFix::kNone);
        choice.fix = NewlineFix::kCoalescedTrailing;
        choice.newlineGlyph = *coalescedGlyph;
        choice.utf8Range.end = fSegmentRange.end;
    }
    return choice;
}

uint8_t CoreTextLineShaper::SegmentShaper::ctRunLevel(size_t utf8Pos, CTRunRef ctRun) const {
    // Level 1 for an RTL paragraph, 0 for LTR
    const uint8_t paragraphLevel =
            fParagraph->paragraphStyle().getTextDirection() == TextDirection::kRtl;
    return reconcile_bidi_level(find_bidi_level(fParagraph->fBidiRegions, utf8Pos, paragraphLevel),
                                ctRun);
}

CoreTextLineShaper::SegmentShaper::BlockFonts& CoreTextLineShaper::SegmentShaper::enterBlock(
        const Block* block) {
    if (fBlockFonts && fBlockFonts->fBlock == block) {
        return *fBlockFonts;
    }
    return fBlockFonts.emplace(block,
                               block ? block->fStyle : fParagraph->paragraphStyle().getTextStyle(),
                               fParagraph->fontCollection().get());
}

bool CoreTextLineShaper::SegmentShaper::isDisallowedRunFont(CTFontRef ctFont) {
    if (this->boundFont(ctFont)->lastResort) {
        return true;
    }
    return !fParagraph->fFontCollection->fontFallbackEnabled() && !fBlockFonts->isAllowed(ctFont);
}

CoreTextBoundFont* CoreTextLineShaper::SegmentShaper::boundFont(CTFontRef ctFont) {
    SkASSERT(ctFont);
    CoreTextBoundFonts& fonts = fOwner->fBoundFonts;
    if (CoreTextBoundFont* entry = fonts.find(ctFont)) {
        return entry;
    }
    return fonts.set(ctFont,
                     {SkUniqueCFRef<CTFontRef>((CTFontRef)CFRetain(ctFont)),
                      is_last_resort_font(ctFont),
                      nullptr});
}

sk_sp<SkTypeface> CoreTextLineShaper::SegmentShaper::boundTypeface(CTFontRef ctFont) {
    sk_sp<SkTypeface>& typeface = this->boundFont(ctFont)->typeface;
    // Memoized: null is legal until the first run that renders with ctFont
    if (!typeface) {
        typeface = SkMakeTypefaceFromCTFont(ctFont);
    }
    return typeface;
}

TextRange CoreTextLineShaper::SegmentShaper::paragraphRange(CFRange utf16Range) const {
    const TextRange range = fBuilder.indexMap().cfrangeToTextRange(utf16Range);
    return TextRange(fSegmentRange.start + range.start, fSegmentRange.start + range.end);
}

size_t CoreTextLineShaper::SegmentShaper::paragraphIndex(CFIndex utf16Index) const {
    return fSegmentRange.start + fBuilder.indexMap().toUtf8Index(utf16Index);
}

CoreTextLineShaper::SegmentShaper::GlyphSlots CoreTextLineShaper::SegmentShaper::NewlineGlyphSlots(
        NewlineFix fix, bool ltr, size_t ctCount) {
    const bool coalesced = fix == NewlineFix::kCoalescedTrailing;
    // A pinned run keeps its CTRun glyphs from slot 0 and its fix overwrites slot 0
    return {coalesced && !ltr ? 1u : 0u, coalesced && ltr ? ctCount : 0u};
}

CTFontRef CoreTextLineShaper::SegmentShaper::tryPinStandaloneNewline(TextRange runUtf8Range,
                                                                     CFIndex utf16Loc) {
    if (runUtf8Range.width() != 1 || fText[runUtf8Range.start] != '\n') {
        return nullptr;
    }
    CTFontRef attrFont = this->attrPrimaryCtFont(utf16Loc);
    const UniChar nlChar = '\n';
    CGGlyph nlGlyph = 0;
    // Only pin when the primary face lacks U+000A (so CoreText cascaded away) and is itself
    // allowed; a face that maps '\n' already kept the run, and disallowed faces stay fallback
    if (CTFontGetGlyphsForCharacters(attrFont, &nlChar, &nlGlyph, 1) ||
        this->isDisallowedRunFont(attrFont)) {
        return nullptr;
    }
    return attrFont;
}

CTFontRef CoreTextLineShaper::SegmentShaper::attrPrimaryCtFont(CFIndex utf16Loc) const {
    CTFontRef font = (CTFontRef)CFAttributedStringGetAttribute(
            fAttrStr.get(), utf16Loc, kCTFontAttributeName, nullptr);
    SkASSERT(font);
    return font;
}

std::optional<CGGlyph> CoreTextLineShaper::SegmentShaper::coalesceTrailingNewline(
        int runIdx, CTFontRef ctFont, uint8_t bidiLevel, TextRange runUtf8Range) const {
    if (runIdx + 2 != fSortedRuns.size() || runUtf8Range.end + 1 != fSegmentRange.end ||
        fText[fSegmentRange.end - 1] != '\n') {
        return std::nullopt;
    }
    const CFRange range = CTRunGetStringRange(fSortedRuns[runIdx]);
    CTRunRef nlRun = fSortedRuns[runIdx + 1];
    const CFRange nlRange = CTRunGetStringRange(nlRun);
    if (nlRange.location != range.location + range.length || nlRange.length != 1 ||
        CTRunGetGlyphCount(nlRun) != 1 ||
        find_style_block(fParagraph->styles(), runUtf8Range.end) != fBlockFonts->fBlock ||
        this->ctRunLevel(runUtf8Range.end, nlRun) != bidiLevel) {
        return std::nullopt;
    }
    // Fold only when the preceding run stayed on the block's primary CTFont; if text before
    // '\n' cascaded to a fallback face, keep '\n' separate so ctFont is not misapplied to it
    CTFontRef nlAttrFont = this->attrPrimaryCtFont(nlRange.location);
    if (!CFEqual(ctFont, nlAttrFont)) {
        return std::nullopt;
    }
    const UniChar nlChar = '\n';
    // CoreText writes glyph 0 for a character the font does not map
    CGGlyph glyph = 0;
    CTFontGetGlyphsForCharacters(ctFont, &nlChar, &glyph, 1);
    return glyph;
}

Run* CoreTextLineShaper::SegmentShaper::appendRun(int runIdx,
                                                  const RunChoice& choice,
                                                  const BlockFonts& block) {
    CTRunRef ctRun = fSortedRuns[runIdx];
    const bool coalesced = choice.fix == NewlineFix::kCoalescedTrailing;
    const bool isLtr = (choice.bidiLevel % 2) == 0;
    const int ctGlyphCount = SkToInt(CTRunGetGlyphCount(ctRun));
    const TextStyle& style = block.fStyle;
    const std::vector<sk_sp<SkTypeface>>& allowed = block.fAllowedTypefaces;
    // An unresolved run draws .notdef boxes with the Block's primary face
    sk_sp<SkTypeface> typeface = choice.isUnresolved ? (allowed.empty() ? nullptr : allowed.front())
                                                     : this->boundTypeface(choice.ctFont);
    const SkFont runFont = make_run_font(std::move(typeface),
                                         choice.ctFont,
                                         style,
                                         fParagraph->paragraphStyle().fakeMissingFontStyles());
    SkFontMetrics fontMetrics;
    runFont.getMetrics(&fontMetrics);
    // The itemization that keeps cursive ranges unspaced also names the run's script, so
    // Run::isCursiveScript agrees with the glyphs CoreText produced. The width and glyph data
    // come once the glyphs are measured; Run's constructor reads neither
    const SkShaper::RunHandler::RunInfo runInfo = {
            runFont,
            choice.bidiLevel,
            fBuilder.scriptAt(choice.utf8Range.start - fSegmentRange.start),
            block.fLanguage.c_str(),
            SkPoint::Make(0, fontMetrics.fDescent - fontMetrics.fAscent + fontMetrics.fLeading),
            0,
            SkShaper::RunHandler::Range(0, choice.utf8Range.width())};
    Run* run = &fParagraph->fRuns.emplace_back(fParagraph,
                                               runInfo,
                                               choice.utf8Range.start,
                                               style.getHeightOverride() ? style.getHeight() : 0.0f,
                                               style.getHalfLeading(),
                                               style.getBaselineShift(),
                                               fParagraph->fRuns.size(),
                                               fOwner->fAdvanceX);
    // Also writes the N+1 sentinel (run end position and cluster index) that line breaking and
    // caret hit-testing interpolate against
    run->allocateGlyphData(SkToSizeT(ctGlyphCount + (coalesced ? 1 : 0)), /*hasOffsets=*/false);
    uint32_t* clusters = run->fGlyphData->clusterIndexes();
    const GlyphSlots slots = NewlineGlyphSlots(choice.fix, isLtr, SkToSizeT(ctGlyphCount));
    this->buildRunClusters(ctRun,
                           choice.utf8Range.start,
                           isLtr,
                           {clusters + slots.ctStart, SkToSizeT(ctGlyphCount)});
    if (coalesced) {
        clusters[slots.newline] = SkToU32(choice.utf8Range.width() - 1);
    }
    return run;
}

void CoreTextLineShaper::SegmentShaper::buildRunClusters(CTRunRef ctRun,
                                                         size_t runStart,
                                                         bool ltr,
                                                         SkSpan<uint32_t> clusters) {
    const int ctGlyphCount = SkToInt(clusters.size());
    skia_private::AutoSTArray<64, CFIndex> indexBuffer(ctGlyphCount);
    CTRunGetStringIndices(ctRun, CFRangeMake(0, ctGlyphCount), indexBuffer.data());
    for (int k = 0; k < ctGlyphCount; ++k) {
        const size_t globalU8 = this->paragraphIndex(indexBuffer[k]);
        // A CTRun's glyphs index only into its own string range, which starts at runStart
        SkASSERT(globalU8 >= runStart);
        clusters[k] = SkToU32(globalU8 - runStart);
    }
    normalize_cluster_indices(clusters, ltr);
}

SkScalar CoreTextLineShaper::SegmentShaper::populateUnresolvedRun(Run* run) {
    SkASSERT(run);
    SkUnicode* unicode = fParagraph->fUnicode.get();
    SkASSERT(unicode);
    const SkFont& font = run->font();
    SkScalar notdefAdvance = font.getWidth(0);
    // Fonts whose .notdef glyph has zero hmtx advance fall back to 1em so unresolved boxes stay
    // visible and distinct instead of collapsing onto one x position
    if (SkScalarNearlyZero(notdefAdvance)) {
        notdefAdvance = font.getSize();
    }
    // .notdef gets the same advance as any other glyph, whole pixels included
    if (!font.isSubpixel()) {
        notdefAdvance = SkScalarRoundToScalar(notdefAdvance);
    }

    // CoreText renders missing glyphs through LastResort; remap to glyph 0 (.notdef) to honor
    // SkParagraph unresolved contract. Invisible clusters take the primary font's space glyph, as
    // in populateShapedRun, so they paint no box over the next run
    const SkGlyphID invisibleGlyph = font.unicharToGlyph(' ');
    const char* text = fText.data();
    SkScalar penX = 0;
    for (size_t k = 0; k < run->size(); ++k) {
        const TextRange gRange = glyph_text_range(*run, k);
        const char* p = text + gRange.start;
        const char* pEnd = text + gRange.end;
        int visibleCount = 0;
        bool hasBoxGlyph = false;
        // Default_Ignorable code points and omitted controls draw nothing; non-spacing marks get a
        // .notdef box without adding an advance
        while (p < pEnd) {
            const SkUnichar u = next_utf8_scalar(&p, pEnd, -1);
            // Treat malformed UTF-8 as a visible .notdef box rather than an omitted control
            if (u < 0) {
                hasBoxGlyph = true;
                break;
            }
            if (unicode->isDefaultIgnorable(u) || is_coretext_omitted_control(u)) {
                continue;
            }
            hasBoxGlyph = true;
            if (!unicode->isNonSpacingMark(u)) {
                ++visibleCount;
            }
        }
        run->fGlyphData->glyphs()[k] = hasBoxGlyph ? 0 : invisibleGlyph;
        run->fGlyphData->positions()[k] = SkPoint::Make(fOwner->fAdvanceX + penX, 0);
        penX += visibleCount * notdefAdvance;
        if (hasBoxGlyph) {
            fOwner->recordUnresolvedGlyph(gRange);
        }
    }
    return penX;
}

SkScalar CoreTextLineShaper::SegmentShaper::populateShapedRun(Run* run,
                                                              CTRunRef ctRun,
                                                              const RunChoice& choice,
                                                              const BlockFonts& block) {
    SkASSERT(run);
    const int glyphCount = SkToInt(run->size());
    const size_t count = SkToSizeT(glyphCount);
    const bool ltr = run->leftToRight();
    const SkFont& font = run->font();
    const TextRange runRange = run->textRange();
    const bool zeroSize = has_non_positive_font_size(block.fStyle);
    Run::ShapingBuffer& buffer = fShapingBuffer;
    if (buffer.fOffsets.size() < glyphCount) {
        buffer.fOffsets.resize_back(glyphCount);
    }
    SkGlyphID* glyphs = run->fGlyphData->glyphs();
    skia_private::AutoSTArray<64, SkScalar> advances(glyphCount);
    const bool coalesced = choice.fix == NewlineFix::kCoalescedTrailing;
    const size_t ctCount = count - (coalesced ? 1 : 0);
    const GlyphSlots slots = NewlineGlyphSlots(choice.fix, ltr, ctCount);
    // ctRun holds the fallback font's glyph ID rather than the pinned primary face's, so a pinned
    // '\n' emits .notdef (0) directly and gets its HarfBuzz advance below
    if (choice.fix != NewlineFix::kPinnedStandalone) {
        // Only left-to-right subpixel runs split pair kerning; a tab's advance to its stop is not
        // kerning, so runs containing a tab keep whole advances
        const bool hasTab =
                memchr(fText.data() + runRange.start, '\t', runRange.width()) != nullptr;
        const bool splitKerning = !zeroSize && ltr && font.isSubpixel() && !hasTab;
        const size_t base = slots.ctStart;
        extract_ctrun_glyphs(ctRun,
                             choice.ctFont,
                             advance_mode(zeroSize, font.isSubpixel(), splitKerning),
                             SkSpan<SkGlyphID>(glyphs, count).subspan(base, ctCount),
                             SkSpan<SkPoint>(buffer.fOffsets.data(), count).subspan(base, ctCount),
                             SkSpan<SkScalar>(advances.data(), count).subspan(base, ctCount));
    }
    if (choice.fix != NewlineFix::kNone) {
        const size_t nl = slots.newline;
        glyphs[nl] = choice.newlineGlyph;
        buffer.fOffsets[nl] = glyph_draw_offset(CGPointZero, zeroSize, 0);
        advances[nl] = 0.0f;
    }

    // CTRunGetPositions folds mark draw offsets in and lacks the non-subpixel and newline
    // adjustments, so the pen walks the advances instead
    SkScalar penX = 0;
    // A run of only characters CoreText drops (C0 controls) keeps one zero-advance
    // kCGFontIndexInvalid placeholder, no glyph id of the font; space draws nothing
    // TODO(jlavrova): a font without a space glyph gets .notdef at zero advance — remove when Run
    // can hold a glyph-less cluster
    const SkGlyphID invisibleGlyph = font.unicharToGlyph(' ');
    for (int k = 0; k < glyphCount; ++k) {
        // CoreText gives '\n' no advance, HarfBuzz the advance of the primary font's cmap glyph
        // (.notdef when it has none); that advance moves all later lines, so their float positions
        // and widths differ unless '\n' gets one here too
        const TextRange gRange = glyph_text_range(*run, k);
        if (!zeroSize && gRange.width() == 1 && fText[gRange.start] == '\n') {
            const SkScalar newlineAdvance = font.getWidth(font.unicharToGlyph('\n'));
            // Whole pixels for a non-subpixel font, as for every other glyph
            advances[k] =
                    font.isSubpixel() ? newlineAdvance : SkScalarRoundToScalar(newlineAdvance);
        }
        if (glyphs[k] == kCGFontIndexInvalid) {
            glyphs[k] = invisibleGlyph;
        }
        // Cluster widths and hit-testing read the positions, and extendedVisit and getPath hand
        // the positions out without the offsets, so the whole origin goes into the offset as
        // SkShaper's HarfBuzz runs put x_offset and y_offset there
        run->fGlyphData->positions()[k] = SkPoint::Make(fOwner->fAdvanceX + penX, 0);
        penX += advances[k];
        if (glyphs[k] == 0) {
            fOwner->recordUnresolvedGlyph(gRange);
        }
    }
    run->commit(buffer);
    return penX;
}

}  // namespace textlayout
}  // namespace skia
