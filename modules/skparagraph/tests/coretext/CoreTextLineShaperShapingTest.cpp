/*
 * Copyright 2026 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkFont.h"
#include "include/core/SkRefCnt.h"
#include "include/core/SkScalar.h"
#include "include/core/SkString.h"
#include "include/core/SkTypeface.h"
#include "include/core/SkTypes.h"
#include "modules/skparagraph/include/DartTypes.h"
#include "modules/skparagraph/include/FontCollection.h"
#include "modules/skparagraph/include/ParagraphPainter.h"
#include "modules/skparagraph/include/ParagraphStyle.h"
#include "modules/skparagraph/include/TextStyle.h"
#include "modules/skparagraph/include/TypefaceFontProvider.h"
#include "modules/skparagraph/src/CoreTextAttrStringBuilder.h"
#include "modules/skparagraph/src/CoreTextIndexMap.h"
#include "modules/skparagraph/src/CoreTextLineShaper.h"
#include "modules/skparagraph/src/ParagraphBuilderImpl.h"
#include "modules/skparagraph/src/ParagraphImpl.h"
#include "modules/skparagraph/src/Run.h"
#include "modules/skparagraph/tests/coretext/CoreTextTestUtils.h"
#include "modules/skunicode/include/SkUnicode.h"
#include "src/core/SkTextBlobPriv.h"
#include "src/core/SkUTF.h"
#include "src/utils/mac/SkUniqueCFRef.h"
#include "tests/Test.h"
#include "tools/fonts/FontToolUtils.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>
#include <string.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace skia::textlayout;
using skia::textlayout::coretext_test::coretext_line;
using skia::textlayout::coretext_test::get_unicode;
using skia::textlayout::coretext_test::has_font_families;
using skia::textlayout::coretext_test::last_blob_x;
using skia::textlayout::coretext_test::layout_ahem;
using skia::textlayout::coretext_test::layout_single_style;
using skia::textlayout::coretext_test::make_ahem_collection;
using skia::textlayout::coretext_test::make_roboto_collection;
using skia::textlayout::coretext_test::painted_glyphs;
using skia::textlayout::coretext_test::PaintedGlyph;
using skia::textlayout::coretext_test::PlainLine;
using skia::textlayout::coretext_test::test_font_collection;
using skia::textlayout::coretext_test::test_unicode;

namespace {
// Run::offsets() is empty when every glyph offset is zero, and has no entry for the end
// sentinel, whose offset is zero
SkPoint glyph_offset(const Run& run, size_t k) {
    return k < run.offsets().size() ? run.offsets()[k] : SkPoint{0, 0};
}
}  // namespace

// Tests assert the internal ParagraphImpl::isCoreTextShaped(): no public API reports the engine,
// and a silent HarfBuzz fallback would pass every other assert

// U+FEFF at offset 0 is a ZWNBSP for SkUnicode, so with Apple's SkUnicode the kCoreText paragraph
// gets the same graphemes and words as without it, one character later
DEF_TEST(CoreTextPipeline_LeadingZwnbsp_BoundariesShiftByOneCharacter, reporter) {
    constexpr size_t kZwnbspUtf8 = 3;
    constexpr size_t kZwnbspUtf16 = 1;
    const char* plainText = "ab\u0301c def";
    const char* zwnbspText = "\uFEFFab\u0301c def";
    auto layout = [](const char* text) {
        ParagraphStyle paragraphStyle;
        paragraphStyle.setTextShapingEngine(TextShapingEngine::kCoreText);
        TextStyle style;
        style.setFontSize(20.0f);
        ParagraphBuilderImpl builder(
                paragraphStyle, test_font_collection(), get_unicode(paragraphStyle));
        builder.pushStyle(style);
        builder.addText(text);
        auto paragraph = builder.Build();
        paragraph->layout(1000.0f);
        return paragraph;
    };
    auto plain = layout(plainText);
    auto zwnbsp = layout(zwnbspText);
    auto plainImpl = static_cast<ParagraphImpl*>(plain.get());
    auto zwnbspImpl = static_cast<ParagraphImpl*>(zwnbsp.get());
    REPORTER_ASSERT(reporter, plainImpl->isCoreTextShaped() && zwnbspImpl->isCoreTextShaped());

    // Code unit flags are indexed in UTF-8
    const size_t plainUtf8 = strlen(plainText);
    for (size_t i = 0; i <= plainUtf8; ++i) {
        bool plainStart =
                plainImpl->codeUnitHasProperty(i, SkUnicode::CodeUnitFlags::kGraphemeStart);
        bool zwnbspStart = zwnbspImpl->codeUnitHasProperty(
                i + kZwnbspUtf8, SkUnicode::CodeUnitFlags::kGraphemeStart);
        REPORTER_ASSERT(reporter,
                        plainStart == zwnbspStart,
                        "grapheme start at %zu: %d vs %d",
                        i,
                        plainStart,
                        zwnbspStart);
    }

    // getWordBoundary takes and returns UTF-16 offsets
    const int plainUtf16 = SkUTF::UTF8ToUTF16(nullptr, 0, plainText, plainUtf8);
    for (unsigned i = 0; i <= SkToUInt(plainUtf16); ++i) {
        SkRange<size_t> plainWord = plain->getWordBoundary(i);
        SkRange<size_t> zwnbspWord = zwnbsp->getWordBoundary(i + kZwnbspUtf16);
        REPORTER_ASSERT(reporter,
                        zwnbspWord.start == plainWord.start + kZwnbspUtf16 &&
                                zwnbspWord.end == plainWord.end + kZwnbspUtf16,
                        "word at %u: [%zu, %zu) vs [%zu, %zu)",
                        i,
                        plainWord.start,
                        plainWord.end,
                        zwnbspWord.start,
                        zwnbspWord.end);
    }
}

// With fallback disabled every glyph is a .notdef box. Default_Ignorable code points (Hangul
// fillers too) and the BN controls a CTLine drops add no width and, alone in a run, paint nothing;
// Mn/Me marks add no width but keep a box, as U+001C..U+001F and U+0085 do. Marks no font has are
// unresolved like HarfBuzz reports them, controls and ignorables are not
DEF_TEST(CoreTextPipeline_FallbackBoxesSkipInvisibleCodepoints, reporter) {
    auto fontCollection = test_font_collection();
    fontCollection->disableFontFallback();
    ParagraphStyle paragraphStyle;
    paragraphStyle.setTextShapingEngine(TextShapingEngine::kCoreText);
    TextStyle style;
    style.setFontSize(16.0f);
    // Lays out "A" + between + "B", or "AB" when between is 0
    auto layout = [&](SkUnichar between) {
        std::string text = "A";
        char buffer[SkUTF::kMaxBytesInUTF8Sequence];
        if (between) {
            text.append(buffer, SkUTF::ToUTF8(between, buffer));
        }
        text += "B";
        ParagraphBuilderImpl builder(paragraphStyle, fontCollection, get_unicode(paragraphStyle));
        builder.pushStyle(style);
        builder.addText(text.c_str(), text.size());
        auto paragraph = builder.Build();
        paragraph->layout(1000.0f);
        return paragraph;
    };
    const SkScalar baseWidth = layout(0)->getMaxIntrinsicWidth();
    REPORTER_ASSERT(reporter, baseWidth > 0, "got %g", baseWidth);

    enum class Kind { kControl, kIgnorable, kMark, kVisible, kVisibleControl };
    const std::pair<SkUnichar, Kind> codepoints[] = {
            {0x0001, Kind::kControl},        {0x001B, Kind::kControl},
            {0x007F, Kind::kControl},        {0x0084, Kind::kControl},
            {0x0086, Kind::kControl},        {0x009F, Kind::kControl},
            {0x00AD, Kind::kIgnorable},      {0x034F, Kind::kIgnorable},
            {0x061C, Kind::kIgnorable},      {0x115F, Kind::kIgnorable},
            {0x200B, Kind::kIgnorable},      {0x200D, Kind::kIgnorable},
            {0x202A, Kind::kIgnorable},      {0x2060, Kind::kIgnorable},
            {0x206A, Kind::kIgnorable},      {0x3164, Kind::kIgnorable},
            {0xFE0F, Kind::kIgnorable},      {0xFEFF, Kind::kIgnorable},
            {0xFFA0, Kind::kIgnorable},      {0xE0001, Kind::kIgnorable},
            {0xE0100, Kind::kIgnorable},     {0x0300, Kind::kMark},
            {0x036F, Kind::kMark},           {0x0E31, Kind::kMark},
            {0x1AB0, Kind::kMark},           {0x1DC0, Kind::kMark},
            {0x20D0, Kind::kMark},           {0xFE20, Kind::kMark},
            {0x0488, Kind::kMark},           {0x20DD, Kind::kMark},
            {'C', Kind::kVisible},           {0x0E01, Kind::kVisible},
            {0x3042, Kind::kVisible},        {0x001C, Kind::kVisibleControl},
            {0x001F, Kind::kVisibleControl}, {0x0085, Kind::kVisibleControl},
    };
    for (const auto& [u, kind] : codepoints) {
        const unsigned hex = static_cast<unsigned>(u);
        auto paragraph = layout(u);
        const SkScalar width = paragraph->getMaxIntrinsicWidth();
        if (kind == Kind::kVisible || kind == Kind::kVisibleControl) {
            REPORTER_ASSERT(reporter, width > baseWidth, "U+%04X: %g", hex, width);
        } else {
            REPORTER_ASSERT(
                    reporter, width == baseWidth, "U+%04X: %g vs %g", hex, width, baseWidth);
        }
        const bool unresolved = paragraph->unresolvedCodepoints().count(u) == 1;
        REPORTER_ASSERT(reporter,
                        unresolved == (kind == Kind::kMark || kind == Kind::kVisible),
                        "U+%04X unresolved=%d",
                        hex,
                        unresolved);
    }

    // An empty family list resolves the only registered face, as FontCollection does, so the
    // first span is a Roboto run: marks still get a box, controls Roboto lacks draw nothing
    sk_sp<SkTypeface> roboto = ToolUtils::CreateTypefaceFromResource("fonts/Roboto-Regular.ttf");
    // Only CoreText-backed font managers load resource data; the portable one cannot
    if (!roboto) {
        return;
    }
    auto provider = sk_make_sp<TypefaceFontProvider>();
    provider->registerTypeface(roboto, SkString("StandIn"));
    auto standInCollection = sk_make_sp<FontCollection>();
    standInCollection->setAssetFontManager(provider);
    standInCollection->disableFontFallback();
    TextStyle emptyFamilyStyle;
    emptyFamilyStyle.setFontFamilies({});
    emptyFamilyStyle.setFontSize(16.0f);
    emptyFamilyStyle.setColor(SK_ColorBLACK);
    TextStyle standInStyle = emptyFamilyStyle;
    standInStyle.setFontFamilies({SkString("StandIn")});
    // Paints prefix (none when 0) in emptyFamilyStyle, then "   B" in standInStyle
    auto paintTwoSpan = [&](SkUnichar prefix) {
        ParagraphBuilderImpl builder(
                paragraphStyle, standInCollection, get_unicode(paragraphStyle));
        if (prefix) {
            char buffer[SkUTF::kMaxBytesInUTF8Sequence];
            builder.pushStyle(emptyFamilyStyle);
            builder.addText(buffer, SkUTF::ToUTF8(prefix, buffer));
            builder.pop();
        }
        builder.pushStyle(standInStyle);
        builder.addText("   B", 4);
        auto paragraph = builder.Build();
        paragraph->layout(1000.0f);
        SkBitmap bitmap;
        bitmap.allocN32Pixels(120, 40);
        SkCanvas canvas(bitmap);
        canvas.clear(SK_ColorWHITE);
        paragraph->paint(&canvas, 0, 0);
        return bitmap;
    };
    const SkBitmap baseBitmap = paintTwoSpan(0);
    const std::pair<SkUnichar, bool> paintCases[] = {
            {0x0001, false},
            {0x0086, false},
            {0x00AD, false},
            {0x115F, false},
            {0x200B, false},
            {0x206A, false},
            {0x3164, false},
            {0xFEFF, false},
            {0xFFA0, false},
            {0xE0001, false},
            {0x0E31, true},
            {0x20DD, true},
            {'C', true},
            {0x001C, false},
            {0x0085, false},
    };
    for (const auto& [u, drawsBox] : paintCases) {
        const SkBitmap bitmap = paintTwoSpan(u);
        const bool differs =
                memcmp(bitmap.getPixels(), baseBitmap.getPixels(), bitmap.computeByteSize()) != 0;
        REPORTER_ASSERT(reporter,
                        differs == drawsBox,
                        "2-span U+%04X differs=%d expected=%d",
                        static_cast<unsigned>(u),
                        differs,
                        drawsBox);
    }
}

// Cluster indexes of 2-byte Cyrillic stay on character starts and tile the run without gaps;
// an index inside a character would break hit-testing and line breaking. Run and cluster counts
// are the contract: one-style text is one CTLine run and each character one atomic cluster
DEF_TEST(CoreTextPipeline_LineShaper_ClusterAtomicity, reporter) {
    auto fontCollection = sk_make_sp<FontCollection>();
    fontCollection->setDefaultFontManager(ToolUtils::TestFontMgr());

    {
        skiatest::ReporterContext ctx(reporter, SkString("LTR Cyrillic"));
        ParagraphStyle style;
        style.setTextShapingEngine(TextShapingEngine::kCoreText);
        ParagraphBuilderImpl builder(style, fontCollection, get_unicode(style));
        TextStyle textStyle;
        textStyle.setFontSize(14.0f);
        builder.pushStyle(textStyle);
        const char* cyrText = "Кластер";
        size_t cyrLen = strlen(cyrText);
        builder.addText(cyrText, cyrLen);
        builder.pop();

        auto paragraph = builder.Build();
        auto impl = static_cast<ParagraphImpl*>(paragraph.get());
        REPORTER_ASSERT(reporter, impl->computeCodeUnitProperties());

        CoreTextLineShaper shaper(impl);
        REPORTER_ASSERT(reporter, shaper.shape());
        REPORTER_ASSERT(reporter, impl->runs().size() == 1, "got %zu runs", impl->runs().size());
        if (impl->runs().size() != 1) {
            return;
        }

        auto& run = impl->runs()[0];
        REPORTER_ASSERT(reporter,
                        run.textRange().width() == cyrLen,
                        "got %zu, want %zu",
                        run.textRange().width(),
                        cyrLen);
        REPORTER_ASSERT(reporter, run.clusterIndexes()[0] == 0, "got %u", run.clusterIndexes()[0]);
        REPORTER_ASSERT(reporter,
                        run.clusterIndexes()[run.size()] == cyrLen,
                        "got %u, want %zu",
                        run.clusterIndexes()[run.size()],
                        cyrLen);

        REPORTER_ASSERT(reporter, run.size() == 7, "got %zu glyphs", run.size());
        for (size_t k = 0; k <= run.size(); ++k) {
            REPORTER_ASSERT(reporter,
                            run.clusterIndexes()[k] % 2 == 0 && run.clusterIndexes()[k] <= cyrLen,
                            "glyph %zu: got %u, want even <= %zu",
                            k,
                            run.clusterIndexes()[k],
                            cyrLen);
        }

        size_t lastCharEnd = run.textRange().start;
        size_t clusterCount = 0;
        run.iterateThroughClustersInTextOrder([&](size_t glyphStart,
                                                  size_t glyphEnd,
                                                  size_t charStart,
                                                  size_t charEnd,
                                                  SkScalar /*width*/) {
            REPORTER_ASSERT(
                    reporter,
                    charStart == lastCharEnd && charEnd > charStart && glyphEnd > glyphStart,
                    "cluster %zu: chars [%zu, %zu) after %zu, glyphs [%zu, %zu)",
                    clusterCount,
                    charStart,
                    charEnd,
                    lastCharEnd,
                    glyphStart,
                    glyphEnd);
            lastCharEnd = charEnd;
            ++clusterCount;
        });
        REPORTER_ASSERT(reporter,
                        lastCharEnd == run.textRange().end,
                        "got %zu, want %zu",
                        lastCharEnd,
                        run.textRange().end);
        REPORTER_ASSERT(reporter, clusterCount == 7, "got %zu clusters", clusterCount);

        impl->buildClusterTable();
        // One cluster per character plus the end-of-text cluster
        REPORTER_ASSERT(reporter,
                        impl->clusters().size() == 8,
                        "got %zu clusters",
                        impl->clusters().size());
        for (size_t i = 0; i < cyrLen; ++i) {
            REPORTER_ASSERT(reporter,
                            impl->clusterIndex(i) == i / 2,
                            "byte %zu: got %zu, want %zu",
                            i,
                            impl->clusterIndex(i),
                            i / 2);
        }
    }

    {
        skiatest::ReporterContext ctx(reporter, SkString("RTL Arabic"));
        ParagraphStyle rtlStyle;
        rtlStyle.setTextDirection(TextDirection::kRtl);
        rtlStyle.setTextShapingEngine(TextShapingEngine::kCoreText);
        ParagraphBuilderImpl builder(rtlStyle, fontCollection, get_unicode(rtlStyle));
        TextStyle textStyle;
        textStyle.setFontSize(14.0f);
        builder.pushStyle(textStyle);
        const char* rtlText = "اختبار";
        size_t rtlLen = strlen(rtlText);
        builder.addText(rtlText, rtlLen);
        builder.pop();

        auto paragraph = builder.Build();
        auto impl = static_cast<ParagraphImpl*>(paragraph.get());
        REPORTER_ASSERT(reporter, impl->computeCodeUnitProperties());

        CoreTextLineShaper shaper(impl);
        REPORTER_ASSERT(reporter, shaper.shape());
        REPORTER_ASSERT(reporter, impl->runs().size() == 1, "got %zu runs", impl->runs().size());
        if (impl->runs().size() != 1) {
            return;
        }

        auto& run = impl->runs()[0];
        REPORTER_ASSERT(reporter, !run.leftToRight());

        size_t lastCharEnd = run.textRange().start;
        size_t clusterCount = 0;
        run.iterateThroughClustersInTextOrder([&](size_t /*glyphStart*/,
                                                  size_t /*glyphEnd*/,
                                                  size_t charStart,
                                                  size_t charEnd,
                                                  SkScalar /*width*/) {
            REPORTER_ASSERT(reporter,
                            charStart == lastCharEnd && charEnd > charStart,
                            "cluster %zu: chars [%zu, %zu) after %zu",
                            clusterCount,
                            charStart,
                            charEnd,
                            lastCharEnd);
            lastCharEnd = charEnd;
            ++clusterCount;
        });
        REPORTER_ASSERT(reporter,
                        lastCharEnd == run.textRange().end,
                        "got %zu, want %zu",
                        lastCharEnd,
                        run.textRange().end);
        REPORTER_ASSERT(reporter, clusterCount > 0, "got %zu clusters", clusterCount);
    }
}

