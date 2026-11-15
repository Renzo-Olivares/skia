// Copyright 2026 Google LLC
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontArguments.h"
#include "include/core/SkFontParameters.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPoint.h"
#include "include/core/SkRect.h"
#include "include/core/SkRefCnt.h"
#include "include/core/SkScalar.h"
#include "include/core/SkSize.h"
#include "include/core/SkString.h"
#include "include/core/SkTypeface.h"
#include "include/effects/SkDashPathEffect.h"
#include "modules/skparagraph/include/DartTypes.h"
#include "modules/skparagraph/include/FontCollection.h"
#include "modules/skparagraph/include/Metrics.h"
#include "modules/skparagraph/include/Paragraph.h"
#include "modules/skparagraph/include/ParagraphStyle.h"
#include "modules/skparagraph/include/TextStyle.h"
#include "modules/skparagraph/src/ParagraphBuilderImpl.h"
#include "modules/skparagraph/src/ParagraphImpl.h"
#include "modules/skparagraph/src/Run.h"
#include "modules/skunicode/include/SkUnicode.h"
#include "modules/skunicode/include/SkUnicode_apple.h"
#if defined(SK_UNICODE_ICU_IMPLEMENTATION)
#include "modules/skunicode/include/SkUnicode_icu.h"
#endif
#include "src/core/SkUTF.h"
#include "tools/fonts/FontToolUtils.h"
#include "tools/viewer/Slide.h"

#include <algorithm>
#include <array>
#include <functional>
#include <vector>

using namespace skia::textlayout;

namespace {

// Palette for engine comparison cards (red = Legacy HarfBuzz, green = Candidate CoreText)
constexpr SkColor kInk = SkColorSetRGB(15, 23, 42);
constexpr SkColor kDark = SkColorSetRGB(30, 41, 59);
constexpr SkColor kMuted = SkColorSetRGB(100, 116, 139);
constexpr SkColor kRed = SkColorSetRGB(220, 38, 38);
constexpr SkColor kGreen = SkColorSetRGB(22, 163, 74);
constexpr SkColor kBlue = SkColorSetRGB(37, 99, 235);
// Partial fixes: CoreText resolves only part of what the issue asks for
constexpr SkColor kAmber = SkColorSetRGB(217, 119, 6);
constexpr SkScalar kSpacing = 16.0f;
// Same string as SkParagraph_Flutter_150824_18915_SystemFontTightensWithSize, so the measured
// ratios on the tracking slides are the ones the test asserts
constexpr char kTrackingText[] = "DynamicTrackingTest Hello World";

// getRectsForRange takes UTF-16 offsets while Run::textRange() is in UTF-8 bytes
size_t utf16Index(const char* text, size_t utf8Index) {
    const char* ptr = text;
    const char* end = text + utf8Index;
    size_t units = 0;
    while (ptr < end) {
        SkUnichar u = SkUTF::NextUTF8(&ptr, end);
        if (u < 0) {
            break;
        }
        units += u > 0xFFFF ? 2 : 1;
    }
    return units;
}

// Design coordinate and range of the axis on the typeface that renders the run; false when the
// typeface has no such axis
bool axisState(const SkTypeface* typeface,
               SkFourByteTag tag,
               SkScalar* value,
               SkScalar* min,
               SkScalar* max) {
    int count = typeface->getVariationDesignParameters({});
    if (count <= 0) {
        return false;
    }
    std::vector<SkFontParameters::Variation::Axis> axes(count);
    typeface->getVariationDesignParameters(axes);
    std::vector<SkFontArguments::VariationPosition::Coordinate> coords(count);
    int coordCount = typeface->getVariationDesignPosition(coords);
    for (const auto& a : axes) {
        if (a.tag != tag) {
            continue;
        }
        *min = a.min;
        *max = a.max;
        // Unpinned axes are omitted from getVariationDesignPosition, so default to a.def first
        *value = a.def;
        for (int i = 0; i < coordCount; ++i) {
            if (coords[i].axis == tag) {
                *value = coords[i].value;
            }
        }
        return true;
    }
    return false;
}

// Sum of the rendered font's own advances, so CoreText 'trak' does not leak into opsz numbers
SkScalar glyphAdvanceSum(const Run& run) {
    std::vector<SkScalar> widths(run.size());
    run.font().getWidths(run.glyphs(), widths);
    SkScalar sum = 0;
    for (SkScalar w : widths) {
        sum += w;
    }
    return sum;
}

class ParagraphBugSlide_Base : public Slide {
public:
    void load(SkScalar w, SkScalar h) override { fSize = {w, h}; }

    void resize(SkScalar w, SkScalar h) override { fSize = {w, h}; }

protected:
    SkSize fSize = {1050, 750};

    sk_sp<FontCollection> getFontCollection() {
        if (!fFontCollection) {
            fFontCollection = sk_make_sp<FontCollection>();
            fFontCollection->setDefaultFontManager(ToolUtils::TestFontMgr());
            fFontCollection->enableFontFallback();
        }
        return fFontCollection;
    }

    // Same pairing as SkParagraphFlutterBugsTest so slides and tests agree: Apple's SkUnicode
    // for CoreText, ICU for HarfBuzz where the build has it
    sk_sp<SkUnicode> getUnicode(TextShapingEngine engine) {
#if defined(SK_UNICODE_ICU_IMPLEMENTATION)
        if (engine == TextShapingEngine::kDefault) {
            return SkUnicodes::ICU::Make();
        }
#endif
        return SkUnicodes::Apple::Make();
    }

    void drawText(SkCanvas* canvas,
                  const char* str,
                  SkScalar x,
                  SkScalar y,
                  SkColor color,
                  bool bold = false,
                  SkScalar fontSize = 12.0f) {
        SkFont font(ToolUtils::DefaultTypeface(), fontSize);
        font.setEmbolden(bold);
        SkPaint paint;
        paint.setColor(color);
        paint.setAntiAlias(true);
        canvas->drawString(str, x, y, font, paint);
    }

    void drawFrame(SkCanvas* canvas, const SkRect& r, SkColor color, bool dashed = false) {
        SkPaint paint;
        paint.setStyle(SkPaint::kStroke_Style);
        paint.setColor(color);
        paint.setStrokeWidth(dashed ? 1.5f : 1.2f);
        paint.setAntiAlias(true);
        if (dashed) {
            constexpr std::array<SkScalar, 2> intervals = {4.0f, 3.0f};
            paint.setPathEffect(SkDashPathEffect::Make(intervals, 0.0f));
        }
        canvas->drawRect(r, paint);
    }

    // Pen origin and usable width inside a card's 20 pt side padding, top pt below its top edge
    struct CardPen {
        SkScalar x;
        SkScalar y;
        SkScalar maxWidth;
    };
    static CardPen cardPen(const SkRect& card, SkScalar top) {
        return {card.fLeft + 20.0f, card.fTop + top, card.width() - 40.0f};
    }

    // Paints p at (x, y) scaled by scale inside a color frame, plus a dashed red slot up to
    // excessRight when the other engine's layout of the same text is wider by over half a point
    void paintScaledRow(SkCanvas* canvas,
                        Paragraph* p,
                        SkScalar x,
                        SkScalar y,
                        SkScalar scale,
                        SkColor color,
                        SkScalar excessRight) {
        const SkScalar w = p->getLongestLine();
        canvas->save();
        canvas->translate(x, y);
        canvas->scale(scale, scale);
        p->paint(canvas, 0, 0);
        drawFrame(canvas, SkRect::MakeWH(w, p->getHeight()), color);
        if (excessRight > w + 0.5f) {
            drawFrame(canvas, SkRect::MakeLTRB(w, 0, excessRight, p->getHeight()), kRed, true);
        }
        canvas->restore();
    }

    // One listed run of drawRunList: its frame color, dashed frame, and list line
    struct RunRow {
        SkColor color;
        bool dashed;
        SkString label;
    };

    // Paints p (laid out from text) in card, frames every run's tight rects and lists the
    // describe(index, run) rows below it, bold unless blue; returns the y below the list
    template <typename Describe>
    SkScalar drawRunList(SkCanvas* canvas,
                         const SkRect& card,
                         Paragraph* p,
                         const char* text,
                         Describe describe) {
        const CardPen pen = cardPen(card, 60.0f);
        p->paint(canvas, pen.x, pen.y);
        auto impl = static_cast<ParagraphImpl*>(p);
        SkScalar listY = pen.y + p->getHeight() + 30.0f;
        size_t index = 0;
        for (const Run& run : impl->runs()) {
            const RunRow row = describe(index++, run);
            auto rects = p->getRectsForRange(utf16Index(text, run.textRange().start),
                                             utf16Index(text, run.textRange().end),
                                             RectHeightStyle::kTight,
                                             RectWidthStyle::kTight);
            for (const TextBox& box : rects) {
                drawFrame(canvas, box.rect.makeOffset(pen.x, pen.y), row.color, row.dashed);
            }
            drawText(canvas, row.label.c_str(), pen.x, listY, row.color, row.color != kBlue);
            listY += 20.0f;
        }
        return listY;
    }

    void drawHeader(SkCanvas* canvas,
                    const char* bugId,
                    const char* title,
                    const char* status,
                    SkColor statusBg,
                    const char* summary) {
        SkScalar width = fSize.width();

        SkPaint bgPaint;
        bgPaint.setColor(SkColorSetRGB(246, 248, 251));
        canvas->drawRect(SkRect::MakeXYWH(0, 0, width, 84.0f), bgPaint);

        SkPaint divider;
        divider.setColor(SkColorSetRGB(222, 228, 237));
        divider.setStrokeWidth(1.0f);
        canvas->drawLine(0, 84.0f, width, 84.0f, divider);

        drawText(canvas, bugId, 24.0f, 28.0f, kBlue, true, 13.0f);
        drawText(canvas, title, 24.0f, 54.0f, kInk, true, 19.0f);

        SkFont badgeFont(ToolUtils::DefaultTypeface(), 11.0f);
        badgeFont.setEmbolden(true);
        SkScalar badgeWidth =
                badgeFont.measureText(status, strlen(status), SkTextEncoding::kUTF8) + 16.0f;
        SkRect badgeRect = SkRect::MakeXYWH(width - badgeWidth - 24.0f, 16.0f, badgeWidth, 22.0f);
        SkPaint badgeBg;
        badgeBg.setColor(statusBg);
        badgeBg.setAntiAlias(true);
        canvas->drawRoundRect(badgeRect, 4.0f, 4.0f, badgeBg);
        drawText(canvas,
                 status,
                 badgeRect.fLeft + 8.0f,
                 badgeRect.fBottom - 6.0f,
                 SK_ColorWHITE,
                 true,
                 11.0f);

        if (summary && summary[0]) {
            drawText(canvas, summary, 24.0f, 74.0f, kMuted);
        }
    }

