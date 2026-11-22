/*
 * Copyright 2026 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "include/core/SkFontStyle.h"
#include "include/core/SkRefCnt.h"
#include "include/core/SkScalar.h"
#include "include/core/SkString.h"
#include "include/core/SkTypeface.h"
#include "include/core/SkTypes.h"
#include "modules/skparagraph/include/DartTypes.h"
#include "modules/skparagraph/include/FontCollection.h"
#include "modules/skparagraph/include/ParagraphStyle.h"
#include "modules/skparagraph/include/TextStyle.h"
#include "modules/skparagraph/src/ParagraphBuilderImpl.h"
#include "modules/skparagraph/src/ParagraphImpl.h"
#include "modules/skparagraph/src/Run.h"
#include "modules/skparagraph/tests/coretext/CoreTextTestUtils.h"
#include "src/core/SkUTF.h"
#include "src/utils/mac/SkUniqueCFRef.h"
#include "tests/Test.h"
#include "tools/fonts/FontToolUtils.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreText/CoreText.h>
#include <string.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace skia::textlayout;
using namespace skia::textlayout::coretext_test;

// Tests assert the internal ParagraphImpl::isCoreTextShaped(): no public API reports the engine,
// and a silent HarfBuzz fallback would pass every other assert

namespace {
// Width of text in the default family at 20pt; clusters receives the cluster count so the
// caller can tell letter spacing from a changed cluster table
SkScalar tracked_width(skiatest::Reporter* reporter,
                       const sk_sp<FontCollection>& fontCollection,
                       TextShapingEngine engine,
                       const char* text,
                       SkScalar letterSpacing,
                       size_t* clusters) {
    TextStyle style;
    // Pin Helvetica so 0.0f baseline has no .SFNS optical trak table
    style.setFontFamilies({SkString("Helvetica")});
    style.setFontSize(20.0f);
    style.setLetterSpacing(letterSpacing);
    ParagraphStyle paragraphStyle;
    paragraphStyle.setTextShapingEngine(engine);
    ParagraphBuilderImpl builder(paragraphStyle, fontCollection, get_unicode(paragraphStyle));
    builder.pushStyle(style);
    builder.addText(text);
    builder.pop();
    auto paragraph = builder.Build();
    paragraph->layout(1000.0f);
    auto impl = static_cast<ParagraphImpl*>(paragraph.get());
    REPORTER_ASSERT(reporter, impl->isCoreTextShaped() == (engine == TextShapingEngine::kCoreText));
    REPORTER_ASSERT(
            reporter, paragraph->unresolvedGlyphs() == 0, "got %d", paragraph->unresolvedGlyphs());
    *clusters = impl->clusters().size();
    return paragraph->getLongestLine();
}
}  // namespace

// letterSpacing tracks Latin by the full amount but leaves Arabic alone, with the same clusters
// as without spacing, on both engines (css-text-4: no tracking inside cursive scripts)
DEF_TEST(CoreTextPipeline_LetterSpacing_SkipsCursiveScripts, reporter) {
    // The HarfBuzz half needs the test font manager to resolve Arabic by fallback
    if (!ToolUtils::TestFontMgr()->matchFamilyStyleCharacter(
                nullptr, SkFontStyle(), nullptr, 0, 0x0645)) {
        return;
    }
    auto fontCollection = sk_make_sp<FontCollection>();
    fontCollection->setDefaultFontManager(ToolUtils::TestFontMgr());

    // Five letters each, so one tracking unit per code point is the full-tracking width
    const char* kArabic = "\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7";
    const char* kLatin = "Latin";
    const SkScalar kSpacing = 4.0f;
    const SkScalar kFullTracking = 5 * kSpacing;
    // A HarfBuzz-free build aborts on kDefault, so only the CoreText half can run there
    for (TextShapingEngine engine : {
#if !defined(SK_PARAGRAPH_DISABLE_HARFBUZZ) && defined(SK_UNICODE_ICU_IMPLEMENTATION)
                 TextShapingEngine::kDefault,
#endif
                 TextShapingEngine::kCoreText}) {
        const bool coreText = engine == TextShapingEngine::kCoreText;
        const char* name = coreText ? "CoreText" : "HarfBuzz";
        size_t plainClusters = 0;
        size_t trackedClusters = 0;
        const SkScalar latinDelta =
                tracked_width(
                        reporter, fontCollection, engine, kLatin, kSpacing, &trackedClusters) -
                tracked_width(reporter, fontCollection, engine, kLatin, 0.0f, &plainClusters);
        REPORTER_ASSERT(reporter,
                        SkScalarNearlyEqual(latinDelta, kFullTracking, 0.01f),
                        "%s: Latin grew by %g for letterSpacing %g",
                        name,
                        latinDelta,
                        kSpacing);
        REPORTER_ASSERT(reporter,
                        trackedClusters == plainClusters,
                        "%s: %zu Latin clusters tracked vs %zu plain",
                        name,
                        trackedClusters,
                        plainClusters);

        // Both engines leave joining scripts alone (css-text-4 cursive tracking)
        const SkScalar expectedArabic = 0.0f;
        const SkScalar arabicDelta =
                tracked_width(
                        reporter, fontCollection, engine, kArabic, kSpacing, &trackedClusters) -
                tracked_width(reporter, fontCollection, engine, kArabic, 0.0f, &plainClusters);
        REPORTER_ASSERT(reporter,
                        SkScalarNearlyEqual(arabicDelta, expectedArabic, 0.01f),
                        "%s: Arabic grew by %g for letterSpacing %g, expected %g",
                        name,
                        arabicDelta,
                        kSpacing,
                        expectedArabic);
        REPORTER_ASSERT(reporter,
                        trackedClusters == plainClusters,
                        "%s: %zu clusters tracked vs %zu plain",
                        name,
                        trackedClusters,
                        plainClusters);
    }
}

// Bengali, Khmer or Ethiopic after an Arabic word are not cursive, so letterSpacing still spaces
// them while the Arabic word stays unspaced, as on the HarfBuzz path
DEF_TEST(CoreTextPipeline_LetterSpacing_UnlistedScriptAfterArabicIsSpaced, reporter) {
    auto fontCollection = sk_make_sp<FontCollection>();
    fontCollection->setDefaultFontManager(ToolUtils::TestFontMgr());
    const std::string arabic = "\u0645\u0631\u062D\u0628\u0627";
    const SkScalar kSpacing = 4.0f;
    auto delta = [&](const std::string& text) {
        size_t clusters = 0;
        return tracked_width(reporter,
                             fontCollection,
                             TextShapingEngine::kCoreText,
                             text.c_str(),
                             kSpacing,
                             &clusters) -
               tracked_width(reporter,
                             fontCollection,
                             TextShapingEngine::kCoreText,
                             text.c_str(),
                             0.0f,
                             &clusters);
    };
    const SkScalar arabicDelta = delta(arabic);
    REPORTER_ASSERT(reporter,
                    SkScalarNearlyEqual(arabicDelta, 0.0f, 0.01f),
                    "Arabic grew by %g",
                    arabicDelta);
    for (const char* unlisted : {"\u09AC\u09BE\u0982\u09B2\u09BE",
                                 "\u1781\u17D2\u1798\u17C2\u179A",
                                 "\u12A0\u121B\u122D\u129B"}) {
        const SkScalar mixedDelta = delta(arabic + unlisted);
        REPORTER_ASSERT(reporter,
                        mixedDelta >= kSpacing - 0.01f,
                        "%s after Arabic grew by %g for letterSpacing %g",
                        unlisted,
                        mixedDelta,
                        kSpacing);
    }
}

namespace {
struct CursiveSplit {
    SkScalar fCursiveWidth = 0;
    SkScalar fOtherWidth = 0;
    size_t fOtherCodepoints = 0;
    std::vector<SkGlyphID> fCursiveGlyphs;
};

// Splits the CoreText runs of text by Run::isCursiveScript after checking that flag against
// the run's letters; a run of spaces only inherits the script of its neighbors
CursiveSplit split_cursive_runs(skiatest::Reporter* reporter,
                                const sk_sp<FontCollection>& fontCollection,
                                const char* family,
                                const char* text,
                                SkScalar letterSpacing) {
    ParagraphStyle paragraphStyle;
    paragraphStyle.setTextShapingEngine(TextShapingEngine::kCoreText);
    TextStyle style;
    style.setFontFamilies({SkString(family)});
    style.setFontSize(36.0f);
    style.setLetterSpacing(letterSpacing);
    ParagraphBuilderImpl builder(paragraphStyle, fontCollection, get_unicode(paragraphStyle));
    builder.pushStyle(style);
    builder.addText(text);
    builder.pop();
    auto paragraph = builder.Build();
    paragraph->layout(10000.0f);
    auto impl = static_cast<ParagraphImpl*>(paragraph.get());
    REPORTER_ASSERT(reporter, impl->isCoreTextShaped());

    CursiveSplit split;
    for (const Run& run : impl->runs()) {
        bool hasArabic = false;
        bool hasLatin = false;
        size_t codepoints = 0;
        const char* ptr = text + run.textRange().start;
        const char* end = text + run.textRange().end;
        while (ptr < end) {
            SkUnichar u = SkUTF::NextUTF8(&ptr, end);
            SkASSERT(u >= 0);
            hasArabic |= u >= 0x0600 && u <= 0x06FF;
            hasLatin |= (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z');
            ++codepoints;
        }
        REPORTER_ASSERT(reporter, !(hasArabic && hasLatin));
        REPORTER_ASSERT(reporter,
                        run.isCursiveScript() == hasArabic || (!hasArabic && !hasLatin),
                        "%s: run [%zu, %zu) cursive=%d, Arabic=%d, Latin=%d",
                        family,
                        run.textRange().start,
                        run.textRange().end,
                        run.isCursiveScript(),
                        hasArabic,
                        hasLatin);
        if (run.isCursiveScript()) {
            split.fCursiveWidth += run.advance().fX;
            split.fCursiveGlyphs.insert(
                    split.fCursiveGlyphs.end(), run.glyphs().begin(), run.glyphs().end());
        } else {
            split.fOtherWidth += run.advance().fX;
            split.fOtherCodepoints += codepoints;
        }
    }
    return split;
}
}  // namespace

// css-text-4 forbids tracking cursive scripts; the HarfBuzz path skips them in
// ParagraphImpl::applySpacingAndBuildClusterTable, and CTTracking on SF Arabic inserts glyphs
// that break the joins, so CoreText must leave Arabic untouched while Latin keeps its spacing
DEF_TEST(CoreTextPipeline_LetterSpacing_CursiveArabicSystemFonts, reporter) {
    if (!has_font_families({".AppleSystemUIFont", "Geeza Pro"})) {
        return;
    }
    auto fontCollection = test_font_collection();
    const char* kArabic = "مرحبا بالعالم";
    const char* kMixed = "Hello مرحبا بالعالم";
    const SkScalar kSpacing = 10.0f;
    for (const char* family : {".AppleSystemUIFont", "Geeza Pro"}) {
        for (const char* text : {kArabic, kMixed}) {
            skiatest::ReporterContext ctx(reporter, SkStringPrintf("%s '%s'", family, text));
            CursiveSplit plain = split_cursive_runs(reporter, fontCollection, family, text, 0);
            CursiveSplit spaced =
                    split_cursive_runs(reporter, fontCollection, family, text, kSpacing);
            REPORTER_ASSERT(reporter, !plain.fCursiveGlyphs.empty());
            REPORTER_ASSERT(reporter,
                            spaced.fCursiveGlyphs == plain.fCursiveGlyphs,
                            "%zu Arabic glyphs plain, %zu spaced",
                            plain.fCursiveGlyphs.size(),
                            spaced.fCursiveGlyphs.size());
            REPORTER_ASSERT(reporter,
                            SkScalarNearlyEqual(spaced.fCursiveWidth, plain.fCursiveWidth, 0.01f),
                            "Arabic width %g plain, %g spaced",
                            plain.fCursiveWidth,
                            spaced.fCursiveWidth);
            REPORTER_ASSERT(reporter,
                            spaced.fOtherCodepoints == plain.fOtherCodepoints,
                            "got %zu, want %zu",
                            spaced.fOtherCodepoints,
                            plain.fOtherCodepoints);
            // Explicit spacing stacks on the .SFNS auto trak
            const SkScalar otherDelta = spaced.fOtherWidth - plain.fOtherWidth;
            const SkScalar expectedOther = kSpacing * plain.fOtherCodepoints;
            REPORTER_ASSERT(reporter,
                            SkScalarNearlyEqual(otherDelta, expectedOther, 0.01f),
                            "Latin grew by %g, expected %g",
                            otherDelta,
                            expectedOther);
        }
        CursiveSplit mixed = split_cursive_runs(reporter, fontCollection, family, kMixed, 0);
        REPORTER_ASSERT(reporter,
                        mixed.fOtherCodepoints >= 5,
                        "%s: got %zu",
                        family,
                        mixed.fOtherCodepoints);
    }
}

// CoreText can give a U+FEFF after a letter no glyph of its own; spacing that empty cluster
// must move the run end once, so the next run still starts where this one ends
DEF_TEST(CoreTextPipeline_LetterSpacing_EmptyClusterKeepsRunsContiguous, reporter) {
    sk_sp<SkTypeface> roboto = ToolUtils::CreateTypefaceFromResource("fonts/Roboto-Regular.ttf");
    if (!roboto) {
        return;
    }
    auto fontCollection = make_roboto_collection(roboto);
    ParagraphStyle paragraphStyle;
    paragraphStyle.setTextShapingEngine(TextShapingEngine::kCoreText);
    TextStyle style;
    style.setFontFamilies({SkString("Roboto")});
    style.setFontSize(20.0f);
    style.setLetterSpacing(4.0f);
    ParagraphBuilderImpl builder(paragraphStyle, fontCollection, get_unicode(paragraphStyle));
    builder.pushStyle(style);
    builder.addText(
            "a\xEF\xBB\xBF"
            "\xD7\xA9\xD7\x9C");
    auto paragraph = builder.Build();
    paragraph->layout(1000);
    auto runs = static_cast<ParagraphImpl*>(paragraph.get())->runs();
    REPORTER_ASSERT(reporter, runs.size() == 2, "got %zu runs", runs.size());
    if (runs.size() != 2) {
        return;
    }
    const Run& latin = runs[0];
    REPORTER_ASSERT(reporter,
                    latin.size() < latin.textRange().width(),
                    "got %zu glyphs for %zu bytes",
                    latin.size(),
                    latin.textRange().width());
    REPORTER_ASSERT(reporter,
                    latin.positions()[latin.size()].fX == runs[1].positions()[0].fX,
                    "first run ends at %g, second starts at %g",
                    latin.positions()[latin.size()].fX,
                    runs[1].positions()[0].fX);
}

namespace {
// kCoreText paragraph of text on one unwrapped line
std::unique_ptr<Paragraph> layout_spaced(const char* family,
                                         SkScalar size,
                                         SkScalar letterSpacing,
                                         const char* text,
                                         TextDirection direction) {
    ParagraphStyle paragraphStyle;
    paragraphStyle.setTextShapingEngine(TextShapingEngine::kCoreText);
    paragraphStyle.setTextDirection(direction);
    paragraphStyle.setTextAlign(TextAlign::kLeft);
    TextStyle style;
    style.setFontFamilies({SkString(family)});
    style.setFontSize(size);
    style.setLetterSpacing(letterSpacing);
    ParagraphBuilderImpl builder(
            paragraphStyle, test_font_collection(), get_unicode(paragraphStyle));
    builder.pushStyle(style);
    builder.addText(text);
    auto paragraph = builder.Build();
    paragraph->layout(10000);
    return paragraph;
}

// 1/4096pt absorbs 32-bit float accumulation across SkParagraph range and line width sums
constexpr SkScalar kTrackedLineTolerance = 1.0f / 4096;

// One unwrapped kCoreText line in the system font: first with firstSpacing, then second with
// secondSpacing
std::unique_ptr<Paragraph> layout_two_spacings(const char* first,
                                               SkScalar firstSpacing,
                                               const char* second,
                                               SkScalar secondSpacing,
                                               SkScalar size) {
    ParagraphStyle paragraphStyle;
    paragraphStyle.setTextShapingEngine(TextShapingEngine::kCoreText);
    paragraphStyle.setTextAlign(TextAlign::kLeft);
    // The rounding hack snaps box edges to 0.01, more than the float error of a range width
    paragraphStyle.setApplyRoundingHack(false);
    ParagraphBuilderImpl builder(
            paragraphStyle, test_font_collection(), get_unicode(paragraphStyle));
    TextStyle style;
    style.setFontFamilies({SkString(".AppleSystemUIFont")});
    style.setFontSize(size);
    style.setLetterSpacing(firstSpacing);
    builder.pushStyle(style);
    builder.addText(first);
    style.setLetterSpacing(secondSpacing);
    builder.pushStyle(style);
    builder.addText(second);
    auto paragraph = builder.Build();
    paragraph->layout(10000);
    return paragraph;
}

// Summed width of the tight boxes of UTF-16 range [start, end)
SkScalar range_width(Paragraph* paragraph, unsigned start, unsigned end) {
    SkScalar width = 0;
    for (const TextBox& box :
         paragraph->getRectsForRange(start, end, RectHeightStyle::kTight, RectWidthStyle::kTight)) {
        width += box.rect.width();
    }
    return width;
}
}  // namespace

// Each style on one line grows by its own letterSpacing per grapheme over the same line unspaced,
// also with the style boundary inside fallback text, so spacing never leaks across styles
DEF_TEST(CoreTextPipeline_LetterSpacing_MixedStylesSpacePerRange, reporter) {
    if (!has_font_families({".AppleSystemUIFont"})) {
        return;
    }
    struct Case {
        const char* first;
        SkScalar firstSpacing;
        const char* second;
        SkScalar secondSpacing;
    };
    constexpr auto kCases = std::to_array<Case>({
            {"Hello ", -0.41f, "World", 1.0f},
            {"Ab \u6C34\u6C34", 1.0f, "\u6C34 \u05E9\u05DC\u05D5\u05DD", -0.41f},
    });
    for (const Case& c : kCases) {
        // Every code point here is one BMP grapheme, so it is also one UTF-16 unit
        const unsigned split = SkToUInt(SkUTF::CountUTF8(c.first, strlen(c.first)));
        const unsigned end = split + SkToUInt(SkUTF::CountUTF8(c.second, strlen(c.second)));
        for (SkScalar size : {13.0f, 17.0f, 28.0f}) {
            skiatest::ReporterContext context(
                    reporter, SkStringPrintf("'%s%s' %gpt", c.first, c.second, size));
            auto spaced =
                    layout_two_spacings(c.first, c.firstSpacing, c.second, c.secondSpacing, size);
            auto plain = layout_two_spacings(c.first, 0, c.second, 0, size);
            REPORTER_ASSERT(reporter,
                            static_cast<ParagraphImpl*>(spaced.get())->isCoreTextShaped());
            const SkScalar firstGrowth =
                    range_width(spaced.get(), 0, split) - range_width(plain.get(), 0, split);
            const SkScalar secondGrowth =
                    range_width(spaced.get(), split, end) - range_width(plain.get(), split, end);
            REPORTER_ASSERT(
                    reporter,
                    SkScalarNearlyEqual(firstGrowth, c.firstSpacing * split, kTrackedLineTolerance),
                    "first style grew by %.5f, want %.5f",
                    firstGrowth,
                    c.firstSpacing * split);
            REPORTER_ASSERT(
                    reporter,
                    SkScalarNearlyEqual(
                            secondGrowth, c.secondSpacing * (end - split), kTrackedLineTolerance),
                    "second style grew by %.5f, want %.5f",
                    secondGrowth,
                    c.secondSpacing * (end - split));
        }
    }
}

// A letter-spaced ellipsis that falls back keeps the fallback font's trak, as the line text
// does. By Flutter convention SkParagraph adds no letterSpacing to the ellipsis (a native
// truncation token would take the tracking), so it is as wide as its text alone unspaced
DEF_TEST(CoreTextPipeline_LetterSpacing_FallbackEllipsisKeepsTrak, reporter) {
    if (!has_font_families({".AppleSystemUIFont"})) {
        return;
    }
    auto fontCollection = test_font_collection();
    for (const char* ellipsis : {"\u6C34\u6C34", "\U0001F600"}) {
        for (SkScalar size : {9.0f, 17.0f, 40.0f}) {
            ParagraphStyle paragraphStyle;
            paragraphStyle.setTextShapingEngine(TextShapingEngine::kCoreText);
            paragraphStyle.setMaxLines(1);
            paragraphStyle.setEllipsis(SkString(ellipsis));
            TextStyle style;
            style.setFontFamilies({SkString(".AppleSystemUIFont")});
            style.setFontSize(size);
            style.setLetterSpacing(1.0f);
            ParagraphBuilderImpl builder(
                    paragraphStyle, fontCollection, get_unicode(paragraphStyle));
            builder.pushStyle(style);
            builder.addText("The quick brown fox jumps over the lazy dog again and again");
            auto paragraph = builder.Build();
            paragraph->layout(10 * size);
            auto impl = static_cast<ParagraphImpl*>(paragraph.get());
            REPORTER_ASSERT(reporter,
                            impl->isCoreTextShaped() && impl->lines().size() == 1,
                            "got %zu lines",
                            impl->lines().size());
            const Run* ellipsisRun = impl->lines().empty() ? nullptr : impl->lines()[0].ellipsis();
            REPORTER_ASSERT(reporter,
                            ellipsisRun && ellipsisRun->isResolved(),
                            "size %g: no resolved ellipsis",
                            size);
            if (!ellipsisRun) {
                continue;
            }

            SkUniqueCFRef<CTFontRef> system(
                    CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, size, nullptr));
            SkUniqueCFRef<CFStringRef> ellipsisString(CFStringCreateWithCString(
                    kCFAllocatorDefault, ellipsis, kCFStringEncodingUTF8));
            SkUniqueCFRef<CTFontRef> fallback(
                    CTFontCreateForString(system.get(),
                                          ellipsisString.get(),
                                          CFRangeMake(0, CFStringGetLength(ellipsisString.get()))));
            SkUniqueCFRef<CFStringRef> fallbackPsName(CTFontCopyPostScriptName(fallback.get()));
            char fallbackName[128] = {};
            CFStringGetCString(fallbackPsName.get(),
                               fallbackName,
                               sizeof(fallbackName),
                               kCFStringEncodingUTF8);
            SkString runName;
            ellipsisRun->font().getTypeface()->getPostScriptName(&runName);
            REPORTER_ASSERT(reporter,
                            runName.equals(fallbackName),
                            "size %g: ellipsis %s, %s",
                            size,
                            runName.c_str(),
                            fallbackName);
            // The same text alone and unspaced must keep the trak, so it cannot match the
            // trak-free copy of the face; otherwise the width check below proves nothing
            auto alone =
                    layout_spaced(".AppleSystemUIFont", size, 0, ellipsis, TextDirection::kLtr);
            const SkScalar unspaced = alone->getLongestLine();
            SkUniqueCFRef<CTFontRef> untracked = untracked_copy(fallback.get());
            const SkScalar untrackedWidth = coretext_line(untracked.get(), ellipsis).width;
            REPORTER_ASSERT(reporter,
                            !SkScalarNearlyEqual(unspaced, untrackedWidth, 0.01f),
                            "size %g %s: no trak, %.5f with and without",
                            size,
                            runName.c_str(),
                            unspaced);
            REPORTER_ASSERT(
                    reporter,
                    SkScalarNearlyEqual(ellipsisRun->advance().fX, unspaced, kTrackedLineTolerance),
                    "size %g %s: ellipsis width %.5f, want %.5f as the text alone unspaced",
                    size,
                    runName.c_str(),
                    ellipsisRun->advance().fX,
                    unspaced);
        }
    }
}

// SP1: a one-style CoreText line grows by letterSpacing per grapheme over the same line unspaced
// in either direction, including multi-glyph base+mark graphemes
DEF_TEST(CoreTextPipeline_LetterSpacing_OneStyleWidthGrowsPerGrapheme, reporter) {
    auto fontCollection = make_ahem_collection();
    if (!fontCollection) {
        return;
    }
    for (const char* text : {"FilterChip", "Cancel", "View licenses"}) {
        for (TextDirection direction : {TextDirection::kLtr, TextDirection::kRtl}) {
            auto plain = layout_ahem(fontCollection, text, direction, 0, 1000);
            for (SkScalar letterSpacing : {0.1f, -0.41f}) {
                auto spaced = layout_ahem(fontCollection, text, direction, letterSpacing, 1000);
                REPORTER_ASSERT(reporter,
                                static_cast<ParagraphImpl*>(spaced.get())->isCoreTextShaped());
                // Every character of these ASCII labels is its own grapheme
                const SkScalar growth = letterSpacing * strlen(text);
                REPORTER_ASSERT(reporter,
                                SkScalarNearlyEqual(spaced->getLongestLine(),
                                                    plain->getLongestLine() + growth,
                                                    1e-3f) &&
                                        SkScalarNearlyEqual(spaced->getMaxIntrinsicWidth(),
                                                            plain->getMaxIntrinsicWidth() + growth,
                                                            1e-3f),
                                "%s dir=%d ls=%g: spaced %.9g/%.9g, unspaced %.9g/%.9g",
                                text,
                                static_cast<int>(direction),
                                letterSpacing,
                                spaced->getLongestLine(),
                                spaced->getMaxIntrinsicWidth(),
                                plain->getLongestLine(),
                                plain->getMaxIntrinsicWidth());
            }
        }
    }

    // A base with a combining mark is one grapheme, so it gets one letterSpacing, however many
    // glyphs CoreText shapes it into
    sk_sp<SkTypeface> roboto = ToolUtils::CreateTypefaceFromResource("fonts/Roboto-Regular.ttf");
    if (!roboto) {
        return;
    }
    auto robotoCollection = make_roboto_collection(roboto);
    SkScalar widths[2];
    for (int i = 0; i < 2; ++i) {
        ParagraphStyle paragraphStyle;
        paragraphStyle.setTextShapingEngine(TextShapingEngine::kCoreText);
        TextStyle style;
        style.setFontFamilies({SkString("Roboto")});
        style.setFontSize(20.0f);
        style.setLetterSpacing(i == 0 ? 0.0f : 4.0f);
        ParagraphBuilderImpl builder(paragraphStyle, robotoCollection, get_unicode(paragraphStyle));
        builder.pushStyle(style);
        builder.addText("a\xCC\x81");
        auto paragraph = builder.Build();
        paragraph->layout(500);
        widths[i] = paragraph->getMaxIntrinsicWidth();
    }
    REPORTER_ASSERT(reporter,
                    SkScalarNearlyEqual(widths[1] - widths[0], 4.0f),
                    "spaced %g vs unspaced %g",
                    widths[1],
                    widths[0]);
}

// A grapheme whose trailing ZWJ or VS16 shares the base glyph's CoreText cluster still gets one
// letterSpacing in one style, while a standalone ignorable grapheme (ZWSP) gets none, as kDefault
DEF_TEST(CoreTextPipeline_LetterSpacing_IgnorableEndsGrapheme, reporter) {
    sk_sp<SkTypeface> roboto = ToolUtils::CreateTypefaceFromResource("fonts/Roboto-Regular.ttf");
    if (!roboto) {
        return;
    }
    auto fontCollection = make_roboto_collection(roboto);
    constexpr SkScalar kLetterSpacing = 4.0f;
    struct Case {
        const char* text;
        int spacedGraphemes;
    };
    const Case kCases[] = {
            {"a\xE2\x80\x8D"
             "b",
             2},  // a+ZWJ, b
            {"a\xEF\xB8\x8F"
             "b",
             2},  // a+VS16, b
            {"a\xE2\x80\x8B"
             "b",
             2},  // a, ZWSP (not spaced), b
            {"a\xE2\x9D\xA4\xEF\xB8\x8F"
             "b",
             3},  // a, emoji heart+VS16 (fallback font), b
            {"\xE8\x91\x9B\xF3\xA0\x84\x80\xE8\x91\x9B", 2},  // Han+IVS, Han
            // Hebrew runs are RTL, so clusters come in descending text order
            {"\xD7\x90\xEF\xB8\x8F\xD7\x91", 2},  // alef+VS16, bet
            {"\xD7\x90\xE2\x80\x8B\xD7\x91", 2},  // alef, ZWSP (not spaced), bet
    };
    for (const Case& c : kCases) {
        for (TextDirection direction : {TextDirection::kLtr, TextDirection::kRtl}) {
            skiatest::ReporterContext ctx(
                    reporter, SkStringPrintf("%s dir=%d", c.text, static_cast<int>(direction)));
            SkScalar widths[2];
            for (int i = 0; i < 2; ++i) {
                ParagraphStyle paragraphStyle;
                paragraphStyle.setTextShapingEngine(TextShapingEngine::kCoreText);
                paragraphStyle.setTextDirection(direction);
                TextStyle style;
                style.setFontFamilies({SkString("Roboto")});
                style.setFontSize(20.0f);
                style.setLetterSpacing(i == 0 ? 0.0f : kLetterSpacing);
                ParagraphBuilderImpl builder(
                        paragraphStyle, fontCollection, get_unicode(paragraphStyle));
                builder.pushStyle(style);
                builder.addText(c.text);
                auto paragraph = builder.Build();
                paragraph->layout(500);
                widths[i] = paragraph->getMaxIntrinsicWidth();
            }
            const SkScalar expected = kLetterSpacing * c.spacedGraphemes;
            REPORTER_ASSERT(reporter,
                            SkScalarNearlyEqual(widths[1] - widths[0], expected, 1e-3f),
                            "got growth %g, expected %g",
                            widths[1] - widths[0],
                            expected);
        }
    }
}

namespace {
// Lays out text in Roboto with letterSpacing 4; a color-only style change at splitAt sends
// ParagraphImpl::applySpacingAndBuildClusterTable through its per-cluster path
std::unique_ptr<Paragraph> layout_spaced_split(const sk_sp<FontCollection>& fontCollection,
                                               const char* text,
                                               size_t splitAt,
                                               TextDirection direction) {
    ParagraphStyle paragraphStyle;
    paragraphStyle.setTextShapingEngine(TextShapingEngine::kCoreText);
    paragraphStyle.setTextDirection(direction);
    ParagraphStyle unrounded = paragraphStyle;
    unrounded.setApplyRoundingHack(false);
    TextStyle style;
    style.setFontFamilies({SkString("Roboto")});
    style.setFontSize(20.0f);
    style.setLetterSpacing(4.0f);
    style.setColor(SK_ColorBLACK);
    ParagraphBuilderImpl builder(unrounded, fontCollection, get_unicode(unrounded));
    builder.pushStyle(style);
    builder.addText(text, splitAt);
    if (splitAt < strlen(text)) {
        style.setColor(SK_ColorRED);
        builder.pushStyle(style);
        builder.addText(text + splitAt, strlen(text) - splitAt);
    }
    auto paragraph = builder.Build();
    paragraph->layout(1000);
    return paragraph;
}
}  // namespace

// CoreText shapes a Hebrew point as its own cluster. The per-cluster spacing path walks RTL
// runs in descending text order, and must still keep the point on its base and give an LTR
// line its half-letterSpacing leading shift across a style split
DEF_TEST(CoreTextPipeline_LetterSpacing_StyleSplitKeepsMarkOnBase, reporter) {
    sk_sp<SkTypeface> roboto = ToolUtils::CreateTypefaceFromResource("fonts/Roboto-Regular.ttf");
    if (!roboto) {
        return;
    }
    auto fontCollection = make_roboto_collection(roboto);
    const char* shalom = "\xD7\xA9\xD6\xB8\xD7\x9C\xD7\x95\xD6\xB9\xD7\x9D";
    auto whole = layout_spaced_split(fontCollection, shalom, strlen(shalom), TextDirection::kRtl);
    auto split = layout_spaced_split(fontCollection, shalom, 4, TextDirection::kRtl);
    auto wholeRuns = static_cast<ParagraphImpl*>(whole.get())->runs();
    auto splitRuns = static_cast<ParagraphImpl*>(split.get())->runs();
    REPORTER_ASSERT(reporter,
                    wholeRuns.size() == 1 && splitRuns.size() == 1,
                    "got %zu and %zu runs",
                    wholeRuns.size(),
                    splitRuns.size());
    if (wholeRuns.size() != 1 || splitRuns.size() != 1) {
        return;
    }
    const Run& wholeRun = wholeRuns[0];
    const Run& splitRun = splitRuns[0];
    bool hasSeparateMark = false;
    for (size_t i = 0; i + 1 < wholeRun.size(); ++i) {
        hasSeparateMark |= wholeRun.positions()[i].fX == wholeRun.positions()[i + 1].fX;
    }
    REPORTER_ASSERT(reporter, hasSeparateMark);
    REPORTER_ASSERT(reporter,
                    wholeRun.size() == splitRun.size(),
                    "got %zu, want %zu",
                    splitRun.size(),
                    wholeRun.size());
    for (size_t i = 0; i <= std::min(wholeRun.size(), splitRun.size()); ++i) {
        REPORTER_ASSERT(reporter,
                        wholeRun.positions()[i].fX == splitRun.positions()[i].fX,
                        "glyph %zu: one style %g, two styles %g",
                        i,
                        wholeRun.positions()[i].fX,
                        splitRun.positions()[i].fX);
    }

    // CoreText puts x and U+0301 in two clusters; the line starts at letterSpacing / 2 (2.0f)
    const char* accented =
            "x\xCC\x81"
            "bc";
    auto ct = layout_spaced_split(fontCollection, accented, 3, TextDirection::kLtr);
    auto unsplit =
            layout_spaced_split(fontCollection, accented, strlen(accented), TextDirection::kLtr);
    auto ctBoxes = ct->getRectsForRange(0, 2, RectHeightStyle::kTight, RectWidthStyle::kTight);
    auto unsplitBoxes =
            unsplit->getRectsForRange(0, 2, RectHeightStyle::kTight, RectWidthStyle::kTight);
    REPORTER_ASSERT(reporter,
                    ctBoxes.size() == 1 && unsplitBoxes.size() == 1,
                    "got %zu and %zu boxes",
                    ctBoxes.size(),
                    unsplitBoxes.size());
    if (ctBoxes.size() == 1 && unsplitBoxes.size() == 1) {
        REPORTER_ASSERT(reporter,
                        ctBoxes[0].rect.fLeft == 2.0f && ctBoxes[0].rect == unsplitBoxes[0].rect,
                        "got [%g, %g], want [2, %g]",
                        ctBoxes[0].rect.fLeft,
                        ctBoxes[0].rect.fRight,
                        unsplitBoxes[0].rect.fRight);
    }
}

// SP1: CoreText shapes each hard-break segment in its own CTLine, and letterSpacing must still
// grow each line width by letterSpacing per grapheme over the same line unspaced
DEF_TEST(CoreTextPipeline_HardBreak_LetterSpacedLinesGrowPerGrapheme, reporter) {
    auto fontCollection = make_ahem_collection();
    if (!fontCollection) {
        return;
    }
    auto layout = [&](const char* text, SkScalar letterSpacing) {
        ParagraphStyle paragraphStyle;
        paragraphStyle.setTextShapingEngine(TextShapingEngine::kCoreText);
        paragraphStyle.setApplyRoundingHack(false);
        TextStyle style;
        style.setFontFamilies({SkString("Ahem")});
        style.setFontSize(15.0f);
        style.setLetterSpacing(letterSpacing);
        ParagraphBuilderImpl builder(paragraphStyle, fontCollection, get_unicode(paragraphStyle));
        builder.pushStyle(style);
        builder.addText(text);
        auto paragraph = builder.Build();
        paragraph->layout(10000);
        return paragraph;
    };
    struct Case {
        const char* text;
        size_t lines;
    };
    for (const Case& c : {Case{"Lorem ipsum \ndolor sit amet", 2},
                          Case{"Cut\nCopy\nPaste\nSelect all", 4},
                          Case{"Cut\n\nCopy", 3}}) {
        auto plain = layout(c.text, 0);
        std::vector<LineMetrics> plainLines;
        plain->getLineMetrics(plainLines);
        for (SkScalar letterSpacing : {-0.15f, 0.3f}) {
            skiatest::ReporterContext ctx(reporter,
                                          SkStringPrintf("'%s' spacing=%g", c.text, letterSpacing));
            auto spaced = layout(c.text, letterSpacing);
            std::vector<LineMetrics> lines;
            spaced->getLineMetrics(lines);
            REPORTER_ASSERT(reporter,
                            lines.size() == c.lines && plainLines.size() == c.lines,
                            "got %zu lines, %zu unspaced",
                            lines.size(),
                            plainLines.size());
            for (size_t i = 0; i < std::min(lines.size(), plainLines.size()); ++i) {
                // ASCII text has one grapheme per byte and trailing spaces are excluded from fWidth
                const size_t graphemes = lines[i].fEndExcludingWhitespaces - lines[i].fStartIndex;
                const double expected = plainLines[i].fWidth + letterSpacing * graphemes;
                REPORTER_ASSERT(reporter,
                                SkScalarNearlyEqual(lines[i].fWidth, expected, 1e-3f),
                                "line %zu: width %.9g, expected %.9g",
                                i,
                                lines[i].fWidth,
                                expected);
            }
        }
    }

    // CoreText gives U+0301 its own glyph in Helvetica; spacing a later line's run with the
    // runs before it must still find that grapheme, so the mark stays on its base
    if (has_font_families({"Helvetica"})) {
        auto spaced = [&](const char* text) {
            ParagraphStyle paragraphStyle;
            paragraphStyle.setTextShapingEngine(TextShapingEngine::kCoreText);
            TextStyle style;
            style.setFontFamilies({SkString("Helvetica")});
            style.setFontSize(20.0f);
            style.setLetterSpacing(4.0f);
            ParagraphBuilderImpl builder(
                    paragraphStyle, test_font_collection(), get_unicode(paragraphStyle));
            builder.pushStyle(style);
            builder.addText(text);
            auto paragraph = builder.Build();
            paragraph->layout(1000);
            return paragraph;
        };
        auto twoLines =
                spaced("ab\nx\xCC\x81"
                       "b");
        auto oneLine =
                spaced("x\xCC\x81"
                       "b");
        const std::vector<Paragraph::FontInfo> oneLineFonts = oneLine->getFonts();
        const std::vector<PaintedGlyph> oneLineGlyphs = painted_glyphs(oneLine.get());
        REPORTER_ASSERT(reporter,
                        oneLineFonts.size() == 1 && oneLineGlyphs.size() == 3,
                        "x, U+0301, b: %zu runs, %zu glyphs",
                        oneLineFonts.size(),
                        oneLineGlyphs.size());
        std::vector<LineMetrics> twoLineMetrics, oneLineMetrics;
        twoLines->getLineMetrics(twoLineMetrics);
        oneLine->getLineMetrics(oneLineMetrics);
        REPORTER_ASSERT(reporter,
                        twoLineMetrics.size() == 2 && oneLineMetrics.size() == 1,
                        "got %zu and %zu lines",
                        twoLineMetrics.size(),
                        oneLineMetrics.size());
        if (twoLineMetrics.size() == 2 && oneLineMetrics.size() == 1) {
            REPORTER_ASSERT(reporter,
                            SkScalarNearlyEqual(twoLineMetrics[1].fWidth, oneLineMetrics[0].fWidth),
                            "accented line: after '\\n' %g, alone %g",
                            twoLineMetrics[1].fWidth,
                            oneLineMetrics[0].fWidth);
        }
    }
}

// Multi-line RTL runs are spaced from the visually leftmost segment first; each line must
// still grow by letterSpacing per letter and stay aligned to the right (start) edge
DEF_TEST(CoreTextPipeline_HardBreak_RtlLetterSpacedLinesKeepStartEdge, reporter) {
    if (!has_font_families({"Arial Hebrew"})) {
        return;
    }
    const char* rtlText =
            "\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D\n"
            "\xD7\xA2\xD7\x95\xD7\x9C\xD7\x9D";
    auto rtl = [&](SkScalar letterSpacing) {
        ParagraphStyle paragraphStyle;
        paragraphStyle.setTextShapingEngine(TextShapingEngine::kCoreText);
        paragraphStyle.setTextDirection(TextDirection::kRtl);
        paragraphStyle.setApplyRoundingHack(false);
        TextStyle style;
        style.setFontFamilies({SkString("Arial Hebrew")});
        style.setFontSize(17.3f);
        style.setLetterSpacing(letterSpacing);
        ParagraphBuilderImpl builder(
                paragraphStyle, test_font_collection(), get_unicode(paragraphStyle));
        builder.pushStyle(style);
        builder.addText(rtlText);
        auto paragraph = builder.Build();
        paragraph->layout(10000);
        return paragraph;
    };
    const SkScalar kSpacing = 0.7f;
    auto plain = rtl(0);
    auto spaced = rtl(kSpacing);
    std::vector<LineMetrics> plainLines, lines;
    plain->getLineMetrics(plainLines);
    spaced->getLineMetrics(lines);
    REPORTER_ASSERT(reporter,
                    lines.size() == 2 && plainLines.size() == 2,
                    "got %zu lines, %zu unspaced",
                    lines.size(),
                    plainLines.size());
    for (size_t i = 0; i < std::min(lines.size(), plainLines.size()); ++i) {
        // Four Hebrew letters per line
        const double expected = plainLines[i].fWidth + 4 * kSpacing;
        REPORTER_ASSERT(reporter,
                        SkScalarNearlyEqual(lines[i].fWidth, expected, 1e-3f) &&
                                SkScalarNearlyEqual(lines[i].fLeft + lines[i].fWidth, 10000, 1e-3f),
                        "RTL line %zu: width %.9g left %.9g, expected width %.9g",
                        i,
                        lines[i].fWidth,
                        lines[i].fLeft,
                        expected);
    }
}