namespace {
// Records where TextLine hands each glyph blob to the canvas. The canvas places glyph k at
// (x, y) + the blob's own point k, so both halves are kept: their sum is the painted baseline
class BaselineShiftBlobRecorder : public ParagraphPainter {
public:
    static constexpr int kMaxBlobs = 4;
    // fPainted is the blob's (x, y) plus its first glyph's point, or (x, y) for an empty blob
    struct Blob {
        SkPoint fPainted;
        uint32_t fGlyphCount;
    };

    void drawTextBlob(const sk_sp<SkTextBlob>& blob,
                      SkScalar x,
                      SkScalar y,
                      const SkPaintOrID&) override {
        // Every call is counted so an unexpected blob split is reported instead of overrunning
        // the fixed storage
        ++fTotal;
        if (fTotal > kMaxBlobs) {
            return;
        }
        Blob& record = fBlobs[fTotal - 1];
        record = {{x, y}, 0};
        if (!blob) {
            return;
        }
        SkTextBlobRunIterator it(blob.get());
        if (!it.done() && it.positioning() == SkTextBlobRunIterator::kFull_Positioning) {
            record.fGlyphCount = it.glyphCount();
            if (record.fGlyphCount > 0) {
                record.fPainted = {x + it.points()[0].fX, y + it.points()[0].fY};
            }
        }
    }
    void drawTextShadow(const sk_sp<SkTextBlob>&, SkScalar, SkScalar, SkColor, SkScalar) override {}
    void drawRect(const SkRect&, const SkPaintOrID&) override {}
    void drawFilledRect(const SkRect&, const DecorationStyle&) override {}
    void drawPath(const SkPath&, const DecorationStyle&) override {}
    void drawLine(SkScalar, SkScalar, SkScalar, SkScalar, const DecorationStyle&) override {}
    void clipRect(const SkRect&) override {}
    void translate(SkScalar, SkScalar) override {}
    void save() override {}
    void restore() override {}