    void drawCard(SkCanvas* canvas,
                  const SkRect& r,
                  const char* header,
                  SkColor headerColor,
                  const char* note = nullptr) {
        SkPaint bg;
        bg.setColor(SK_ColorWHITE);
        canvas->drawRoundRect(r, 8.0f, 8.0f, bg);

        SkPaint border;
        border.setStyle(SkPaint::kStroke_Style);
        border.setColor(SkColorSetRGB(226, 232, 240));
        border.setStrokeWidth(1.5f);
        border.setAntiAlias(true);
        canvas->drawRoundRect(r, 8.0f, 8.0f, border);

        SkRect headerRect = SkRect::MakeXYWH(r.fLeft, r.fTop, r.width(), 32.0f);
        SkPaint headerBg;
        headerBg.setColor(SkColorSetRGB(248, 250, 252));
        headerBg.setAntiAlias(true);
        canvas->drawRoundRect(headerRect, 8.0f, 8.0f, headerBg);
        canvas->drawRect(SkRect::MakeXYWH(r.fLeft, r.fTop + 16.0f, r.width(), 16.0f), headerBg);
        canvas->drawLine(r.fLeft, r.fTop + 32.0f, r.fRight, r.fTop + 32.0f, border);

        drawText(canvas, header, r.fLeft + 14.0f, r.fTop + 21.0f, headerColor, true, 13.0f);

        if (note && note[0]) {
            drawText(canvas, note, r.fLeft + 14.0f, r.fBottom - 10.0f, kMuted, false, 11.0f);
        }
    }

    struct ComparisonSlideDesc {
        const char* bugId;
        const char* title;
        const char* status;
        SkColor statusBg;
        const char* summary;
        const char* leftTitle;
        const char* leftSubtitle;
        std::function<void(const SkRect&)> drawLeft;
        const char* rightTitle;
        const char* rightSubtitle;
        std::function<void(const SkRect&)> drawRight;
        const char* oldDefectExplanation;
        const char* newFixExplanation;
        const char* invariantExplanation;
    };

    // Paints the full-canvas two-card comparison layout and bottom summary band; passes each
    // card's outer bounds (including its header strip) to desc.drawLeft and desc.drawRight
    void drawComparisonSlide(SkCanvas* canvas, const ComparisonSlideDesc& desc) {
        canvas->drawColor(SkColorSetRGB(248, 250, 252));
        drawHeader(canvas, desc.bugId, desc.title, desc.status, desc.statusBg, desc.summary);

        SkScalar cardWidth = (fSize.width() - 72.0f) / 2.0f;
        SkScalar cardHeight = 424.0f;

        SkRect leftRect = SkRect::MakeXYWH(24.0f, 96.0f, cardWidth, cardHeight);
        drawCard(canvas, leftRect, desc.leftTitle, kRed, desc.leftSubtitle);
        desc.drawLeft(leftRect);

        SkRect rightRect = SkRect::MakeXYWH(48.0f + cardWidth, 96.0f, cardWidth, cardHeight);
        drawCard(canvas, rightRect, desc.rightTitle, kGreen, desc.rightSubtitle);
        desc.drawRight(rightRect);

        SkRect bottomRect = SkRect::MakeXYWH(24.0f, 532.0f, fSize.width() - 48.0f, 194.0f);
        drawCard(canvas,
                 bottomRect,
                 "Comparative Analysis & Resolution Mechanics",
                 kInk,
                 "Every number on the cards above is measured from the live layouts");

        SkScalar bx = bottomRect.fLeft + 20.0f;
        SkScalar by = bottomRect.fTop + 56.0f;
        drawText(canvas, "Legacy Version (Baseline):", bx, by, kRed, true);
        drawText(canvas, desc.oldDefectExplanation, bx + 225.0f, by, kDark);
        by += 32.0f;
        drawText(canvas, "Candidate Version (New Engine):", bx, by, kGreen, true);
        drawText(canvas, desc.newFixExplanation, bx + 225.0f, by, kDark);
        by += 32.0f;
        drawText(canvas, "Architectural Invariant:", bx, by, kBlue, true);
        drawText(canvas, desc.invariantExplanation, bx + 225.0f, by, kDark);
    }

    static TextStyle makeStyle(const char* family, SkScalar fontSize, SkScalar spacing = 0.0f) {
        TextStyle ts;
        ts.setFontFamilies({SkString(family)});
        ts.setFontSize(fontSize);
        ts.setLetterSpacing(spacing);
        ts.setColor(kInk);
        return ts;
    }

    std::unique_ptr<Paragraph> makeParagraph(TextShapingEngine engine,
                                             const TextStyle& ts,
                                             const char* text,
                                             SkScalar layoutWidth = 620.0f) {
        ParagraphStyle style;
        style.setTextShapingEngine(engine);
        ParagraphBuilderImpl builder(style, getFontCollection(), getUnicode(engine));
        builder.pushStyle(ts);
        builder.addText(text);
        builder.pop();
        auto paragraph = builder.Build();
        paragraph->layout(layoutWidth);
        return paragraph;
    }

    // The dashed red slot shows how far the HarfBuzz layout of the same text extends past this one
    // Wide rows are drawn scaled down to maxWidth; the printed numbers stay unscaled
    SkScalar drawWidthRow(SkCanvas* canvas,
                          Paragraph* p,
                          SkScalar x,
                          SkScalar y,
                          SkScalar maxWidth,
                          SkScalar fontSize,
                          const char* label,
                          SkColor accent,
                          SkScalar harfBuzzWidth) {
        drawText(canvas, label, x, y, kDark, true);
        y += 10.0f;
        SkScalar w = p->getLongestLine();
        SkScalar scale = std::min(1.0f, maxWidth / std::max(w, harfBuzzWidth));
        paintScaledRow(canvas, p, x, y, scale, accent, harfBuzzWidth);
        bool excess = harfBuzzWidth > w + 0.5f;
        y += p->getHeight() * scale + 18.0f;
        SkString info;
        info.printf("Width %.2f pt | %.3f pt per em", w, w / fontSize);
        if (excess) {
            info.appendf(" | HarfBuzz is %.2f pt wider (dashed)", harfBuzzWidth - w);
        }
        if (scale < 1.0f) {
            info.appendf(" | drawn at %.0f%%", scale * 100.0f);
        }
        drawText(canvas, info.c_str(), x, y, accent, true);
        return y + 34.0f;
    }

    // Glyphs without ink become dashed advance slots because letterSpacing still widens them
    size_t drawGlyphBoxes(SkCanvas* canvas, Paragraph* p, SkScalar x, SkScalar y, SkColor accent) {
        auto impl = static_cast<ParagraphImpl*>(p);
        std::vector<LineMetrics> lines;
        p->getLineMetrics(lines);
        // Non-empty sample text shaped for the slide always produces at least one line and run
        SkASSERT(!impl->runs().empty() && !lines.empty());
        const Run& run = impl->runs()[0];
        auto glyphs = run.glyphs();
        auto pos = run.positions();
        auto off = run.offsets();
        // Run keeps a trailing sentinel position, so pos[i + 1] exists for every glyph
        SkASSERT(pos.size() == run.size() + 1);
        // offsets() is empty when every glyph offset is zero
        const SkPoint zero = {0, 0};
        SkScalar originX = x + lines[0].fLeft;
        SkScalar baseY = y + lines[0].fBaseline;
        size_t phantoms = 0;
        for (size_t i = 0; i < run.size(); ++i) {
            const SkPoint& o = off.empty() ? zero : off[i];
            // Run positions carry paragraph-global x; subtract pos[0].fX to make it line-local
            SkScalar gx = originX + pos[i].fX - pos[0].fX + o.fX;
            SkScalar gy = baseY + pos[i].fY + o.fY;
            SkRect ink = run.font().getBounds(glyphs[i], nullptr);
            if (!ink.isEmpty()) {
                drawFrame(canvas, ink.makeOffset(gx, gy), accent);
                continue;
            }
            ++phantoms;
            SkScalar slot = std::max(pos[i + 1].fX - pos[i].fX, 2.0f);
            drawFrame(canvas,
                      SkRect::MakeLTRB(gx, baseY + run.ascent(), gx + slot, baseY + run.descent()),
                      kRed,
                      true);
        }
        return phantoms;
    }

    // Draws unspaced (p0) and kSpacing-spaced (p16) laid-out non-empty paragraphs inside card,
    // marking zero-ink advance slots and deriving spaced slots from (width16 - width0) / kSpacing
    void drawSpacingCard(SkCanvas* canvas,
                         const SkRect& card,
                         Paragraph* p0,
                         Paragraph* p16,
                         SkColor accent,
                         const SkString& extra) {
        SkScalar x = card.fLeft + 20.0f;
        SkScalar y = card.fTop + 56.0f;
        struct SpacingRow {
            Paragraph* para;
            SkScalar spacing;
        };
        const std::array<SpacingRow, 2> rows = {{{p0, 0.0f}, {p16, kSpacing}}};
        for (const auto& row : rows) {
            auto impl = static_cast<ParagraphImpl*>(row.para);
            // Non-empty sample text in the letter spacing rows always shapes into a run
            SkASSERT(!impl->runs().empty());
            SkString label;
            label.printf("letterSpacing = %.0f pt: %zu run(s), %zu glyphs in run 0, width %.2f pt",
                         row.spacing,
                         impl->runs().size(),
                         impl->runs()[0].size(),
                         row.para->getLongestLine());
            drawText(canvas, label.c_str(), x, y, kDark, true);
            y += 14.0f;
            row.para->paint(canvas, x, y);
            size_t phantoms = drawGlyphBoxes(canvas, row.para, x, y, accent);
            y += row.para->getHeight() + 16.0f;
            if (phantoms > 0) {
                SkString note;
                note.printf("%zu glyph(s) without ink (dashed red) still own an advance slot",
                            phantoms);
                drawText(canvas, note.c_str(), x, y, kRed);
                y += 18.0f;
            }
            y += 14.0f;
        }
        SkScalar delta = p16->getLongestLine() - p0->getLongestLine();
        SkString deltaStr;
        deltaStr.printf("Width delta +%.1f pt = %.0f spaced slots x %.0f pt",
                        delta,
                        delta / kSpacing,
                        kSpacing);
        drawText(canvas, deltaStr.c_str(), x, y, accent, true, 13.0f);
        if (!extra.isEmpty()) {
            drawText(canvas, extra.c_str(), x, y + 22.0f, accent, true, 13.0f);
        }
    }

    // Lays out non-empty text at 0 and kSpacing pt on both engines and renders their spacing
    // cards side by side; extra(p0, p16) must be non-null and formats an optional summary line
    void drawSpacingSlide(SkCanvas* canvas,
                          const char* bugId,
                          const char* title,
                          const char* summary,
                          const char* family,
                          SkScalar fontSize,
                          const char* text,
                          SkString (*extra)(Paragraph*, Paragraph*),
                          const char* leftSubtitle,
                          const char* rightSubtitle,
                          const char* defect,
                          const char* fix,
                          const char* invariant) {
        auto hb0 = makeParagraph(TextShapingEngine::kDefault, makeStyle(family, fontSize), text);
        auto hb16 = makeParagraph(
                TextShapingEngine::kDefault, makeStyle(family, fontSize, kSpacing), text);
        auto ct0 = makeParagraph(TextShapingEngine::kCoreText, makeStyle(family, fontSize), text);
        auto ct16 = makeParagraph(
                TextShapingEngine::kCoreText, makeStyle(family, fontSize, kSpacing), text);

        drawComparisonSlide(canvas,
                            {bugId,
                             title,
                             "FIXED IN CORETEXT",
                             kGreen,
                             summary,
                             "Legacy Version: HarfBuzz (Run::addLetterSpacesEvenly)",
                             leftSubtitle,
                             [&](const SkRect& card) {
                                 drawSpacingCard(canvas,
                                                 card,
                                                 hb0.get(),
                                                 hb16.get(),
                                                 kRed,
                                                 extra(hb0.get(), hb16.get()));
                             },
                             "Candidate Version: CoreText (Run::addLetterSpacesAfterGraphemes)",
                             rightSubtitle,
                             [&](const SkRect& card) {
                                 drawSpacingCard(canvas,
                                                 card,
                                                 ct0.get(),
                                                 ct16.get(),
                                                 kGreen,
                                                 extra(ct0.get(), ct16.get()));
                             },
                             defect,
                             fix,
                             invariant});
    }

    // Point sizes of a tracking slide; cupertino > 0 adds the explicit-letterSpacing row
    struct TrackingSizes {
        SkScalar small;
        SkScalar large;
        SkScalar cupertino;
        SkScalar cupertinoSpacing;
    };

    // One engine's tracking rows; the Cupertino pair stays null when sizes.cupertino is 0
    struct TrackingLayouts {
        std::unique_ptr<Paragraph> small;
        std::unique_ptr<Paragraph> large;
        std::unique_ptr<Paragraph> cupertinoAuto;
        std::unique_ptr<Paragraph> cupertinoExplicit;
    };

    TrackingLayouts layoutTracking(TextShapingEngine engine,
                                   const char* text,
                                   const TrackingSizes& sizes) {
        const char* family = ".AppleSystemUIFont";
        // Unbounded like the test, so getLongestLine() is the single-line width the ratio needs
        const SkScalar kNoWrap = 2000.0f;
        TrackingLayouts l;
        l.small = makeParagraph(engine, makeStyle(family, sizes.small), text, kNoWrap);
        l.large = makeParagraph(engine, makeStyle(family, sizes.large), text, kNoWrap);
        if (sizes.cupertino > 0.0f) {
            l.cupertinoAuto =
                    makeParagraph(engine, makeStyle(family, sizes.cupertino), text, kNoWrap);
            l.cupertinoExplicit = makeParagraph(
                    engine,
                    makeStyle("CupertinoSystemText", sizes.cupertino, sizes.cupertinoSpacing),
                    text,
                    kNoWrap);
        }
        return l;
    }

    // harfBuzz is null on the HarfBuzz card; on the CoreText card it supplies the dashed widths
    void drawTrackingCard(SkCanvas* canvas,
                          const SkRect& card,
                          const TrackingLayouts& rows,
                          const TrackingLayouts* harfBuzz,
                          const TrackingSizes& sizes,
                          SkColor accent) {
        auto [x, y, maxWidth] = cardPen(card, 56.0f);
        SkString label;
        label.printf("%.0f pt .AppleSystemUIFont:", sizes.small);
        y = drawWidthRow(canvas,
                         rows.small.get(),
                         x,
                         y,
                         maxWidth,
                         sizes.small,
                         label.c_str(),
                         accent,
                         harfBuzz ? harfBuzz->small->getLongestLine() : 0.0f);
        label.printf("%.0f pt .AppleSystemUIFont:", sizes.large);
        y = drawWidthRow(canvas,
                         rows.large.get(),
                         x,
                         y,
                         maxWidth,
                         sizes.large,
                         label.c_str(),
                         accent,
                         harfBuzz ? harfBuzz->large->getLongestLine() : 0.0f);
        SkString ratio;
        ratio.printf("Width ratio %.0f pt / %.0f pt = %.3f (linear scaling = %.3f)",
                     sizes.large,
                     sizes.small,
                     rows.large->getLongestLine() / rows.small->getLongestLine(),
                     sizes.large / sizes.small);
        drawText(canvas, ratio.c_str(), x, y, accent, true, 13.0f);
        if (!rows.cupertinoExplicit) {
            return;
        }
        // CupertinoTextThemeData's explicit letterSpacing stacks on auto trak, as native
        // tracking does
        SkString expLine;
        if (harfBuzz) {
            expLine.printf(
                    "%.0f pt Cupertino letterSpacing=%+.2f: %.2f pt (auto-trak %.2f pt; "
                    "spacing stacks on trak as in UIKit)",
                    sizes.cupertino,
                    sizes.cupertinoSpacing,
                    rows.cupertinoExplicit->getLongestLine(),
                    rows.cupertinoAuto->getLongestLine());
        } else {
            expLine.printf(
                    "%.0f pt Cupertino letterSpacing=%+.2f: %.2f pt (falls back without "
                    ".SFNS opsz)",
                    sizes.cupertino,
                    sizes.cupertinoSpacing,
                    rows.cupertinoExplicit->getLongestLine());
        }
        drawText(canvas, expLine.c_str(), x, y + 22.0f, accent, true, 12.0f);
    }

    // Compares single-line (2000 pt no-wrap) .AppleSystemUIFont widths at smallSize and largeSize
    // (> 0, non-empty text) on HarfBuzz vs CoreText to expose automatic opsz + trak scaling; adds
    // an explicit-letterSpacing CupertinoSystemText row when cupertinoSize > 0
    void drawTrackingSlide(SkCanvas* canvas,
                           const char* bugId,
                           const char* title,
                           const char* summary,
                           const char* text,
                           SkScalar smallSize,
                           SkScalar largeSize,
                           SkScalar cupertinoSize,
                           SkScalar cupertinoSpacing,
                           const char* defect,
                           const char* fix,
                           const char* invariant) {
        const TrackingSizes sizes = {smallSize, largeSize, cupertinoSize, cupertinoSpacing};
        TrackingLayouts hb = layoutTracking(TextShapingEngine::kDefault, text, sizes);
        TrackingLayouts ct = layoutTracking(TextShapingEngine::kCoreText, text, sizes);
        drawComparisonSlide(canvas,
                            {bugId,
                             title,
                             "FIXED IN CORETEXT",
                             kGreen,
                             summary,
                             "Legacy Version: HarfBuzz (SkCTFontCreateExactCopy)",
                             "Size-frozen CTFont copy: advances scale linearly with point size",
                             [&](const SkRect& card) {
                                 drawTrackingCard(canvas, card, hb, nullptr, sizes, kRed);
                             },
                             "Candidate Version: CoreText (.SFNS 'opsz' + 'trak')",
                             "CTFont created at the target size; dashed red = extra HarfBuzz width",
                             [&](const SkRect& card) {
                                 drawTrackingCard(canvas, card, ct, &hb, sizes, kGreen);
                             },
                             defect,
                             fix,
                             invariant});
    }

private:
    sk_sp<FontCollection> fFontCollection;
};

class ParagraphSlide_BugsOverview : public ParagraphBugSlide_Base {
public:
    ParagraphSlide_BugsOverview() { fName = "ParagraphSlide_BugsOverview"; }