    int fTotal = 0;
    Blob fBlobs[kMaxBlobs] = {};
};

// "X" on the line's own baseline followed by a smaller "2" shifted by shift; the size
// difference keeps the two spans in separate runs independently of the shift itself
std::unique_ptr<Paragraph> layout_shifted_pair(const sk_sp<FontCollection>& fontCollection,
                                               SkScalar shift) {
    TextStyle plain;
    plain.setFontFamilies({SkString("Roboto")});
    plain.setFontSize(20.0f);
    TextStyle shifted = plain;
    shifted.setFontSize(12.0f);
    shifted.setBaselineShift(shift);

    ParagraphStyle paragraphStyle;
    paragraphStyle.setTextShapingEngine(TextShapingEngine::kCoreText);
    ParagraphBuilderImpl builder(paragraphStyle, fontCollection, get_unicode(paragraphStyle));
    builder.pushStyle(plain);
    builder.addText("X");
    builder.pop();
    builder.pushStyle(shifted);
    builder.addText("2");
    builder.pop();
    auto paragraph = builder.Build();
    paragraph->layout(1000.0f);
    return paragraph;
}

// Painted baseline of "2" minus painted baseline of "X" (Skia y grows downwards), or NaN when
// the paint did not produce the two single-glyph blobs the contrast relies on
SkScalar painted_shift_delta(skiatest::Reporter* reporter,
                             const sk_sp<FontCollection>& fontCollection,
                             SkScalar shift) {
    auto paragraph = layout_shifted_pair(fontCollection, shift);
    auto impl = static_cast<ParagraphImpl*>(paragraph.get());
    REPORTER_ASSERT(reporter, impl->isCoreTextShaped());
    REPORTER_ASSERT(
            reporter, paragraph->unresolvedGlyphs() == 0, "got %d", paragraph->unresolvedGlyphs());
    REPORTER_ASSERT(reporter, paragraph->lineNumber() == 1, "got %zu", paragraph->lineNumber());

    // The shaper must hand the style shift to the run model: paint applies it from there
    SkSpan<Run> runs = impl->runs();
    REPORTER_ASSERT(reporter, runs.size() == 2, "shift %g: %zu run(s)", shift, runs.size());
    for (const Run& run : runs) {
        const SkScalar expected = run.textRange().start == 0 ? 0.0f : shift;
        REPORTER_ASSERT(reporter,
                        run.baselineShift() == expected,
                        "shift %g: run [%zu, %zu) carries baselineShift %g",
                        shift,
                        run.textRange().start,
                        run.textRange().end,
                        run.baselineShift());
    }

    BaselineShiftBlobRecorder recorder;
    paragraph->paint(&recorder, 0.0f, 0.0f);
    REPORTER_ASSERT(
            reporter, recorder.fTotal == 2, "shift %g: %d blob(s) painted", shift, recorder.fTotal);
    if (recorder.fTotal != 2) {
        return SK_ScalarNaN;
    }
    const auto& a = recorder.fBlobs[0];
    const auto& b = recorder.fBlobs[1];
    // LTR line: whichever blob lands further left is "X"
    const bool aIsLeft = a.fPainted.fX < b.fPainted.fX;
    const auto& plain = aIsLeft ? a : b;
    const auto& shifted = aIsLeft ? b : a;
    REPORTER_ASSERT(reporter,
                    plain.fGlyphCount == 1 && shifted.fGlyphCount == 1,
                    "shift %g: blobs carry %u and %u glyph(s)",
                    shift,
                    plain.fGlyphCount,
                    shifted.fGlyphCount);
    return shifted.fPainted.fY - plain.fPainted.fY;
}
}  // namespace