    void draw(SkCanvas* canvas) override {
        canvas->drawColor(SkColorSetRGB(248, 250, 252));
        // 13 of the 15 cataloged cases are fully resolved by the CoreText pipeline; #109308 asks
        // for full variable font support and CoreText only covers automatic optical sizing, and
        // for #193812 CoreText adds only the size-dependent trak and opsz of the designs
        drawHeader(canvas,
                   "DASHBOARD",
                   "SkParagraph HarfBuzz vs CoreText: Verified Layout Differences",
                   "15 CASES: 13 FIXED, 2 PARTIAL IN CORETEXT",
                   kGreen,
                   "Each row is backed by a dm test; each slide lays out the same text with both "
                   "engines and draws the measured geometry");

        const SkScalar rowWidth = fSize.width() - 48.0f;
        // 15 rows of 38 + 4 pt below the 100 pt header stay within a 750 pt tall window
        constexpr SkScalar kRowHeight = 38.0f;
        SkScalar y = 100.0f;
        SkFont badgeFont(ToolUtils::DefaultTypeface(), 10.0f);
        badgeFont.setEmbolden(true);
        for (const OverviewRow& row : kBugs) {
            this->drawOverviewRow(
                    canvas, badgeFont, row, SkRect::MakeXYWH(24.0f, y, rowWidth, kRowHeight));
            y += kRowHeight + 4.0f;
        }
    }

private:
    struct OverviewRow {
        const char* id;
        const char* title;
        const char* test;
        bool fixed;
    };

    static constexpr std::array<OverviewRow, 15> kBugs = {{
            {"Flutter 150824",
             "Size-dependent .SFNS tracking (11 pt vs 48 pt)",
             "SkParagraph_Flutter_150824_18915_SystemFontTightensWithSize",
             true},
            {"Flutter 18915",
             "Optical size: 28 pt / 14 pt width ratio below 2.0",
             "SkParagraph_Flutter_150824_18915_SystemFontTightensWithSize",
             true},
            {"Flutter 147708",
             "CupertinoSystemText/Display aliases with fontFamilyFallback",
             "SkParagraph_Flutter_147708_CupertinoAliasesBypassFallback",
             true},
            {"Related: Flutter 193812",
             "Rounded/Serif designs: size-dependent trak and opsz (reachability unchanged)",
             "SkParagraph_Flutter_193812_DesignsTrackAtLayoutSize",
             false},
            {"Flutter 22572",
             "17 pt body text tracking vs 11 pt (auto trak + Cupertino letterSpacing)",
             "SkParagraph_Flutter_22572_CupertinoLetterSpacingStacksOnTrak",
             true},
            {"No tracked issue",
             "Keycap emoji: zero-ink U+FE0F glyphs take letterSpacing",
             "SkParagraph_KeycapEmojiClusterLetterSpacing",
             true},
            {"Flutter 36062",
             "Keycap emoji: primary font mapping U+20E3 shows digit + box",
             "SkParagraph_Flutter_36062_KeycapPrimaryFontMapsU20E3",
             true},
            {"No tracked issue",
             "ZWJ emoji: zero-ink U+FE0F glyphs take letterSpacing",
             "SkParagraph_ZWJEmojiVariationSelectorLetterSpacing",
             true},
            {"Flutter 118127",
             "Combining acute drifts off its base under letterSpacing",
             "SkParagraph_Flutter_118127_CombiningDiacriticsLetterSpacing",
             true},
            {"Flutter 144528",
             "Devanagari matras detach under letterSpacing",
             "SkParagraph_Flutter_144528_IndicConjunctLetterSpacing",
             true},
            {"No tracked issue",
             "Unmapped PUA: LastResort tofu vs .SFNS .notdef",
             "SkParagraph_AppleSystemUIFontCascade",
             true},
            {"Flutter 109308",
             "Variable fonts, auto optical size only: opsz follows 12 pt vs 72 pt",
             "SkParagraph_Flutter_109308_AutoOpticalSize",
             false},
            {"Flutter 144099",
             "CupertinoSystemText Arabic text resolves to the system Arabic face",
             "SkParagraph_Flutter_144099_CupertinoArabicSystemFont",
             true},
            {"Skia b/458132646",
             "Arabic under letterSpacing: CoreText skips cursive runs like HarfBuzz",
             "CoreTextPipeline_LetterSpacing_CursiveArabicSystemFonts",
             true},
            {"Flutter 141080",
             "Thai tone mark colour: CupertinoSystemText fallback picks ligating Thonburi",
             "SkParagraph_Flutter_141080_CupertinoThaiToneMarkColor",
             true},
    }};

    void drawOverviewRow(SkCanvas* canvas,
                         const SkFont& badgeFont,
                         const OverviewRow& row,
                         const SkRect& rowRect) {
        const char* badge = row.fixed ? "FIXED" : "PARTIAL";
        SkPaint rowBg;
        rowBg.setColor(SK_ColorWHITE);
        rowBg.setAntiAlias(true);
        canvas->drawRoundRect(rowRect, 6.0f, 6.0f, rowBg);
        drawFrame(canvas, rowRect, SkColorSetRGB(226, 232, 240));

        const SkScalar x = rowRect.fLeft;
        const SkScalar y = rowRect.fTop;
        drawText(canvas, row.id, x + 14.0f, y + 17.0f, kInk, true, 13.0f);
        drawText(canvas, row.title, x + 230.0f, y + 17.0f, kDark, false, 13.0f);
        drawText(canvas, row.test, x + 230.0f, y + 32.0f, kMuted, false, 11.0f);

        const SkScalar bWidth =
                badgeFont.measureText(badge, strlen(badge), SkTextEncoding::kUTF8) + 16.0f;
        const SkRect bRect =
                SkRect::MakeXYWH(rowRect.fRight - bWidth - 14.0f, y + 8.0f, bWidth, 22.0f);
        SkPaint bBg;
        bBg.setColor(row.fixed ? kGreen : kAmber);
        bBg.setAntiAlias(true);
        canvas->drawRoundRect(bRect, 4.0f, 4.0f, bBg);
        drawText(canvas,
                 badge,
                 bRect.fLeft + 8.0f,
                 bRect.fBottom - 7.0f,
                 SK_ColorWHITE,
                 true,
                 10.0f);
    }
};

class ParagraphSlide_Flutter_150824_DynamicTracking : public ParagraphBugSlide_Base {
public:
    ParagraphSlide_Flutter_150824_DynamicTracking() {
        fName = "ParagraphSlide_Flutter_150824_DynamicTracking";
    }

    void draw(SkCanvas* canvas) override {
        // Explicit letterSpacing stacks on auto trak, as NSTrackingAttributeName does
        drawTrackingSlide(
                canvas,
                "FLUTTER ISSUE 150824",
                "Size-Dependent Tracking of San Francisco (.AppleSystemUIFont)",
                "CoreText applies .SFNS 'trak' by default and adds explicit letterSpacing on "
                "top of it, as UIKit does",
                kTrackingText,
                11.0f,
                48.0f,
                17.0f,
                -0.41f,
                "SkCTFontCreateExactCopy pins the CTFont size, so HarfBuzz advances scale "
                "linearly and miss 'trak'.",
                "CoreText applies .SFNS 'trak' at every letterSpacing and SkParagraph adds the "
                "spacing on top, as NSTrackingAttributeName does.",
                "Explicit theme letterSpacing stacks on automatic .SFNS tracking, as in native "
                "Apple text, while optical sizing stays active.");
    }
};

class ParagraphSlide_Flutter_18915_OpticalSize : public ParagraphBugSlide_Base {
public:
    ParagraphSlide_Flutter_18915_OpticalSize() {
        fName = "ParagraphSlide_Flutter_18915_OpticalSize";
    }

    void draw(SkCanvas* canvas) override {
        drawTrackingSlide(
                canvas,
                "FLUTTER ISSUE 18915",
                "Optical Sizing of .AppleSystemUIFont (Text vs Display)",
                "Doubling the size from 14 pt to 28 pt must not double the width "
                "(see also #147708 slide for CupertinoSystemText/Display fallback)",
                kTrackingText,
                14.0f,
                28.0f,
                0.0f,
                0.0f,
                "HarfBuzz shapes a size-frozen copy, so 28 pt is exactly 2.0x the 14 pt width.",
                "CoreText selects the display optical size and tracking at 28 pt, contracting "
                "the ratio below 2.0.",
                "Optical size and tracking are functions of the point size, never constants of "
                "the typeface.");
    }
};

class ParagraphSlide_Flutter_22572_DefaultTextTrackingHIGDelta : public ParagraphBugSlide_Base {
public:
    ParagraphSlide_Flutter_22572_DefaultTextTrackingHIGDelta() {
        fName = "ParagraphSlide_Flutter_22572_DefaultTextTrackingHIGDelta";
    }

    void draw(SkCanvas* canvas) override {
        drawTrackingSlide(
                canvas,
                "FLUTTER ISSUE 22572",
                "Default Tracking of 17 pt Body Text (Apple HIG)",
                "17 pt body text is tighter per em than 11 pt caption text; explicit Cupertino "
                "letterSpacing (-0.41 pt) stacks on auto 'trak' as in UIKit",
                kTrackingText,
                11.0f,
                17.0f,
                17.0f,
                -0.41f,
                "HarfBuzz renders 11 pt and 17 pt with the same per-em width (no 'trak' or "
                "'opsz').",
                "CoreText applies .SFNS 'trak' and adds the -0.41 pt Cupertino letterSpacing "
                "on top of it, as UIKit does.",
                "Body text density matches native Apple text: .SFNS 'trak' plus explicit "
                "Cupertino letterSpacing.");
    }
};

class ParagraphSlide_KeycapEmojiClusterLetterSpacing : public ParagraphBugSlide_Base {
public:
    ParagraphSlide_KeycapEmojiClusterLetterSpacing() {
        fName = "ParagraphSlide_KeycapEmojiClusterLetterSpacing";
    }