// A baselineShift span paints moved by exactly the shift, which holds only if the CoreText
// shaper leaves glyphs on the unshifted baseline for TextLine to move
DEF_TEST(CoreTextPipeline_BaselineShift_PaintsShiftedOnce, reporter) {
    sk_sp<SkTypeface> roboto = ToolUtils::CreateTypefaceFromResource("fonts/Roboto-Regular.ttf");
    if (!roboto) {
        // Only CoreText-backed font managers load resource data; the portable one cannot
        return;
    }
    auto provider = sk_make_sp<TypefaceFontProvider>();
    provider->registerTypeface(roboto, SkString("Roboto"));
    auto fontCollection = sk_make_sp<FontCollection>();
    fontCollection->setAssetFontManager(provider);
    fontCollection->setDefaultFontManager(ToolUtils::TestFontMgr());

    // TextLine::buildTextBlob is the single place that applies Run::fBaselineShift, so glyph
    // positions coming back from the shaper must sit on the unshifted baseline; a shaper that
    // pre-shifts them paints the span twice as far. The floor(+0.5) rounding of the painted
    // baseline is what the 0.51 tolerance absorbs
    for (SkScalar shift : {6.0f, -6.0f}) {
        const SkScalar delta = painted_shift_delta(reporter, fontCollection, shift);
        REPORTER_ASSERT(reporter,
                        SkScalarNearlyEqual(delta, shift, 0.51f),
                        "painted shift %g, want %g",
                        delta,
                        shift);
    }
}

// A zero font size lays out with zero width and stays on CoreText, so a build without
// HarfBuzz handles it too instead of falling back
DEF_TEST(CoreTextPipeline_ZeroFontSizeHasNoWidth, reporter) {
    auto fontCollection = sk_make_sp<FontCollection>();
    fontCollection->setDefaultFontManager(ToolUtils::TestFontMgr());
    ParagraphStyle style;
    style.setTextShapingEngine(TextShapingEngine::kCoreText);
    ParagraphBuilderImpl builder(style, fontCollection, get_unicode(style));
    TextStyle textStyle;
    textStyle.setFontSize(0);
    builder.pushStyle(textStyle);
    builder.addText("abc");
    auto paragraph = builder.Build();
    paragraph->layout(500);
    REPORTER_ASSERT(reporter,
                    paragraph->getMaxIntrinsicWidth() == 0,
                    "zero-size text is %g wide",
                    paragraph->getMaxIntrinsicWidth());
    // Zero sizes stay on CoreText so a build without HarfBuzz lays them out as well
    REPORTER_ASSERT(reporter, static_cast<ParagraphImpl*>(paragraph.get())->isCoreTextShaped());
}

// Truncated UTF-8 gives a CFString exactly as long as CoreTextIndexMap counts, one U+FFFD per
// invalid byte, so every CFRange still lands on its text; a mismatch would shift all runs
DEF_TEST(CoreTextPipeline_InvalidUtf8MatchesIndexMap, reporter) {
    // A truncated three-byte sequence followed by ASCII
    const char text[] =
            "\xE2\x82"
            "a";
    const size_t bytes = sizeof(text) - 1;
    auto fontCollection = sk_make_sp<FontCollection>();
    fontCollection->setDefaultFontManager(ToolUtils::TestFontMgr());
    CoreTextAttrStringBuilder attrBuilder(
            std::string_view(text, bytes), TextDirection::kLtr, test_unicode());
    attrBuilder.addStyle(TextRange(0, bytes), TextStyle());
    CoreTextRequestedFonts fonts(fontCollection);
    auto attr = attrBuilder.build(&fonts);
    REPORTER_ASSERT(reporter, attr != nullptr);
    REPORTER_ASSERT(reporter,
                    SkToSizeT(CFAttributedStringGetLength(attr.get())) ==
                            attrBuilder.indexMap().toUtf16Index(bytes),
                    "got %zu, want %zu",
                    SkToSizeT(CFAttributedStringGetLength(attr.get())),
                    attrBuilder.indexMap().toUtf16Index(bytes));
}

// Complex scripts that reorder glyphs within a cluster (Devanagari pre-base matra U+093F in
// "ककि") must normalize CTRun cluster indices to monotonic text order so cluster table
// construction covers the full text range
DEF_TEST(CoreTextPipeline_NonMonotonicClusterIndices_DevanagariMatra, reporter) {
    auto fc = test_font_collection();
    ParagraphStyle ps;
    ps.setTextShapingEngine(TextShapingEngine::kCoreText);
    TextStyle ts;
    ts.setFontSize(20.0f);

    // "ककि" = U+0915 U+0915 U+093F (9 UTF-8 bytes)
    const char* deva = "\xE0\xA4\x95\xE0\xA4\x95\xE0\xA4\xBF";
    ParagraphBuilderImpl builder(ps, fc, get_unicode(ps));
    builder.pushStyle(ts);
    builder.addText(deva);
    builder.pop();

    auto p = builder.Build();
    p->layout(500.0f);

    auto impl = static_cast<ParagraphImpl*>(p.get());
    REPORTER_ASSERT(reporter, impl->isCoreTextShaped());
    REPORTER_ASSERT(reporter, !impl->runs().empty());
    for (const auto& run : impl->runs()) {
        REPORTER_ASSERT(reporter, run.size() > 0, "got %zu glyphs", run.size());
        for (size_t k = 1; k <= run.size(); ++k) {
            if (run.leftToRight()) {
                REPORTER_ASSERT(reporter,
                                run.clusterIndexes()[k] >= run.clusterIndexes()[k - 1],
                                "glyph %zu: cluster %u after %u",
                                k,
                                run.clusterIndexes()[k],
                                run.clusterIndexes()[k - 1]);
            }
        }
    }
    auto boxes = p->getRectsForRange(0, 3, RectHeightStyle::kTight, RectWidthStyle::kTight);
    REPORTER_ASSERT(reporter, !boxes.empty());
}

namespace {
// text in family at 20pt, left aligned on a 1000-wide line, in test_font_collection()
std::unique_ptr<Paragraph> layout_marked(TextShapingEngine engine,
                                         const char* family,
                                         const char* text,
                                         TextDirection direction) {
    ParagraphStyle paragraphStyle;
    paragraphStyle.setTextShapingEngine(engine);
    paragraphStyle.setTextDirection(direction);
    paragraphStyle.setTextAlign(TextAlign::kLeft);
    TextStyle style;
    style.setFontFamilies({SkString(family)});
    style.setFontSize(20.0f);
    ParagraphBuilderImpl builder(
            paragraphStyle, test_font_collection(), get_unicode(paragraphStyle));
    builder.pushStyle(style);
    builder.addText(text);
    auto paragraph = builder.Build();
    paragraph->layout(1000);
    return paragraph;
}

// Glyph x and width CoreText itself draws for text on one CTLine at 20pt, optionally pinned to an
// explicit paragraph direction
PlainLine plain_coretext_line(const char* family,
                              const char* text,
                              std::optional<TextDirection> direction = std::nullopt) {
    SkUniqueCFRef<CFStringRef> name(
            CFStringCreateWithCString(nullptr, family, kCFStringEncodingUTF8));
    SkUniqueCFRef<CTFontRef> font(CTFontCreateWithName(name.get(), 20, nullptr));
    return coretext_line(font.get(), text, direction);
}

struct CaretStep {
    SkScalar x;
    int position;
};

// Hit-test UTF-16 positions along [0, width + 2) at 0.25px steps, keeping each x where position
// changes
std::vector<CaretStep> caret_steps(SkScalar width, const std::function<int(SkScalar)>& positionAt) {
    std::vector<CaretStep> steps;
    const int count = SkScalarCeilToInt(width * 4) + 8;
    for (int i = 0; i < count; ++i) {
        const SkScalar x = (i + 0.5f) / 4;
        const int position = positionAt(x);
        if (steps.empty() || steps.back().position != position) {
            steps.push_back({x, position});
        }
    }
    return steps;
}

struct MarkedGlyph {
    SkScalar drawn;
    bool mark;
    size_t cluster;
};

// Glyphs in the order and at the x they are painted; Run positions follow the logical run order,
// CTLine glyphs the visual one. Marks are glyphs off a grapheme start, which only holds for fonts
// that draw each mark from its own code point: dots a Nastaliq font splits off its base, a reph
// or a pre-base matra start a grapheme and count as spacing here
std::vector<MarkedGlyph> marked_glyphs(ParagraphImpl* impl) {
    std::vector<MarkedGlyph> glyphs;
    impl->visit([&](int, const Paragraph::VisitorInfo* info) {
        if (!info) {
            return;
        }
        for (int i = 0; i < info->count; ++i) {
            const size_t cluster = info->utf8Starts[i];
            glyphs.push_back(
                    {info->origin.fX + info->positions[i].fX,
                     !impl->codeUnitHasProperty(cluster, SkUnicode::CodeUnitFlags::kGraphemeStart),
                     cluster});
        }
    });
    return glyphs;
}

// Lays text out on kCoreText and checks it against the CTLine CoreText builds for it: the same
// width, each glyph drawn at the same x, and each spacing glyph's cluster as wide as the gap to
// the next spacing glyph, so mark clusters stay empty
std::unique_ptr<Paragraph> expect_drawn_as_coretext(skiatest::Reporter* reporter,
                                                    const char* family,
                                                    const char* text,
                                                    TextDirection direction,
                                                    const PlainLine& plain) {
    auto paragraph = layout_marked(TextShapingEngine::kCoreText, family, text, direction);
    auto* impl = static_cast<ParagraphImpl*>(paragraph.get());
    REPORTER_ASSERT(reporter, impl->isCoreTextShaped());
    REPORTER_ASSERT(reporter,
                    SkScalarNearlyEqual(paragraph->getMaxIntrinsicWidth(), plain.width, 0.01f),
                    "%s: width %g, CoreText line %g",
                    family,
                    paragraph->getMaxIntrinsicWidth(),
                    plain.width);
    for (const Run& run : impl->runs()) {
        for (size_t k = 0; k < run.size(); ++k) {
            const size_t cluster = run.globalClusterIndex(k);
            const bool mark =
                    !impl->codeUnitHasProperty(cluster, SkUnicode::CodeUnitFlags::kGraphemeStart);
            REPORTER_ASSERT(reporter,
                            mark || glyph_offset(run, k).fX == 0,
                            "%s cluster %zu: spacing glyph offset %g",
                            family,
                            cluster,
                            glyph_offset(run, k).fX);
        }
    }
    const std::vector<MarkedGlyph> glyphs = marked_glyphs(impl);
    REPORTER_ASSERT(reporter,
                    glyphs.size() == plain.x.size(),
                    "%s: %zu glyphs, CoreText line has %zu",
                    family,
                    glyphs.size(),
                    plain.x.size());
    if (glyphs.size() != plain.x.size()) {
        return paragraph;
    }
    std::vector<SkScalar> clusterWidths(strlen(text) + 1, 0);
    for (size_t k = 0; k < glyphs.size(); ++k) {
        REPORTER_ASSERT(reporter,
                        SkScalarNearlyEqual(glyphs[k].drawn, plain.x[k], 0.01f),
                        "%s glyph %zu: drawn at %g, CoreText at %g",
                        family,
                        k,
                        glyphs[k].drawn,
                        plain.x[k]);
        if (glyphs[k].mark) {
            continue;
        }
        SkScalar next = plain.width;
        for (size_t j = k + 1; j < glyphs.size(); ++j) {
            if (!glyphs[j].mark) {
                next = plain.x[j];
                break;
            }
        }
        clusterWidths[glyphs[k].cluster] += next - plain.x[k];
    }
    for (const Cluster& cluster : impl->clusters()) {
        const TextRange range = cluster.textRange();
        if (range.width() == 0) {
            continue;
        }
        REPORTER_ASSERT(reporter,
                        SkScalarNearlyEqual(cluster.width(), clusterWidths[range.start], 0.01f),
                        "%s cluster [%zu,%zu): width %g, drawn %g",
                        family,
                        range.start,
                        range.end,
                        cluster.width(),
                        clusterWidths[range.start]);
    }
    return paragraph;
}
}  // namespace

// CoreText derives advances from drawn x, so a mark drawn off the pen gets an advance and the
// glyph before it in its run loses as much. Combining marks must add glyphs without growing the
// line width, stay over their base grapheme box, and keep carets at CTLine's UTF-16 steps
DEF_TEST(CoreTextPipeline_LineShaper_MarksTakeNoAdvance, reporter) {
    if (!has_font_families({".AppleSystemUIFont", "Arial Hebrew", "Arial", "Courier New"})) {
        return;
    }
    struct Case {
        const char* family;
        const char* text;
        TextDirection direction;
    };
    // SF draws U+0301 left of the pen after b. Arial Hebrew draws qamats right of the pen, and
    // the dagesh of the last word comes first in the RTL run, before its base. Arial draws the
    // Arabic marks off the pen, and the qamats first in an RTL run between LTR runs. Courier
    // New's Arabic marks have a nonzero hmtx advance that CoreText zeroes
    const Case kCases[] = {
            {".AppleSystemUIFont", "ab\u0301c", TextDirection::kLtr},
            {"Arial Hebrew",
             "\u05E9\u05C1\u05B8\u05DC\u05D5\u05B9\u05DD \u05D1\u05BC\u05B0",
             TextDirection::kRtl},
            {"Arial",
             "\u0628\u0650\u0633\u0652\u0645\u0650 \u0627\u0644\u0644\u0651\u064E\u0647\u0650",
             TextDirection::kRtl},
            {"Arial", "abc \u05D1\u05B8 def", TextDirection::kLtr},
            {"Courier New", "\u0628\u0650\u0633\u0652\u0645\u0650", TextDirection::kRtl},
    };
    for (const Case& c : kCases) {
        skiatest::ReporterContext context(reporter, SkStringPrintf("%s '%s'", c.family, c.text));
        const PlainLine plain = plain_coretext_line(c.family, c.text, c.direction);
        ParagraphStyle paragraphStyle;
        paragraphStyle.setTextShapingEngine(TextShapingEngine::kCoreText);
        paragraphStyle.setTextDirection(c.direction);
        paragraphStyle.setTextAlign(TextAlign::kLeft);
        paragraphStyle.setApplyRoundingHack(false);
        TextStyle style;
        style.setFontFamilies({SkString(c.family)});
        style.setFontSize(20.0f);
        ParagraphBuilderImpl builder(
                paragraphStyle, test_font_collection(), get_unicode(paragraphStyle));
        builder.pushStyle(style);
        builder.addText(c.text);
        auto paragraph = builder.Build();
        paragraph->layout(1000);
        auto* impl = static_cast<ParagraphImpl*>(paragraph.get());
        REPORTER_ASSERT(reporter, impl->isCoreTextShaped());

        std::vector<LineMetrics> lines;
        paragraph->getLineMetrics(lines);
        REPORTER_ASSERT(reporter, lines.size() == 1, "got %zu lines", lines.size());
        if (lines.size() != 1) {
            continue;
        }
        REPORTER_ASSERT(
                reporter,
                SkScalarNearlyEqual(
                        static_cast<SkScalar>(lines[0].fWidth), plain.width, 1.0f / 4096.0f),
                "line fWidth %g, want %g",
                lines[0].fWidth,
                plain.width);
        // getMaxIntrinsicWidth() rounds to 0.01pt on ParagraphImpl
        REPORTER_ASSERT(reporter,
                        SkScalarNearlyEqual(paragraph->getMaxIntrinsicWidth(), plain.width, 0.01f),
                        "maxIntrinsicWidth %g, want %g",
                        paragraph->getMaxIntrinsicWidth(),
                        plain.width);

        impl->ensureUTF16Mapping();
        int baseGlyphs = 0;
        int markGlyphs = 0;
        paragraph->visit([&](int, const Paragraph::VisitorInfo* info) {
            if (!info) {
                return;
            }
            for (int i = 0; i < info->count; ++i) {
                const size_t cluster = info->utf8Starts[i];
                const bool isMark = !impl->codeUnitHasProperty(
                        cluster, SkUnicode::CodeUnitFlags::kGraphemeStart);
                if (!isMark) {
                    ++baseGlyphs;
                    continue;
                }
                ++markGlyphs;
                Paragraph::GlyphInfo grapheme;
                const size_t u16 = impl->getUTF16Index(cluster);
                REPORTER_ASSERT(reporter, paragraph->getGlyphInfoAtUTF16Offset(u16, &grapheme));
                SkRect ink;
                info->font.getBounds({&info->glyphs[i], 1}, {&ink, 1}, nullptr);
                ink.offset(info->origin + info->positions[i]);
                const SkRect& baseBox = grapheme.fGraphemeLayoutBounds;
                REPORTER_ASSERT(reporter,
                                ink.fLeft < baseBox.fRight && ink.fRight > baseBox.fLeft,
                                "cluster %zu: mark ink [%g, %g] misses base box [%g, %g]",
                                cluster,
                                ink.fLeft,
                                ink.fRight,
                                baseBox.fLeft,
                                baseBox.fRight);
            }
        });
        REPORTER_ASSERT(reporter,
                        baseGlyphs >= 1 && markGlyphs >= 1,
                        "got %d base and %d mark glyphs",
                        baseGlyphs,
                        markGlyphs);

        // The case only tests something if CoreText gives some mark an advance
        bool markAdvanced = false;
        const std::vector<MarkedGlyph> glyphs = marked_glyphs(impl);
        REPORTER_ASSERT(reporter,
                        !glyphs.empty() && !plain.advances.empty(),
                        "got %zu drawn, %zu native glyphs",
                        glyphs.size(),
                        plain.advances.size());
        for (size_t k = 0; k < std::min(glyphs.size(), plain.advances.size()); ++k) {
            markAdvanced |= glyphs[k].mark && plain.advances[k] != 0;
        }
        REPORTER_ASSERT(reporter, markAdvanced);

        const std::vector<CaretStep> ctSteps =
                caret_steps(paragraph->getMaxIntrinsicWidth(), [&](SkScalar x) {
                    return paragraph->getGlyphPositionAtCoordinate(x, 10).position;
                });
        const std::vector<CaretStep> nativeSteps = caret_steps(plain.width, [&](SkScalar x) {
            return SkToInt(CTLineGetStringIndexForPosition(plain.line.get(), CGPointMake(x, 0)));
        });
        REPORTER_ASSERT(reporter,
                        !ctSteps.empty() && ctSteps.size() == nativeSteps.size(),
                        "got %zu caret steps, want %zu",
                        ctSteps.size(),
                        nativeSteps.size());
        for (size_t i = 0; i < std::min(ctSteps.size(), nativeSteps.size()); ++i) {
            const CaretStep& ct = ctSteps[i];
            const CaretStep& native = nativeSteps[i];
            REPORTER_ASSERT(
                    reporter,
                    ct.position == native.position && SkScalarNearlyEqual(ct.x, native.x, 0.5f),
                    "caret step %zu: got %d at x=%g, want %d at x=%g",
                    i,
                    ct.position,
                    ct.x,
                    native.position,
                    native.x);
        }
    }
}