    void draw(SkCanvas* canvas) override {
        // Adjacent keycaps without spaces isolate inter-keycap letterSpacing from word spaces
        drawSpacingSlide(
                canvas,
                "NO TRACKED ISSUE (related: 36062)",
                "Keycap Emoji Sequences under Letter Spacing",
                "\"1\\uFE0F\\u20E3 2\\uFE0F\\u20E3 3\\uFE0F\\u20E3\": both engines keep 3 grapheme "
                "clusters, but HarfBuzz emits an extra zero-ink glyph per keycap",
                ".AppleSystemUIFont",
                36.0f,
                "1\xEF\xB8\x8F\xE2\x83\xA3"
                "2\xEF\xB8\x8F\xE2\x83\xA3"
                "3\xEF\xB8\x8F\xE2\x83\xA3",
                [](Paragraph*, Paragraph*) { return SkString(); },
                "Zero-ink U+FE0F glyphs get their own letterSpacing slot",
                "One glyph per keycap; one letterSpacing slot per cluster",
                "Apple Color Emoji morx leaves a zero-ink glyph for U+FE0F; per-glyph spacing "
                "widens every keycap twice.",
                "CoreText emits one glyph per keycap and tracks once per grapheme cluster.",
                "letterSpacing is added once per user-perceived character, never to inkless "
                "glyphs inside a cluster.");
    }
};

class ParagraphSlide_ZWJEmojiVariationSelectorLetterSpacing : public ParagraphBugSlide_Base {
public:
    ParagraphSlide_ZWJEmojiVariationSelectorLetterSpacing() {
        fName = "ParagraphSlide_ZWJEmojiVariationSelectorLetterSpacing";
    }

    void draw(SkCanvas* canvas) override {
        drawSpacingSlide(
                canvas,
                "NO TRACKED ISSUE",
                "ZWJ Emoji Sequences with U+FE0F under Letter Spacing",
                "Rainbow flag, heart on fire, woman technologist: HarfBuzz keeps zero-ink "
                "U+FE0F glyphs inside the first two sequences",
                ".AppleSystemUIFont",
                36.0f,
                "\xF0\x9F\x8F\xB3\xEF\xB8\x8F\xE2\x80\x8D\xF0\x9F\x8C\x88"
                "\xE2\x9D\xA4\xEF\xB8\x8F\xE2\x80\x8D\xF0\x9F\x94\xA5"
                "\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x92\xBB",
                [](Paragraph*, Paragraph*) { return SkString(); },
                "Zero-ink U+FE0F glyphs inflate the cluster advance",
                "One fused glyph per ZWJ sequence",
                "HarfBuzz leaves a zero-ink U+FE0F glyph in FE0F-bearing ZWJ sequences; "
                "letterSpacing widens it.",
                "CoreText fuses each ZWJ sequence into one glyph and tracks once per cluster.",
                "Inkless glyphs inside an emoji cluster must not receive inter-character "
                "spacing.");
    }
};

class ParagraphSlide_Flutter_118127_CombiningDiacriticsLetterSpacing
        : public ParagraphBugSlide_Base {
public:
    ParagraphSlide_Flutter_118127_CombiningDiacriticsLetterSpacing() {
        fName = "ParagraphSlide_Flutter_118127_CombiningDiacriticsLetterSpacing";
    }

    void draw(SkCanvas* canvas) override {
        // x + U+0301 has no precomposed NFC form, so the mark stays a separate glyph
        drawSpacingSlide(
                canvas,
                "FLUTTER ISSUE 118127",
                "Combining Acute Accent Attachment under Letter Spacing",
                "\"ax\\u0301b\" in Helvetica: the accent glyph must stay over 'x' when "
                "letterSpacing is applied",
                "Helvetica",
                40.0f,
                "ax\xCC\x81"
                "b",
                [](Paragraph* p0, Paragraph* p16) {
                    auto markOffset = [](Paragraph* p) -> SkScalar {
                        auto runs = static_cast<ParagraphImpl*>(p)->runs();
                        // "ax\u0301b" shapes 'a' at 0, base 'x' at 1, and the combining acute at 2
                        SkASSERT(!runs.empty() && runs[0].size() >= 3);
                        const Run& run = runs[0];
                        auto pos = run.positions();
                        auto off = run.offsets();
                        // offsets() is empty when every glyph offset is zero
                        auto x = [&](size_t i) {
                            return pos[i].fX + (off.empty() ? 0 : off[i].fX);
                        };
                        return x(2) - x(1);
                    };
                    SkString s;
                    s.printf("Accent drift relative to 'x': %+.1f pt",
                             markOffset(p16) - markOffset(p0));
                    return s;
                },
                "Accent glyph is shifted along with the next letter",
                "Accent keeps its mark-to-base offset over 'x'",
                "Run::addLetterSpacesEvenly shifts glyph i by i x letterSpacing, including "
                "the zero-advance mark.",
                "CoreText tracks between grapheme clusters, leaving mark positioning intact.",
                "Combining marks stay anchored to their base glyph under inter-cluster "
                "spacing.");
    }
};

class ParagraphSlide_Flutter_144528_IndicConjunctLetterSpacing : public ParagraphBugSlide_Base {
public:
    ParagraphSlide_Flutter_144528_IndicConjunctLetterSpacing() {
        fName = "ParagraphSlide_Flutter_144528_IndicConjunctLetterSpacing";
    }

    void draw(SkCanvas* canvas) override {
        drawSpacingSlide(
                canvas,
                "FLUTTER ISSUE 144528",
                "Devanagari Matra Attachment under Letter Spacing",
                "\"Hindi\" in Kohinoor Devanagari: 2 orthographic syllables shaped into 4 glyphs",
                "Kohinoor Devanagari",
                40.0f,
                "\xE0\xA4\xB9\xE0\xA4\xBF\xE0\xA4\xA8\xE0\xA5\x8D\xE0\xA4\xA6\xE0\xA5\x80",
                [](Paragraph* p0, Paragraph* p16) {
                    auto gap = [](Paragraph* p) -> SkScalar {
                        auto runs = static_cast<ParagraphImpl*>(p)->runs();
                        // The Hindi sample shapes into at least a consonant and a matra glyph
                        SkASSERT(!runs.empty() && runs[0].size() >= 2);
                        auto pos = runs[0].positions();
                        return pos[1].fX - pos[0].fX;
                    };
                    SkString s;
                    s.printf("Pre-base matra to consonant gap: %+.1f pt", gap(p16) - gap(p0));
                    return s;
                },
                "Matras pulled away from their consonants",
                "Syllables spaced as intact units",
                "Per-glyph spacing inserts a gap between each matra and the consonant it "
                "belongs to.",
                "CoreText tracks between orthographic syllables, keeping matras attached.",
                "Spacing never splits an orthographic syllable.");
    }
};

class ParagraphSlide_AppleSystemUIFontCascade : public ParagraphBugSlide_Base {
public:
    ParagraphSlide_AppleSystemUIFontCascade() { fName = "ParagraphSlide_AppleSystemUIFontCascade"; }

    void draw(SkCanvas* canvas) override {
        auto style = makeStyle(".AppleSystemUIFont", 28.0f);
        auto hb = makeParagraph(TextShapingEngine::kDefault, style, kText);
        auto ct = makeParagraph(TextShapingEngine::kCoreText, style, kText);

        drawComparisonSlide(
                canvas,
                {"NO TRACKED ISSUE",
                 "Unmapped PUA Codepoints: LastResort Tofu vs .SFNS .notdef",
                 "FIXED IN CORETEXT",
                 kGreen,
                 "Latin, CJK and Arabic resolve identically; only U+100000..U+100002 differ "
                 "(red = LastResort run, green = .notdef run)",
                 "Legacy Version: HarfBuzz (matchFamilyStyleCharacter)",
                 "Character fallback lands on LastResort, drawing [?] boxes",
                 [&](const SkRect& card) { drawRuns(canvas, card, hb.get()); },
                 "Candidate Version: CoreText (isLastResortFont suppression)",
                 "LastResort is rejected; primary font .notdef is kept",
                 [&](const SkRect& card) { drawRuns(canvas, card, ct.get()); },
                 "HarfBuzz fallback resolves unmapped PUA codepoints to LastResort, hiding them "
                 "from unresolvedGlyphs().",
                 "CoreTextLineShaper rejects LastResort and keeps .SFNS glyph 0, reported as "
                 "unresolved.",
                 "Unmapped codepoints render as the primary font .notdef and are reported, never "
                 "masked by LastResort."});
    }

private:
    // Plane-16 PUA (U+100000..U+100002) has no glyph in any system cascade font except
    // LastResort
    static constexpr char kText[] =
            "SF Pro  \xE7\xAE\x80\xE4\xBD\x93  \xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7  "
            "\xF4\x80\x80\x80\xF4\x80\x80\x81\xF4\x80\x80\x82";

    void drawRuns(SkCanvas* canvas, const SkRect& card, Paragraph* p) {
        const SkScalar listY =
                drawRunList(canvas, card, p, kText, [](size_t index, const Run& run) {
                    SkString ps;
                    run.font().getTypeface()->getPostScriptName(&ps);
                    size_t notdefs = 0;
                    for (SkGlyphID g : run.glyphs()) {
                        notdefs += g == 0 ? 1 : 0;
                    }
                    // Red flags LastResort leaks (HarfBuzz); green marks primary-font .notdef
                    const SkColor color = ps.contains("LastResort") ? kRed
                                          : notdefs > 0             ? kGreen
                                                                    : kBlue;
                    SkString line;
                    line.printf("Run %zu: %s, %zu glyphs, %zu .notdef",
                                index,
                                ps.c_str(),
                                run.size(),
                                notdefs);
                    return RunRow{color, color != kBlue, line};
                });
        SkString summary;
        summary.printf("unresolvedGlyphs() = %d", p->unresolvedGlyphs());
        drawText(canvas, summary.c_str(), cardPen(card, 0).x, listY + 10.0f, kDark, true, 13.0f);
    }
};

class ParagraphSlide_Flutter_147708_CupertinoAliases : public ParagraphBugSlide_Base {
public:
    ParagraphSlide_Flutter_147708_CupertinoAliases() {
        fName = "ParagraphSlide_Flutter_147708_CupertinoAliases";
    }