// On a line that starts its CTLine, a tab ends at CoreText's tab stop even when its glyph has no
// hmtx advance, and the tab's cluster keeps that advance. This is CoreText's tab model, not
// HarfBuzz parity: HarfBuzz gives the tab its hmtx advance (TODO in extract_ctrun_glyphs)
DEF_TEST(CoreTextPipeline_LineShaper_UnwrappedTabEndsAtCoreTextStop, reporter) {
    if (!has_font_families({"Galvji", "Zapfino"})) {
        return;
    }
    for (const char* family : {"Galvji", "Zapfino"}) {
        for (const char* text : {"a\tb", "\tb"}) {
            const PlainLine plain = plain_coretext_line(family, text);
            auto paragraph =
                    expect_drawn_as_coretext(reporter, family, text, TextDirection::kLtr, plain);
            auto* impl = static_cast<ParagraphImpl*>(paragraph.get());
            // The case only tests something if the tab glyph has no hmtx advance
            const Run& run = impl->runs()[0];
            const size_t tab = text[0] == '\t' ? 0 : 1;
            SkGlyphID glyph = run.glyphs()[tab];
            SkScalar width = -1;
            run.font().getWidths({&glyph, 1}, {&width, 1});
            REPORTER_ASSERT(reporter,
                            glyph != 0 && width == 0,
                            "%s: tab glyph %u has advance %g",
                            family,
                            glyph,
                            width);
#if !defined(SK_PARAGRAPH_DISABLE_HARFBUZZ) && defined(SK_UNICODE_ICU_IMPLEMENTATION)
            auto harfBuzz =
                    layout_marked(TextShapingEngine::kDefault, family, text, TextDirection::kLtr);
            INFOF(reporter,
                  "%s: CoreText width %g, HarfBuzz %g\n",
                  family,
                  paragraph->getMaxIntrinsicWidth(),
                  harfBuzz->getMaxIntrinsicWidth());
#endif
        }
    }
}

// SH8, SH7: Noto Nastaliq Urdu joins letters with GPOS cursive attachment in fOffsets while
// keeping fPositions on the baseline, and draws each fatha above the baseline inside its base box
DEF_TEST(CoreTextPipeline_LineShaper_CursiveMarksDrawnOnTheirBase, reporter) {
    const char* family = "Noto Nastaliq Urdu";
    if (!has_font_families({family})) {
        return;
    }
    // Fatha over noon and over teh: a mark the font draws above its base
    const char* text = "\u0646\u064E\u0633\u062A\u064E\u0639\u0644\u06CC\u0642";
    auto paragraph = layout_marked(TextShapingEngine::kCoreText, family, text, TextDirection::kRtl);
    auto* impl = static_cast<ParagraphImpl*>(paragraph.get());
    REPORTER_ASSERT(reporter,
                    impl->isCoreTextShaped() && paragraph->unresolvedGlyphs() == 0,
                    "got %d unresolved",
                    paragraph->unresolvedGlyphs());
    // extendedVisit and getPath read fPositions without fOffsets, so cursive y stays in fOffsets
    REPORTER_ASSERT(reporter, !impl->runs().empty());
    bool movedInY = false;
    for (const Run& run : impl->runs()) {
        REPORTER_ASSERT(reporter, run.size() > 0, "got %zu glyphs", run.size());
        for (size_t k = 0; k < run.size(); ++k) {
            movedInY |= glyph_offset(run, k).fY != 0;
            REPORTER_ASSERT(reporter,
                            run.positions()[k].fY == 0,
                            "glyph %zu: fPositions y %g",
                            k,
                            run.positions()[k].fY);
        }
    }
    REPORTER_ASSERT(reporter, movedInY);

    struct Glyph {
        SkGlyphID id;
        SkPoint drawn;
        SkScalar advance;
        size_t cluster;
    };
    std::vector<Glyph> glyphs;
    impl->visit([&](int, const Paragraph::VisitorInfo* info) {
        if (!info) {
            return;
        }
        for (int i = 0; i < info->count; ++i) {
            SkScalar advance = 0;
            info->font.getWidths({&info->glyphs[i], 1}, {&advance, 1});
            glyphs.push_back({info->glyphs[i],
                              info->origin + info->positions[i],
                              advance,
                              info->utf8Starts[i]});
        }
    });
    std::vector<LineMetrics> lines;
    paragraph->getLineMetrics(lines);
    REPORTER_ASSERT(reporter,
                    !lines.empty() && glyphs.size() >= 8,
                    "got %zu lines, %zu glyphs",
                    lines.size(),
                    glyphs.size());
    const SkScalar baseline = lines.empty() ? 0 : static_cast<SkScalar>(lines[0].fBaseline);
    int marks = 0;
    for (const Glyph& mark : glyphs) {
        REPORTER_ASSERT(reporter, mark.id != 0, "cluster %zu: .notdef", mark.cluster);
        if (impl->codeUnitHasProperty(mark.cluster, SkUnicode::CodeUnitFlags::kGraphemeStart)) {
            continue;
        }
        ++marks;
        const size_t baseCluster = impl->findPreviousGraphemeBoundary(mark.cluster);
        const Glyph* base = nullptr;
        for (const Glyph& g : glyphs) {
            if (g.cluster == baseCluster) {
                base = &g;
            }
        }
        REPORTER_ASSERT(
                reporter, base, "mark at %zu: no base glyph at %zu", mark.cluster, baseCluster);
        if (!base) {
            continue;
        }
        // Skia y grows downwards: a fatha starts above the baseline and inside its base's box
        REPORTER_ASSERT(reporter,
                        base->drawn.fX <= mark.drawn.fX &&
                                mark.drawn.fX <= base->drawn.fX + base->advance &&
                                mark.drawn.fY < baseline,
                        "mark at %zu drawn at (%g,%g), base at %g advance %g, baseline %g",
                        mark.cluster,
                        mark.drawn.fX,
                        mark.drawn.fY,
                        base->drawn.fX,
                        base->advance,
                        baseline);
    }
    REPORTER_ASSERT(reporter, marks == 2, "%d fatha glyphs", marks);
}

// A C0/C1 control with Bidi_Class BN draws nothing and takes no width, as in a native CTLine
// (HarfBuzz shows .notdef instead). A run of only such controls keeps one kCGFontIndexInvalid
// (0xFFFF) placeholder, which is no glyph id of the run's font
DEF_TEST(CoreTextPipeline_ControlCharacter_InvisibleAndZeroWidth, reporter) {
    auto fontCollection = make_ahem_collection();
    if (!fontCollection) {
        return;
    }
    struct Case {
        const char* text;
        const char* withoutControl;
    };
    for (const Case& c : {Case{"\x01", ""},
                          Case{"\x1F", ""},
                          Case{"a\x01"
                               "b",
                               "ab"},
                          Case{"a\x1B"
                               "b",
                               "ab"},
                          Case{"a\x7F"
                               "b",
                               "ab"},
                          Case{"a\xC2\x90"
                               "b",
                               "ab"},
                          Case{"\x01"
                               "b",
                               "b"},
                          Case{"a\x01", "a"},
                          Case{"\xD7\x90\x01\xD7\x91", "\xD7\x90\xD7\x91"}}) {
        for (TextDirection direction : {TextDirection::kLtr, TextDirection::kRtl}) {
            auto ct = layout_ahem(fontCollection, c.text, direction, 0, 1000);
            auto plain = layout_ahem(fontCollection, c.withoutControl, direction, 0, 1000);
            auto* ctImpl = static_cast<ParagraphImpl*>(ct.get());
            bool validGlyphs = true;
            for (auto& run : ctImpl->runs()) {
                for (SkGlyphID glyph : run.glyphs()) {
                    validGlyphs &= glyph < run.font().getTypeface()->countGlyphs();
                }
            }
            // Caret, hit test and selection need clusters that tile the whole text, the dropped
            // control's bytes included
            size_t covered = 0;
            for (const Cluster& cluster : ctImpl->clusters()) {
                if (cluster.textRange().start == covered) {
                    covered = cluster.textRange().end;
                }
            }
            REPORTER_ASSERT(reporter,
                            covered == strlen(c.text),
                            "len=%zu dir=%d: covered %zu",
                            strlen(c.text),
                            static_cast<int>(direction),
                            covered);
            // An empty paragraph is never shaped and reports -1 unresolved glyphs
            const int expectedUnresolved = c.withoutControl[0] ? plain->unresolvedGlyphs() : 0;
            REPORTER_ASSERT(reporter,
                            SkScalarNearlyEqual(ct->getMaxIntrinsicWidth(),
                                                plain->getMaxIntrinsicWidth()) &&
                                    validGlyphs &&
                                    ctImpl->unresolvedGlyphs() == expectedUnresolved &&
                                    ct->getHeight() > 0,
                            "len=%zu dir=%d: w=%g without control w=%g valid=%d unresolved=%d "
                            "vs %d h=%g",
                            strlen(c.text),
                            static_cast<int>(direction),
                            ct->getMaxIntrinsicWidth(),
                            plain->getMaxIntrinsicWidth(),
                            validGlyphs,
                            ctImpl->unresolvedGlyphs(),
                            expectedUnresolved,
                            ct->getHeight());
        }
    }
}

namespace {
// Glyph positions of every run of paragraph, end sentinels included, that are not whole pixels.
// Run positions are the shaper's accumulated glyph advances, which is the contract under test
int count_fractional_positions(skiatest::Reporter* reporter, Paragraph* paragraph) {
    SkSpan<Run> runs = static_cast<ParagraphImpl*>(paragraph)->runs();
    REPORTER_ASSERT(reporter, !runs.empty());
    int fractional = 0;
    for (const Run& run : runs) {
        REPORTER_ASSERT(reporter, run.size() > 0, "got %zu glyphs", run.size());
        for (size_t i = 0; i <= run.size(); ++i) {
            fractional += run.positions()[i].fX != SkScalarRoundToScalar(run.positions()[i].fX);
        }
    }
    return fractional;
}
}  // namespace

// SH6: a TextStyle with subpixel off lands every glyph advance and the line width on whole
// pixels, while subpixel on preserves fractional advances at fractional font sizes
DEF_TEST(CoreTextPipeline_NonSubpixel_AdvancesWholePixels, reporter) {
    auto ahem = make_ahem_collection();
    if (!ahem) {
        return;
    }
    for (const char* text : {"OK", "Cancel", "Delete all", "Actions section overscroll"}) {
        for (SkScalar fontSize : {16.8f, 14.2f, 13.3f}) {
            for (bool subpixel : {false, true}) {
                skiatest::ReporterContext ctx(
                        reporter,
                        SkStringPrintf("'%s' size=%g subpixel=%d", text, fontSize, subpixel));
                auto paragraph = layout_single_style(ahem, "Ahem", text, fontSize, subpixel);
                REPORTER_ASSERT(reporter,
                                static_cast<ParagraphImpl*>(paragraph.get())->isCoreTextShaped());
                const int fractional = count_fractional_positions(reporter, paragraph.get());
                if (subpixel) {
                    REPORTER_ASSERT(
                            reporter, fractional > 0, "got %d fractional positions", fractional);
                    continue;
                }
                REPORTER_ASSERT(
                        reporter, fractional == 0, "got %d fractional positions", fractional);
                const SkScalar width = paragraph->getLongestLine();
                REPORTER_ASSERT(
                        reporter, width == SkScalarRoundToScalar(width), "got width %.9g", width);
            }
        }
    }
}

// SH6: the ellipsis is shaped apart from the line, and with subpixel off its glyphs must still be
// painted on the whole-pixel grid of the line they end
DEF_TEST(CoreTextPipeline_NonSubpixel_EllipsisOnPixelGrid, reporter) {
    auto ahem = make_ahem_collection();
    if (!ahem) {
        return;
    }
    for (SkScalar fontSize : {16.8f, 14.2f, 13.3f}) {
        skiatest::ReporterContext ctx(reporter, SkStringPrintf("size=%g", fontSize));
        ParagraphStyle paragraphStyle;
        paragraphStyle.setTextShapingEngine(TextShapingEngine::kCoreText);
        paragraphStyle.setApplyRoundingHack(false);
        paragraphStyle.setMaxLines(1);
        paragraphStyle.setEllipsis(u"...");
        TextStyle style;
        style.setFontFamilies({SkString("Ahem")});
        style.setFontSize(fontSize);
        style.setSubpixel(false);
        ParagraphBuilderImpl builder(paragraphStyle, ahem, get_unicode(paragraphStyle));
        builder.pushStyle(style);
        builder.addText("Actions section overscroll");
        auto paragraph = builder.Build();
        paragraph->layout(fontSize * 12.5f);
        REPORTER_ASSERT(reporter, paragraph->didExceedMaxLines());
        const std::vector<SkScalar> ellipsisX = last_blob_x(paragraph.get());
        REPORTER_ASSERT(reporter, ellipsisX.size() == 3, "got %zu glyphs", ellipsisX.size());
        for (size_t i = 0; i < ellipsisX.size(); ++i) {
            REPORTER_ASSERT(reporter,
                            ellipsisX[i] == SkScalarRoundToScalar(ellipsisX[i]),
                            "glyph %zu: got x %.9g",
                            i,
                            ellipsisX[i]);
        }
    }
}