    void draw(SkCanvas* canvas) override {
        AliasLayouts hb = layoutAliases(TextShapingEngine::kDefault);
        AliasLayouts ct = layoutAliases(TextShapingEngine::kCoreText);

        drawComparisonSlide(
                canvas,
                {"FLUTTER ISSUE 147708",
                 "CupertinoSystemText & CupertinoSystemDisplay with fontFamilyFallback",
                 "FIXED IN CORETEXT",
                 kGreen,
                 "When fontFamilyFallback is non-empty, Cupertino aliases must resolve to .SFNS "
                 "instead of falling through to the first fallback family",
                 "Legacy Version: HarfBuzz (matchFamilyStyle Loop)",
                 "Misses CupertinoSystemText/Display and falls through to Helvetica",
                 [&](const SkRect& card) { drawAliasCard(canvas, card, hb, nullptr, kRed); },
                 "Candidate Version: CoreText (CoreTextFontRegistry)",
                 "Resolves Cupertino aliases directly to .SFNS with display optical sizing",
                 [&](const SkRect& card) { drawAliasCard(canvas, card, ct, &hb, kGreen); },
                 "matchFamilyStyle has no entry for CupertinoSystemText/Display, so findTypefaces "
                 "skips them and picks the first fontFamilyFallback face (Helvetica).",
                 "CoreTextFontRegistry::isSystemUiFace recognizes both Cupertino aliases and "
                 "instantiates .SFNS at the requested point size.",
                 "Cupertino system font aliases resolve as primary system UI faces before any "
                 "fallback family is consulted."});
    }

private:
    struct AliasLayouts {
        std::unique_ptr<Paragraph> text;
        std::unique_ptr<Paragraph> display;
    };

    static TextStyle styleWithFallback(const char* family, SkScalar fontSize) {
        TextStyle ts;
        // Pair each alias with Helvetica so an unrecognized alias visibly falls through to it
        ts.setFontFamilies({SkString(family), SkString("Helvetica")});
        ts.setFontSize(fontSize);
        ts.setColor(kInk);
        return ts;
    }

    AliasLayouts layoutAliases(TextShapingEngine engine) {
        const SkScalar kNoWrap = 2000.0f;
        return {makeParagraph(engine,
                              styleWithFallback("CupertinoSystemText", 14.0f),
                              kTrackingText,
                              kNoWrap),
                makeParagraph(engine,
                              styleWithFallback("CupertinoSystemDisplay", 28.0f),
                              kTrackingText,
                              kNoWrap)};
    }

    // harfBuzz is null on the HarfBuzz card; on the CoreText card it supplies the dashed widths
    void drawAliasCard(SkCanvas* canvas,
                       const SkRect& card,
                       const AliasLayouts& rows,
                       const AliasLayouts* harfBuzz,
                       SkColor accent) {
        auto [x, y, maxWidth] = cardPen(card, 56.0f);
        auto impl = static_cast<ParagraphImpl*>(rows.display.get());
        // Non-empty sample text shaped for the alias rows always produces at least one run
        SkASSERT(!impl->runs().empty());
        SkString ps;
        impl->runs()[0].font().getTypeface()->getPostScriptName(&ps);
        SkString label14;
        label14.printf("14 pt {CupertinoSystemText, Helvetica} -> %s:", ps.c_str());
        y = drawWidthRow(canvas,
                         rows.text.get(),
                         x,
                         y,
                         maxWidth,
                         14.0f,
                         label14.c_str(),
                         accent,
                         harfBuzz ? harfBuzz->text->getLongestLine() : 0.0f);
        SkString label28;
        label28.printf("28 pt {CupertinoSystemDisplay, Helvetica} -> %s:", ps.c_str());
        y = drawWidthRow(canvas,
                         rows.display.get(),
                         x,
                         y,
                         maxWidth,
                         28.0f,
                         label28.c_str(),
                         accent,
                         harfBuzz ? harfBuzz->display->getLongestLine() : 0.0f);
        SkString ratio;
        ratio.printf("Width ratio 28 pt / 14 pt = %.3f (resolved face: %s)",
                     rows.display->getLongestLine() / rows.text->getLongestLine(),
                     ps.c_str());
        drawText(canvas, ratio.c_str(), x, y, accent, true, 13.0f);
    }
};

class ParagraphSlide_Flutter_193812_SystemDesigns : public ParagraphBugSlide_Base {
public:
    ParagraphSlide_Flutter_193812_SystemDesigns() {
        fName = "ParagraphSlide_Flutter_193812_SystemDesigns";
    }

    void draw(SkCanvas* canvas) override {
        // Both engines are laid out first so each card compares widths against CoreText at 22 pt
        const DesignLayouts hb = layoutDesigns(TextShapingEngine::kDefault);
        const DesignLayouts ct = layoutDesigns(TextShapingEngine::kCoreText);

        drawComparisonSlide(
                canvas,
                {"FLUTTER ISSUE 193812",
                 "Apple System Font Design Variants: Rounded, Serif, Monospaced",
                 "TRAK/OPSZ ONLY",
                 kAmber,
                 ".AppleSystemUIFont{Rounded,Serif,Monospaced} at 22 pt: Rounded and Serif require "
                 "size-instantiated 'trak' and 'opsz'",
                 "Legacy Version: HarfBuzz (Default Font Manager)",
                 "Size-0 CTFont without 'trak': Rounded (+22.6 pt) and Serif (opsz 12, +17.3 pt) "
                 "are too wide",
                 [&](const SkRect& card) {
                     drawEngineCard(canvas, card, hb, ct, /*coreText=*/false, kRed);
                 },
                 "Candidate Version: CoreText (NSCTFontUIFontDesignTrait)",
                 "22 pt UI design CTFont applies 'trak' and 'opsz'; dashed red = HarfBuzz excess",
                 [&](const SkRect& card) {
                     drawEngineCard(canvas, card, hb, ct, /*coreText=*/true, kGreen);
                 },
                 "SkFontMgr_mac_ct creates CTFonts at size 0 without 'trak', freezing .NewYork "
                 "opsz at 12 and widening Rounded and Serif.",
                 "CoreTextFontRegistry applies NSCTFontUIFontDesignTrait at the layout point size, "
                 "preserving both 'opsz' and 'trak'.",
                 "System UI design variants must be instantiated at the target point size so "
                 "optical sizing and tracking tables apply."});
    }

private:
    struct DesignVariant {
        const char* family;
        const char* expectedPs;
    };
    static constexpr std::array<DesignVariant, 3> kDesigns = {{
            {".AppleSystemUIFontRounded", "Rounded"},
            {".AppleSystemUIFontSerif", "NewYork"},
            {".AppleSystemUIFontMonospaced", "Monospaced"},
    }};

    // One engine's 22 pt design rows plus the fixed-pitch probe pair
    struct DesignLayouts {
        std::array<std::unique_ptr<Paragraph>, 3> designs;
        std::unique_ptr<Paragraph> narrow;
        std::unique_ptr<Paragraph> wide;
    };

    DesignLayouts layoutDesigns(TextShapingEngine engine) {
        DesignLayouts l;
        for (size_t i = 0; i < kDesigns.size(); ++i) {
            l.designs[i] = makeParagraph(engine,
                                         makeStyle(kDesigns[i].family, 22.0f),
                                         "System UI Design: 0123456789 iiiWWW");
        }
        auto mono = makeStyle(".AppleSystemUIFontMonospaced", 20.0f);
        l.narrow = makeParagraph(engine, mono, "iiiiiiii");
        l.wide = makeParagraph(engine, mono, "WWWWWWWW");
        return l;
    }

    // A label wider than the card shrinks to fit, so the numbers stay inside a narrow window
    void drawFittedText(SkCanvas* canvas,
                        const SkString& str,
                        SkScalar x,
                        SkScalar y,
                        SkScalar maxWidth,
                        SkColor color) {
        SkScalar fontSize = 12.0f;
        SkFont font(ToolUtils::DefaultTypeface(), fontSize);
        font.setEmbolden(true);
        const SkScalar width = font.measureText(str.c_str(), str.size(), SkTextEncoding::kUTF8);
        if (width > maxWidth) {
            fontSize *= maxWidth / width;
        }
        drawText(canvas, str.c_str(), x, y, color, true, fontSize);
    }

    // Draws design i of the selected engine at (x, y) and returns the y below it
    SkScalar drawDesignRow(SkCanvas* canvas,
                           size_t i,
                           const DesignLayouts& hb,
                           const DesignLayouts& ct,
                           bool coreText,
                           SkScalar x,
                           SkScalar y,
                           SkScalar maxWidth) {
        Paragraph* p = (coreText ? ct : hb).designs[i].get();
        const SkScalar w = p->getLongestLine();
        const SkScalar hbW = hb.designs[i]->getLongestLine();
        const SkScalar ctW = ct.designs[i]->getLongestLine();
        auto impl = static_cast<ParagraphImpl*>(p);
        // Non-empty system design sample text always produces at least one run
        SkASSERT(!impl->runs().empty());
        SkString ps;
        SkScalar opsz = 0, minOpsz = 0, maxOpsz = 0;
        const SkTypeface* tf = impl->runs()[0].font().getTypeface();
        tf->getPostScriptName(&ps);
        const bool hasOpsz =
                axisState(tf, SkSetFourByteTag('o', 'p', 's', 'z'), &opsz, &minOpsz, &maxOpsz);
        // Rounded and Serif carry size-dependent 'trak' (and .NewYork 'opsz'), so only
        // layouts whose face and width match the 22 pt CoreText target pass
        const bool rowOk = ps.contains(kDesigns[i].expectedPs) && SkScalarNearlyEqual(w, ctW, 0.5f);
        const SkColor rowColor = rowOk ? kGreen : kRed;
        SkString face;
        face.printf("%s -> %s", kDesigns[i].family, ps.c_str());
        SkString metrics;
        metrics.printf("width %.1f pt", w);
        if (hasOpsz) {
            metrics.appendf(", opsz %.0f", opsz);
        }
        if (!coreText && w > ctW + 0.5f) {
            metrics.appendf(", +%.1f pt: no trak", w - ctW);
        } else if (coreText && hbW > w + 0.5f) {
            metrics.appendf(", -%.1f pt via trak", hbW - w);
        }
        drawFittedText(canvas, face, x, y, maxWidth, rowColor);
        drawFittedText(canvas, metrics, x, y + 15.0f, maxWidth, rowColor);
        y += 25.0f;
        // The widths above come from the 22 pt layouts; only the drawing scales down when
        // the line and the HarfBuzz excess frame are wider than the card
        const bool showExcess = coreText && hbW > w + 0.5f;
        const SkScalar extent = showExcess ? hbW : w;
        const SkScalar scale = extent > maxWidth ? maxWidth / extent : 1.0f;
        // Highlight the excess width that HarfBuzz adds when 'trak' and 'opsz' are missing
        paintScaledRow(canvas, p, x, y, scale, rowColor, coreText ? hbW : w);
        return y + p->getHeight() * scale + 24.0f;
    }