// SH6: CoreText gives ZWJ and soft hyphen no advance while their nominal one is not zero; they
// stay zero, which is also a whole pixel, and the glyphs after them stay on the grid
DEF_TEST(CoreTextPipeline_NonSubpixel_IgnorablesStayOnGrid, reporter) {
    if (!has_font_families({"Helvetica"})) {
        return;
    }
    const char* text =
            "a\xE2\x80\x8D"
            "b\xC2\xAD"
            "c";
    for (SkScalar fontSize : {13.3f, 16.8f, 23.7f}) {
        skiatest::ReporterContext ctx(reporter, SkStringPrintf("size=%g", fontSize));
        auto paragraph =
                layout_single_style(test_font_collection(), "Helvetica", text, fontSize, false);
        const int fractional = count_fractional_positions(reporter, paragraph.get());
        REPORTER_ASSERT(reporter, fractional == 0, "got %d fractional positions", fractional);
    }
}

// SH4: kerning fonts pull kerned pairs ("AV", "To") closer than the sum of their nominal glyph
// advances while keeping the font's cmap glyphs in text order
DEF_TEST(CoreTextPipeline_Kerning_PairNarrowerThanNominal, reporter) {
    auto fontCollection = test_font_collection();
    for (const char* family : {"Helvetica", "Avenir", ".AppleSystemUIFont"}) {
        if (!has_font_families({family})) {
            continue;
        }
        for (const char* text : {"AV", "To"}) {
            for (SkScalar fontSize : {13.3f, 17.0f, 23.7f}) {
                auto paragraph = layout_single_style(fontCollection, family, text, fontSize, true);
                REPORTER_ASSERT(reporter,
                                static_cast<ParagraphImpl*>(paragraph.get())->isCoreTextShaped());
                SkFont font;
                const std::vector<PaintedGlyph> glyphs = painted_glyphs(paragraph.get(), &font);
                SkGlyphID cmap[2] = {};
                font.textToGlyphs(text, 2, SkTextEncoding::kUTF8, cmap);
                REPORTER_ASSERT(
                        reporter,
                        glyphs.size() == 2 && glyphs[0].id == cmap[0] && glyphs[1].id == cmap[1],
                        "%s '%s' size=%g: %zu glyphs, cmap %u %u",
                        family,
                        text,
                        fontSize,
                        glyphs.size(),
                        cmap[0],
                        cmap[1]);
                if (glyphs.size() != 2) {
                    continue;
                }
                // A one-line pair: the line width is the pair's width
                const SkScalar nominal = glyphs[0].nominal + glyphs[1].nominal;
                REPORTER_ASSERT(reporter,
                                paragraph->getLongestLine() < nominal,
                                "%s '%s' size=%g: width %.9g, nominal %.9g",
                                family,
                                text,
                                fontSize,
                                paragraph->getLongestLine(),
                                nominal);
            }
        }
    }
}

namespace {
// 3,534 * 3 = 10,602 UTF-16 code units for "AV " and "fi " (> 10,240 cutoff)
constexpr size_t kUnboundedRepeats = 3534;

// Glyphs drawn for word repeated count times on one 1e7-wide line at 20pt with the CoreText
// engine; visit does not draw the space that ends the line
std::vector<PaintedGlyph> painted_repeated(const sk_sp<FontCollection>& fontCollection,
                                           const char* family,
                                           const char* word,
                                           size_t count) {
    std::string text;
    text.reserve(count * strlen(word));
    for (size_t i = 0; i < count; ++i) {
        text += word;
    }
    ParagraphStyle paragraphStyle;
    paragraphStyle.setTextShapingEngine(TextShapingEngine::kCoreText);
    paragraphStyle.setApplyRoundingHack(false);
    TextStyle style;
    style.setFontFamilies({SkString(family)});
    style.setFontSize(20.0f);
    ParagraphBuilderImpl builder(paragraphStyle, fontCollection, get_unicode(paragraphStyle));
    builder.pushStyle(style);
    builder.addText(text.data(), text.size());
    auto paragraph = builder.Build();
    paragraph->layout(1e7f);
    return painted_glyphs(paragraph.get());
}
}  // namespace

// CTTypesetterCreateWithAttributedString disables GPOS/kerx above 10,240 UTF-16 code units
// unless kCTTypesetterOptionAllowUnboundedLayout is enabled: Roboto keeps "AV" kerning there
DEF_TEST(CoreTextPipeline_UnboundedTypesetterAbove10240Utf16_Kerning, reporter) {
    sk_sp<SkTypeface> roboto = ToolUtils::CreateTypefaceFromResource("fonts/Roboto-Regular.ttf");
    if (!roboto) {
        return;
    }
    auto fontCollection = make_roboto_collection(roboto);
    const std::vector<PaintedGlyph> shortAv = painted_repeated(fontCollection, "Roboto", "AV ", 1);
    const std::vector<PaintedGlyph> longAv =
            painted_repeated(fontCollection, "Roboto", "AV ", kUnboundedRepeats);
    REPORTER_ASSERT(reporter, shortAv.size() == 2, "got %zu glyphs", shortAv.size());
    REPORTER_ASSERT(reporter,
                    longAv.size() == 3 * kUnboundedRepeats - 1,
                    "got %zu glyphs, want %zu",
                    longAv.size(),
                    3 * kUnboundedRepeats - 1);
    if (shortAv.size() < 2 || longAv.size() < 2) {
        return;
    }
    const SkScalar shortPairAdv = shortAv[1].drawnX - shortAv[0].drawnX;
    const SkScalar longPairAdv = longAv[1].drawnX - longAv[0].drawnX;
    REPORTER_ASSERT(reporter,
                    longPairAdv == shortPairAdv,
                    "got 'A' advance %.9g, want %.9g",
                    longPairAdv,
                    shortPairAdv);
}

// Above 10,240 UTF-16 code units GSUB/morx stay on too: each Roboto "fi " still draws the fi
// ligature, so the long text draws two glyphs per word but the unrendered final space
DEF_TEST(CoreTextPipeline_UnboundedTypesetterAbove10240Utf16_Ligatures, reporter) {
    sk_sp<SkTypeface> roboto = ToolUtils::CreateTypefaceFromResource("fonts/Roboto-Regular.ttf");
    if (!roboto) {
        return;
    }
    auto fontCollection = make_roboto_collection(roboto);
    const size_t shortCount = painted_repeated(fontCollection, "Roboto", "fi ", 1).size();
    const size_t longCount =
            painted_repeated(fontCollection, "Roboto", "fi ", kUnboundedRepeats).size();
    REPORTER_ASSERT(reporter, shortCount == 1, "got %zu glyphs", shortCount);
    REPORTER_ASSERT(reporter,
                    longCount == 2 * kUnboundedRepeats - 1,
                    "got %zu glyphs, want %zu",
                    longCount,
                    2 * kUnboundedRepeats - 1);
}

// Arabic joining forms survive above 10,240 UTF-16 code units: each word draws as many glyphs as
// it does alone, plus one for each drawn inter-word space
DEF_TEST(CoreTextPipeline_UnboundedTypesetterAbove10240Utf16_ArabicJoining, reporter) {
    if (!has_font_families({"Geeza Pro"})) {
        return;
    }
    // "سلام " is 5 UTF-16 code units; 2,120 * 5 = 10,600 UTF-16 code units
    constexpr size_t kArabicRepeats = 2120;
    const char* word = "\xD8\xB3\xD9\x84\xD8\xA7\xD9\x85 ";
    auto fontCollection = test_font_collection();
    const size_t shortCount = painted_repeated(fontCollection, "Geeza Pro", word, 1).size();
    const size_t longCount =
            painted_repeated(fontCollection, "Geeza Pro", word, kArabicRepeats).size();
    const size_t expected = kArabicRepeats * shortCount + (kArabicRepeats - 1);
    REPORTER_ASSERT(reporter,
                    longCount == expected,
                    "got %zu glyphs, want %zu (%zu per word)",
                    longCount,
                    expected,
                    shortCount);
}