    void drawEngineCard(SkCanvas* canvas,
                        const SkRect& card,
                        const DesignLayouts& hb,
                        const DesignLayouts& ct,
                        bool coreText,
                        SkColor accent) {
        auto [x, y, maxWidth] = cardPen(card, 54.0f);
        for (size_t i = 0; i < kDesigns.size(); ++i) {
            y = drawDesignRow(canvas, i, hb, ct, coreText, x, y, maxWidth);
        }
        const DesignLayouts& rows = coreText ? ct : hb;
        // SF Mono has no optical size or size-tracking tables, so both engines match on the
        // fixed-pitch check
        const SkScalar narrow = rows.narrow->getLongestLine();
        const SkScalar wide = rows.wide->getLongestLine();
        SkString monoInfo;
        monoInfo.printf(
                "Monospaced check: \"iiiiiiii\" = %.1f pt vs \"WWWWWWWW\" = %.1f pt "
                "(no opsz/trak, matches)",
                narrow,
                wide);
        drawFittedText(canvas,
                       monoInfo,
                       x,
                       y + 2.0f,
                       maxWidth,
                       SkScalarNearlyEqual(narrow, wide, 0.05f) ? kGreen : kRed);

        const SkScalar roundedDelta =
                hb.designs[0]->getLongestLine() - ct.designs[0]->getLongestLine();
        const SkScalar serifDelta =
                hb.designs[1]->getLongestLine() - ct.designs[1]->getLongestLine();
        SkString deltaInfo;
        if (!coreText) {
            deltaInfo.printf(
                    "Rounded/Serif excess vs CoreText: +%.1f pt / +%.1f pt "
                    "(size-0 CTFont omits trak & opsz)",
                    roundedDelta,
                    serifDelta);
        } else {
            deltaInfo.printf(
                    "Rounded/Serif tightened by -%.1f pt / -%.1f pt at 22 pt "
                    "(dashed red = HarfBuzz excess)",
                    roundedDelta,
                    serifDelta);
        }
        drawFittedText(canvas, deltaInfo, x, y + 22.0f, maxWidth, accent);
    }
};

// Same string and sizes as SkParagraph_Flutter_109308_AutoOpticalSize, so the slide shows the
// values the test asserts
constexpr char kOpszText[] = "Variable fonts honor axes";

class ParagraphSlide_Flutter_109308_AutoOpticalSize : public ParagraphBugSlide_Base {
public:
    ParagraphSlide_Flutter_109308_AutoOpticalSize() {
        fName = "ParagraphSlide_Flutter_109308_AutoOpticalSize";
    }

    void draw(SkCanvas* canvas) override {
        OpszPair hb = {measure(TextShapingEngine::kDefault, kSizes[0]),
                       measure(TextShapingEngine::kDefault, kSizes[1])};
        OpszPair ct = {measure(TextShapingEngine::kCoreText, kSizes[0]),
                       measure(TextShapingEngine::kCoreText, kSizes[1])};
        const bool hbAuto = autoOpsz(hb);
        const bool ctAuto = autoOpsz(ct);
        const char* status = ctAuto && !hbAuto ? "PARTIAL FIX: AUTO OPTICAL SIZE ONLY"
                             : ctAuto          ? "NO ENGINE DIFFERENCE"
                                               : "NOT FIXED";
        SkColor statusColor = ctAuto && !hbAuto ? kAmber : ctAuto ? kMuted : kRed;

        SkString hbSubtitle;
        hbSubtitle.printf("opsz %.1f at 12 pt and %.1f at 72 pt", hb[0].opsz, hb[1].opsz);
        SkString ctSubtitle;
        ctSubtitle.printf("opsz %.1f at 12 pt and %.1f at 72 pt; dashed red = extra HarfBuzz width",
                          ct[0].opsz,
                          ct[1].opsz);
        drawComparisonSlide(
                canvas,
                {"FLUTTER ISSUE 109308 (PARTIAL)",
                 "Variable Fonts: Automatic Optical Size of .AppleSystemUIFont",
                 status,
                 statusColor,
                 "The issue asks for full variable font support; this slide covers only the "
                 "automatic opsz axis following the font size (12 pt vs 72 pt)",
                 "Legacy Version: HarfBuzz (SkCTFontCreateExactCopy)",
                 hbSubtitle.c_str(),
                 [&](const SkRect& card) { drawOpszCard(canvas, card, hb, nullptr, kRed); },
                 "Candidate Version: CoreText (.SFNS at the requested size)",
                 ctSubtitle.c_str(),
                 [&](const SkRect& card) { drawOpszCard(canvas, card, ct, &hb, kGreen); },
                 "The size-frozen CTFont copy keeps one opsz coordinate, so glyph advances scale "
                 "linearly with the point size.",
                 "CoreText instantiates .SFNS at the requested size, so opsz follows it; explicit "
                 "axes and other variable font requests stay out of scope.",
                 "Unless the caller sets opsz explicitly, the opsz coordinate follows the font "
                 "size."});
    }

private:
    static constexpr std::array<SkScalar, 2> kSizes = {12.0f, 72.0f};

    struct OpszMeasure {
        std::unique_ptr<Paragraph> para;
        SkScalar advancePerEm = 0;
        SkScalar opsz = 0;
        SkScalar min = 0;
        SkScalar max = 0;
        bool hasAxis = false;
    };
    using OpszPair = std::array<OpszMeasure, 2>;

    OpszMeasure measure(TextShapingEngine engine, SkScalar size) {
        OpszMeasure m;
        m.para = makeParagraph(engine, makeStyle(".AppleSystemUIFont", size), kOpszText, 2000.0f);
        auto impl = static_cast<ParagraphImpl*>(m.para.get());
        // Non-empty sample text shaped for opsz measurement always produces at least one run
        SkASSERT(!impl->runs().empty());
        const Run& run = impl->runs()[0];
        m.advancePerEm = glyphAdvanceSum(run) / size;
        m.hasAxis = axisState(run.font().getTypeface(),
                              SkSetFourByteTag('o', 'p', 's', 'z'),
                              &m.opsz,
                              &m.min,
                              &m.max);
        return m;
    }

    // Same criterion as the test: small optical sizes are drawn wider per em
    static bool autoOpsz(const OpszPair& m) {
        return m[0].advancePerEm > m[1].advancePerEm * 1.01f;
    }

    // harfBuzz is null on the HarfBuzz card; on the CoreText card it supplies the dashed widths
    void drawOpszCard(SkCanvas* canvas,
                      const SkRect& card,
                      const OpszPair& m,
                      const OpszPair* harfBuzz,
                      SkColor accent) {
        auto [x, y, maxWidth] = cardPen(card, 56.0f);
        for (size_t i = 0; i < kSizes.size(); ++i) {
            SkString label;
            if (m[i].hasAxis) {
                label.printf("%.0f pt: opsz = %.1f (axis %.0f..%.0f), glyph advances %.3f/em",
                             kSizes[i],
                             m[i].opsz,
                             m[i].min,
                             m[i].max,
                             m[i].advancePerEm);
            } else {
                label.printf("%.0f pt: rendered face has no opsz axis, glyph advances %.3f/em",
                             kSizes[i],
                             m[i].advancePerEm);
            }
            y = drawWidthRow(canvas,
                             m[i].para.get(),
                             x,
                             y,
                             maxWidth,
                             kSizes[i],
                             label.c_str(),
                             accent,
                             harfBuzz ? (*harfBuzz)[i].para->getLongestLine() : 0.0f);
        }
        SkString ratio;
        ratio.printf("Glyph advances per em, 12 pt / 72 pt = %.3f (fixed opsz = 1.000)",
                     m[0].advancePerEm / m[1].advancePerEm);
        drawText(canvas, ratio.c_str(), x, y, accent, true, 13.0f);
        drawText(canvas,
                 autoOpsz(m) ? "Automatic optical size: applied"
                             : "Automatic optical size: not applied",
                 x,
                 y + 22.0f,
                 accent,
                 true,
                 13.0f);
    }
};

class ParagraphSlide_Flutter_144099_CupertinoArabic : public ParagraphBugSlide_Base {
public:
    ParagraphSlide_Flutter_144099_CupertinoArabic() {
        fName = "ParagraphSlide_Flutter_144099_CupertinoArabic";
    }

    void draw(SkCanvas* canvas) override {
        // Same text and locale as SkParagraph_Flutter_144099_CupertinoArabicSystemFont
        TextStyle style = makeStyle("CupertinoSystemText", 32.0f);
        style.setLocale(SkString("ar"));
        auto hb = makeParagraph(TextShapingEngine::kDefault, style, kText);
        auto ct = makeParagraph(TextShapingEngine::kCoreText, style, kText);
        const bool hbOk = arabicOnSystemFace(hb.get());
        const bool ctOk = arabicOnSystemFace(ct.get());
        const char* status = ctOk && !hbOk ? "FIXED IN CORETEXT"
                             : ctOk        ? "NO ENGINE DIFFERENCE"
                                           : "NOT FIXED";
        SkColor statusColor = ctOk && !hbOk ? kGreen : ctOk ? kMuted : kRed;

        drawComparisonSlide(
                canvas,
                {"FLUTTER ISSUE 144099",
                 "Arabic Text in CupertinoSystemText: System Arabic Face",
                 status,
                 statusColor,
                 "\"Marhaba 11:40\" with locale ar: Arabic runs must render in SF Arabic, "
                 "as native iOS text does (green = SF Arabic, red = other face, blue = common "
                 "script)",
                 "Legacy Version: HarfBuzz (matchFamilyStyleCharacter)",
                 "Alias not resolved: common script lands in the default family",
                 [&](const SkRect& card) { drawRuns(canvas, card, hb.get()); },
                 "Candidate Version: CoreText (.AppleSystemUIFont cascade)",
                 "CupertinoSystemText becomes the system UI CTFont",
                 [&](const SkRect& card) { drawRuns(canvas, card, ct.get()); },
                 "CupertinoSystemText is not a system alias on this path, so Arabic falls back by "
                 "coverage to a non-system face.",
                 "CoreTextFontRegistry creates the .AppleSystemUIFont CTFont, whose own cascade "
                 "list supplies SF Arabic.",
                 "Script fallback for a system UI font alias follows the system cascade, matching "
                 "native platform text."});
    }

private:
    static constexpr char kText[] =
            "\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7 \xD9\xA1\xD9\xA1:\xD9\xA4\xD9\xA0";

    // Space and ':' are common-script and may stay in the Latin face; every Arabic letter
    // and Arabic-Indic digit here is a 0xD8/0xD9-led UTF-8 sequence
    static bool isArabicRun(const Run& run) {
        const unsigned char lead = kText[run.textRange().start];
        return lead == 0xD8 || lead == 0xD9;
    }

    static bool arabicOnSystemFace(Paragraph* p) {
        auto impl = static_cast<ParagraphImpl*>(p);
        bool ok = !impl->runs().empty();
        for (const Run& run : impl->runs()) {
            SkString family;
            run.font().getTypeface()->getFamilyName(&family);
            if (isArabicRun(run)) {
                ok = ok && family.contains("SF Arabic");
            }
        }
        return ok;
    }

    void drawRuns(SkCanvas* canvas, const SkRect& card, Paragraph* p) {
        drawRunList(canvas, card, p, kText, [](size_t index, const Run& run) {
            SkString family;
            run.font().getTypeface()->getFamilyName(&family);
            const bool arabic = isArabicRun(run);
            const SkColor color = !arabic ? kBlue : family.contains("SF Arabic") ? kGreen : kRed;
            SkString line;
            line.printf("Run %zu: bytes [%zu..%zu) %s -> %s",
                        index,
                        run.textRange().start,
                        run.textRange().end,
                        arabic ? "Arabic" : "common",
                        family.c_str());
            return RunRow{color, color == kRed, line};
        });
    }
};

class ParagraphSlide_ArabicCursiveTracking : public ParagraphBugSlide_Base {
public:
    ParagraphSlide_ArabicCursiveTracking() { fName = "ParagraphSlide_ArabicCursiveTracking"; }

    void draw(SkCanvas* canvas) override {
        CursiveLayouts hb = layoutCursive(TextShapingEngine::kDefault);
        CursiveLayouts ct = layoutCursive(TextShapingEngine::kCoreText);
        const bool hbOk = spacingIgnored(hb);
        const bool ctOk = spacingIgnored(ct);
        const char* status = ctOk && hbOk ? "FIXED: CORETEXT MATCHES HARFBUZZ"
                             : ctOk       ? "FIXED IN CORETEXT"
                                          : "NOT FIXED";
        SkColor statusColor = ctOk ? kGreen : kRed;

        drawComparisonSlide(
                canvas,
                {"SKIA B/458132646",
                 "Arabic Text under letterSpacing: HarfBuzz vs CoreText",
                 status,
                 statusColor,
                 "Already fixed on the HarfBuzz path, CoreText path now matches: \"Marhaba "
                 "bil-alam\" at 36 pt, letterSpacing 0 and 10",
                 "Legacy Version: HarfBuzz (Run::isCursiveScript)",
                 "Cursive runs skip letterSpacing entirely",
                 [&](const SkRect& card) { drawEngineCard(canvas, card, hb, kRed); },
                 "Candidate Version: CoreText (Run::isCursiveScript)",
                 "Spacing is applied only to non-cursive script runs",
                 [&](const SkRect& card) { drawEngineCard(canvas, card, ct, kGreen); },
                 "ParagraphImpl skips spacing for cursive scripts, so Arabic width does not change "
                 "with letterSpacing.",
                 "ParagraphImpl adds no spacing to cursive script runs, so SF Arabic and Geeza Pro "
                 "keep the same glyphs and width.",
                 "Spacing inside a cursive word never breaks the joins between its letters."});
    }

private:
    // "Marhaba bil-alam": every word has joined letters, so any gap inside a word is a break
    static constexpr char kText[] =
            "\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7 "
            "\xD8\xA8\xD8\xA7\xD9\x84\xD8\xB9\xD8\xA7\xD9\x84\xD9\x85";
    static constexpr std::array<const char*, 2> kFamilies = {".AppleSystemUIFont", "Geeza Pro"};
    static constexpr SkScalar kArabicSpacing = 10.0f;

    // One family laid out at letterSpacing 0 and kArabicSpacing
    struct CursivePair {
        std::unique_ptr<Paragraph> p0;
        std::unique_ptr<Paragraph> p10;
    };
    using CursiveLayouts = std::array<CursivePair, 2>;

    CursiveLayouts layoutCursive(TextShapingEngine engine) {
        CursiveLayouts l;
        for (size_t i = 0; i < kFamilies.size(); ++i) {
            l[i].p0 = makeParagraph(engine, makeStyle(kFamilies[i], 36.0f), kText);
            l[i].p10 = makeParagraph(engine, makeStyle(kFamilies[i], 36.0f, kArabicSpacing), kText);
        }
        return l;
    }

    static std::vector<SkGlyphID> glyphsOf(Paragraph* p) {
        std::vector<SkGlyphID> glyphs;
        for (const Run& run : static_cast<ParagraphImpl*>(p)->runs()) {
            glyphs.insert(glyphs.end(), run.glyphs().begin(), run.glyphs().end());
        }
        return glyphs;
    }

    // Same contract as CoreTextPipeline_LetterSpacing_CursiveArabicSystemFonts: spacing must
    // leave both the glyph sequence and the width of cursive text unchanged
    static bool spacingIgnored(const CursiveLayouts& l) {
        bool ok = true;
        for (const CursivePair& pair : l) {
            // Compare getMaxIntrinsicWidth() because getLongestLine() wraps once +10pt per
            // HarfBuzz glyph overflows the 620pt layout width
            ok = ok && glyphsOf(pair.p0.get()) == glyphsOf(pair.p10.get()) &&
                 SkScalarNearlyEqual(pair.p0->getMaxIntrinsicWidth(),
                                     pair.p10->getMaxIntrinsicWidth());
        }
        return ok;
    }

    void drawEngineCard(SkCanvas* canvas,
                        const SkRect& card,
                        const CursiveLayouts& l,
                        SkColor accent) {
        SkScalar x = card.fLeft + 20.0f;
        SkScalar y = card.fTop + 52.0f;
        for (size_t f = 0; f < kFamilies.size(); ++f) {
            std::vector<SkGlyphID> g0 = glyphsOf(l[f].p0.get());
            std::vector<SkGlyphID> g10 = glyphsOf(l[f].p10.get());
            // Glyphs that appear only under spacing are inserted extenders (kashida)
            size_t inserted = 0;
            for (SkGlyphID g : g10) {
                inserted += std::find(g0.begin(), g0.end(), g) == g0.end() ? 1 : 0;
            }
            const std::array<Paragraph*, 2> rows = {l[f].p0.get(), l[f].p10.get()};
            const std::array<SkScalar, 2> spacings = {0.0f, kArabicSpacing};
            for (int i = 0; i < 2; ++i) {
                Paragraph* p = rows[i];
                SkString label;
                label.printf("%s, letterSpacing %.0f: width %.2f pt, %zu glyphs",
                             kFamilies[f],
                             spacings[i],
                             p->getMaxIntrinsicWidth(),
                             i == 0 ? g0.size() : g10.size());
                if (i == 1) {
                    label.appendf(" (%zu inserted)", inserted);
                }
                drawText(canvas, label.c_str(), x, y, kDark, true);
                y += 6.0f;
                p->paint(canvas, x, y);
                drawFrame(canvas,
                          SkRect::MakeXYWH(x, y, p->getMaxIntrinsicWidth(), p->getHeight()),
                          accent);
                y += p->getHeight() + 22.0f;
            }
            y += 6.0f;
        }
    }
};

}  // namespace

DEF_SLIDE(return new ParagraphSlide_BugsOverview();)
DEF_SLIDE(return new ParagraphSlide_Flutter_150824_DynamicTracking();)
DEF_SLIDE(return new ParagraphSlide_Flutter_18915_OpticalSize();)
DEF_SLIDE(return new ParagraphSlide_Flutter_147708_CupertinoAliases();)
DEF_SLIDE(return new ParagraphSlide_Flutter_193812_SystemDesigns();)
DEF_SLIDE(return new ParagraphSlide_Flutter_22572_DefaultTextTrackingHIGDelta();)
DEF_SLIDE(return new ParagraphSlide_KeycapEmojiClusterLetterSpacing();)
DEF_SLIDE(return new ParagraphSlide_ZWJEmojiVariationSelectorLetterSpacing();)
DEF_SLIDE(return new ParagraphSlide_Flutter_118127_CombiningDiacriticsLetterSpacing();)
DEF_SLIDE(return new ParagraphSlide_Flutter_144528_IndicConjunctLetterSpacing();)
DEF_SLIDE(return new ParagraphSlide_AppleSystemUIFontCascade();)
DEF_SLIDE(return new ParagraphSlide_Flutter_109308_AutoOpticalSize();)
DEF_SLIDE(return new ParagraphSlide_Flutter_144099_CupertinoArabic();)
DEF_SLIDE(return new ParagraphSlide_ArabicCursiveTracking();)
