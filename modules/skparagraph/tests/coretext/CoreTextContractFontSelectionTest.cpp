/*
 * Copyright 2026 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

// Contract tests for the CoreText pipeline through the public Paragraph API
// (CoreTextContract_<ID>_<Property>) on TextShapingEngine::kCoreText; glyph positions are never
// compared for equality with a native CTLine or with HarfBuzz. Where an open bug breaks part of a
// contract, the test covers the passing part and names the bug

#include "include/core/SkData.h"
#include "include/core/SkFontArguments.h"
#include "include/core/SkFontParameters.h"
#include "include/core/SkFontStyle.h"
#include "include/core/SkRefCnt.h"
#include "include/core/SkScalar.h"
#include "include/core/SkStream.h"
#include "include/core/SkString.h"
#include "include/core/SkTypeface.h"
#include "include/core/SkTypes.h"
#include "include/ports/SkTypeface_mac.h"
#include "include/private/SkTo.h"
#include "modules/skparagraph/include/DartTypes.h"
#include "modules/skparagraph/include/FontCollection.h"
#include "modules/skparagraph/include/Paragraph.h"
#include "modules/skparagraph/include/ParagraphStyle.h"
#include "modules/skparagraph/include/TextStyle.h"
#include "modules/skparagraph/include/TypefaceFontProvider.h"
#include "modules/skparagraph/src/ParagraphBuilderImpl.h"
#include "modules/skparagraph/src/ParagraphImpl.h"
#include "modules/skparagraph/tests/coretext/CoreTextTestUtils.h"
#include "src/core/SkAdvancedTypefaceMetrics.h"
#include "src/core/SkScalerContext.h"
#include "src/utils/mac/SkUniqueCFRef.h"
#include "tests/Test.h"
#include "tools/Resources.h"
#include "tools/fonts/FontToolUtils.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreText/CoreText.h>

#include <algorithm>
#include <array>
#include <memory>
#include <string_view>
#include <vector>

using namespace skia::textlayout;
using namespace skia::textlayout::coretext_test;

namespace {
// PostScript name of the face drawing the UTF-16 unit at utf16Offset; runs carry a new
// SkTypeface wrapping the bound CTFont, so faces compare by name, never by pointer or uniqueID
SkString fs_postscript_name_at(Paragraph* paragraph, size_t utf16Offset) {
    SkString name;
    sk_sp<SkTypeface> typeface = paragraph->getFontAtUTF16Offset(utf16Offset).refTypeface();
    if (typeface) {
        typeface->getPostScriptName(&name);
    }
    return name;
}

std::vector<SkString> fs_font_families(const Paragraph& paragraph) {
    std::vector<SkString> families;
    for (const Paragraph::FontInfo& info : paragraph.getFonts()) {
        families.push_back(family_name(info.fFont));
    }
    return families;
}

// One span of family at 20pt with the OpenType feature tag set to value; no feature when tag
// is null, which is the default the feature has to change
std::unique_ptr<Paragraph> fs_layout_with_feature(skiatest::Reporter* reporter,
                                                  const sk_sp<FontCollection>& fontCollection,
                                                  const char* family,
                                                  std::string_view text,
                                                  const char* tag,
                                                  int value) {
    TextStyle style = contract_text_style(family, 20.0f);
    if (tag) {
        style.addFontFeature(SkString(tag), value);
    }
    return layout_text(reporter, fontCollection, contract_paragraph_style(), style, text, 1000.0f);
}

// Tight box widths of the ten digits "0123456789" of family at 20pt with feature tag on; -1
// for a digit whose box is missing or split
std::array<SkScalar, 10> fs_digit_widths(skiatest::Reporter* reporter,
                                         const sk_sp<FontCollection>& fontCollection,
                                         const char* family,
                                         const char* tag) {
    auto paragraph = fs_layout_with_feature(reporter, fontCollection, family, "0123456789", tag, 1);
    std::array<SkScalar, 10> widths = {};
    for (size_t i = 0; i < widths.size(); ++i) {
        const std::vector<TextBox> boxes = tight_boxes(paragraph.get(), i, i + 1);
        widths[i] = boxes.size() == 1 ? boxes[0].rect.width() : -1.0f;
    }
    return widths;
}

std::unique_ptr<Paragraph> fs_layout_face(skiatest::Reporter* reporter,
                                          const sk_sp<FontCollection>& fontCollection,
                                          const char* family,
                                          SkScalar size,
                                          SkFontStyle fontStyle,
                                          std::string_view text) {
    TextStyle style = contract_text_style(family, size);
    style.setFontStyle(fontStyle);
    return layout_text(reporter, fontCollection, contract_paragraph_style(), style, text, 1000.0f);
}

// FS1 case: family in fontStyle draws the face FontCollection::findTypefaces picks
void fs_check_installed_face(skiatest::Reporter* reporter,
                             const sk_sp<FontCollection>& fontCollection,
                             const char* family,
                             SkFontStyle fontStyle) {
    skiatest::ReporterContext context(reporter,
                                      SkStringPrintf("%s w%d width %d slant %d",
                                                     family,
                                                     fontStyle.weight(),
                                                     fontStyle.width(),
                                                     static_cast<int>(fontStyle.slant())));
    auto expected = fontCollection->findTypefaces({SkString(family)}, fontStyle);
    REPORTER_ASSERT(reporter, expected.size() == 1, "got %zu faces", expected.size());
    if (expected.size() != 1) {
        return;
    }
    auto paragraph =
            fs_layout_face(reporter, fontCollection, family, 16.0f, fontStyle, "Hamburgefonstiv");
    SkString want;
    expected[0]->getPostScriptName(&want);
    const SkString got = fs_postscript_name_at(paragraph.get(), 0);
    REPORTER_ASSERT(reporter, got == want, "got %s, want %s", got.c_str(), want.c_str());
}

// FS5 sample: U+76F4, a unified Han code point whose regional forms differ, in one span of the
// system font tagged with locale
std::unique_ptr<Paragraph> fs_layout_han(skiatest::Reporter* reporter,
                                         const sk_sp<FontCollection>& fontCollection,
                                         const char* locale) {
    TextStyle style = contract_text_style(".AppleSystemUIFont", 16.0f);
    style.setLocale(SkString(locale));
    return layout_text(
            reporter, fontCollection, contract_paragraph_style(), style, "\xE7\x9B\xB4", 1000.0f);
}

// Roboto "a", then a nested span "b" whose only family is not in the collection
void add_missing_child_family_spans(ParagraphBuilderImpl* builder) {
    builder->pushStyle(contract_text_style("Roboto", 20.0f));
    builder->addText("a");
    builder->pushStyle(contract_text_style("NoSuchFamily", 20.0f));
    builder->addText("b");
}
}  // namespace

// FS1: an installed family draws the face FontCollection::findTypefaces picks for any weight,
// width and slant, so switching engines never changes the face of an existing Flutter app
DEF_TEST(CoreTextContract_FS1_InstalledFaceMatchesFontCollection, reporter) {
    constexpr std::array<const char*, 9> kFamilies = {"Georgia",
                                                      "Didot",
                                                      "Futura",
                                                      "Helvetica",
                                                      "Helvetica Neue",
                                                      "Menlo",
                                                      "Optima",
                                                      "Avenir Next",
                                                      "American Typewriter"};
    // Off-grid weights catch a matcher that rounds to the nearest hundred
    constexpr std::array<int, 11> kWeights = {1, 150, 300, 450, 500, 550, 700, 800, 900, 950, 1000};
    sk_sp<FontCollection> fontCollection = system_collection();
    for (const char* family : kFamilies) {
        if (!has_font_families({family})) {
            continue;
        }
        for (int weight : kWeights) {
            for (auto width : {SkFontStyle::kNormal_Width, SkFontStyle::kCondensed_Width}) {
                for (auto slant : {SkFontStyle::kUpright_Slant,
                                   SkFontStyle::kItalic_Slant,
                                   SkFontStyle::kOblique_Slant}) {
                    fs_check_installed_face(
                            reporter, fontCollection, family, SkFontStyle(weight, width, slant));
                }
            }
        }
    }
}

// FS2: families are tried in list order and an app font beats an installed family of the same
// name, so a Flutter app bundling its own "Helvetica" never gets Apple's
DEF_TEST(CoreTextContract_FS2_FamilyOrderAssetFirst, reporter) {
    sk_sp<FontCollection> fontCollection = asset_collection(
            {{"fonts/ahem.ttf", "Ahem"}, {"fonts/Roboto-Regular.ttf", "Helvetica"}});
    if (!fontCollection) {
        return;
    }
    fontCollection->enableFontFallback();
    fontCollection->setDefaultFontManager(ToolUtils::TestFontMgr());
    struct FamilyOrderCase {
        std::vector<SkString> families;
        const char* postscriptName;
    };
    const std::array<FamilyOrderCase, 3> kCases = {{
            {{SkString("NoSuch"), SkString("Ahem"), SkString("Helvetica")}, "Ahem"},
            {{SkString("NoSuch"), SkString("Helvetica"), SkString("Ahem")}, "Roboto-Regular"},
            {{SkString("Helvetica")}, "Roboto-Regular"},
    }};
    for (const FamilyOrderCase& c : kCases) {
        skiatest::ReporterContext context(reporter, SkStringPrintf("first %s", c.postscriptName));
        TextStyle style = contract_text_style("", 20.0f);
        style.setFontFamilies(c.families);
        auto paragraph = layout_text(
                reporter, fontCollection, contract_paragraph_style(), style, "abc", 1000.0f);
        const SkString got = fs_postscript_name_at(paragraph.get(), 0);
        REPORTER_ASSERT(reporter, got.equals(c.postscriptName), "got %s", got.c_str());
    }
}

// FS3: with fallback off (--use-test-fonts) only app fonts draw and a missing character is tofu;
// the text has no partial clusters while B1 (a missing mark pulls its base into tofu) is open
DEF_TEST(CoreTextContract_FS3_DisabledFallbackOnlyAppFonts, reporter) {
    sk_sp<FontCollection> fontCollection = asset_collection({{"fonts/ahem.ttf", "Ahem"}});
    if (!fontCollection) {
        return;
    }
    auto paragraph = layout_text(reporter,
                                 fontCollection,
                                 contract_paragraph_style(),
                                 contract_text_style("Ahem", 20.0f),
                                 "ab\xE4\xB8\xAD",
                                 1000.0f);
    REPORTER_ASSERT(reporter,
                    paragraph->unresolvedCodepoints() == std::unordered_set<SkUnichar>{0x4E2D});
    for (const SkString& family : fs_font_families(*paragraph)) {
        REPORTER_ASSERT(reporter, family.equals("Ahem"), "got %s", family.c_str());
    }
    const std::vector<SkGlyphID> glyphs = drawn_glyph_ids(paragraph.get());
    REPORTER_ASSERT(reporter, glyphs.size() == 3, "got %zu glyphs", glyphs.size());
    if (glyphs.size() != 3) {
        return;
    }
    REPORTER_ASSERT(reporter, glyphs[0] != 0 && glyphs[1] != 0, "got %u, %u", glyphs[0], glyphs[1]);
    REPORTER_ASSERT(reporter, glyphs[2] == 0, "got glyph %d", glyphs[2]);
}

// FS3 / R129: an empty fontFamilies list resolves the FontCollection default or first app font
// instead of .AppleSystemUIFont, and unstyled gaps use ParagraphStyle::getTextStyle()
DEF_TEST(CoreTextContract_FS3_EmptyFamiliesAndUnstyledGaps, reporter) {
    sk_sp<FontCollection> fontCollection =
            asset_collection({{"fonts/Roboto-Regular.ttf", "Roboto"}, {"fonts/ahem.ttf", "Ahem"}});
    if (!fontCollection) {
        return;
    }
    TextStyle emptyFamilies;
    emptyFamilies.setFontSize(20.0f);
    emptyFamilies.setFontFamilies({});
    auto standIn = layout_text(
            reporter, fontCollection, contract_paragraph_style(), emptyFamilies, "Hello", 1000.0f);
    REPORTER_ASSERT(reporter,
                    standIn->unresolvedGlyphs() == 0,
                    "got %d unresolved",
                    standIn->unresolvedGlyphs());
    const SkString standInFace = fs_postscript_name_at(standIn.get(), 0);
    REPORTER_ASSERT(reporter, standInFace.equals("Roboto-Regular"), "got %s", standInFace.c_str());

    fontCollection->setDefaultFontManager(nullptr, "Ahem");
    auto fromDefault = layout_text(
            reporter, fontCollection, contract_paragraph_style(), emptyFamilies, "Hello", 1000.0f);
    REPORTER_ASSERT(reporter,
                    fromDefault->unresolvedGlyphs() == 0,
                    "got %d unresolved",
                    fromDefault->unresolvedGlyphs());
    const SkString defaultFace = fs_postscript_name_at(fromDefault.get(), 0);
    REPORTER_ASSERT(reporter, defaultFace.equals("Ahem"), "got %s", defaultFace.c_str());

    // ParagraphBuilderImpl starts a default Block in its constructor, so construct ParagraphImpl
    // directly to verify leading and trailing unstyled gaps shape with default TextStyle (30px)
    ParagraphStyle gapParagraphStyle = contract_paragraph_style();
    const TextStyle ahem30 = contract_text_style("Ahem", 30.0f);
    gapParagraphStyle.setTextStyle(ahem30);
    skia_private::TArray<Block> gapBlocks;
    gapBlocks.emplace_back(1, 2, contract_text_style("Ahem", 10.0f));
    skia_private::TArray<Placeholder> placeholders;
    placeholders.emplace_back(
            3, 3, PlaceholderStyle(), ahem30, BlockRange(0, 1), skia::textlayout::TextRange(0, 3));
    ParagraphImpl gapParagraph(SkString("abc"),
                               gapParagraphStyle,
                               std::move(gapBlocks),
                               std::move(placeholders),
                               fontCollection,
                               contract_unicode());
    gapParagraph.layout(1000.0f);
    REPORTER_ASSERT(reporter, gapParagraph.isCoreTextShaped());
    REPORTER_ASSERT(reporter,
                    gapParagraph.unresolvedGlyphs() == 0,
                    "got %d unresolved",
                    gapParagraph.unresolvedGlyphs());
    REPORTER_ASSERT(reporter,
                    SkScalarNearlyEqual(gapParagraph.getMaxIntrinsicWidth(), 70.0f, 0.05f),
                    "got %g, want 70",
                    gapParagraph.getMaxIntrinsicWidth());
}

// FS4: with fallback on, CoreText's cascade resolves what the system font lacks; a code point no
// font has draws tofu and counts as unresolved, never LastResort's glyph Flutter would paint
DEF_TEST(CoreTextContract_FS4_FallbackOrTofu, reporter) {
    if (!has_font_families({".AppleSystemUIFont"})) {
        return;
    }
    sk_sp<FontCollection> fontCollection = system_collection();
    const TextStyle style = contract_text_style(".AppleSystemUIFont", 16.0f);
    // Latin, Han, emoji and Arabic need four different fallback faces
    auto covered = layout_text(reporter,
                               fontCollection,
                               contract_paragraph_style(),
                               style,
                               "a\xE4\xB8\xAD\xF0\x9F\x98\x80\xD8\xA8",
                               1000.0f);
    REPORTER_ASSERT(
            reporter, covered->unresolvedGlyphs() == 0, "got %d", covered->unresolvedGlyphs());
    // U+0378 is unassigned, so no installed font maps it
    auto uncovered = layout_text(
            reporter, fontCollection, contract_paragraph_style(), style, "a\xCD\xB8", 1000.0f);
    REPORTER_ASSERT(reporter,
                    uncovered->unresolvedCodepoints() == std::unordered_set<SkUnichar>{0x378});
    const std::vector<SkGlyphID> glyphs = drawn_glyph_ids(uncovered.get());
    REPORTER_ASSERT(reporter, glyphs.size() == 2, "got %zu glyphs", glyphs.size());
    if (glyphs.size() == 2) {
        REPORTER_ASSERT(reporter, glyphs[1] == 0, "got glyph %d", glyphs[1]);
    }
    for (Paragraph* paragraph : {covered.get(), uncovered.get()}) {
        for (const SkString& family : fs_font_families(*paragraph)) {
            REPORTER_ASSERT(reporter, !family.contains("LastResort"), "got %s", family.c_str());
        }
    }
}

// FS5: the span locale picks the regional face for unified Han (ja, zh-Hans, zh-Hant, ko), per
// span, and text outside any span takes the locale of ParagraphStyle's text style
DEF_TEST(CoreTextContract_FS5_LocalePicksHanFace, reporter) {
    if (!has_font_families({".AppleSystemUIFont",
                            "Hiragino Sans",
                            "PingFang SC",
                            "PingFang TC",
                            "Apple SD Gothic Neo"})) {
        return;
    }
    sk_sp<FontCollection> fontCollection = system_collection();
    constexpr std::array<const char*, 4> kLocales = {"ja", "zh-Hans", "zh-Hant", "ko"};
    std::array<SkString, 4> faces;
    for (size_t i = 0; i < kLocales.size(); ++i) {
        faces[i] = fs_postscript_name_at(fs_layout_han(reporter, fontCollection, kLocales[i]).get(),
                                         0);
        for (size_t j = 0; j < i; ++j) {
            REPORTER_ASSERT(reporter,
                            faces[i] != faces[j],
                            "%s and %s both got %s",
                            kLocales[i],
                            kLocales[j],
                            faces[i].c_str());
        }
    }

    // Two spans in one paragraph keep their own locales
    auto builder = contract_builder(fontCollection, contract_paragraph_style());
    for (const char* locale : {"ja", "zh-Hant"}) {
        TextStyle style = contract_text_style(".AppleSystemUIFont", 16.0f);
        style.setLocale(SkString(locale));
        builder->pushStyle(style);
        builder->addText("\xE7\x9B\xB4");
        builder->pop();
    }
    auto mixed = layout_built(reporter, builder.get(), 1000.0f);
    const SkString mixedJa = fs_postscript_name_at(mixed.get(), 0);
    const SkString mixedZhHant = fs_postscript_name_at(mixed.get(), 1);
    REPORTER_ASSERT(
            reporter, mixedJa == faces[0], "got %s, want %s", mixedJa.c_str(), faces[0].c_str());
    REPORTER_ASSERT(reporter,
                    mixedZhHant == faces[2],
                    "got %s, want %s",
                    mixedZhHant.c_str(),
                    faces[2].c_str());

    // ko is seldom the pick for text without a locale, so inheriting it shows
    ParagraphStyle paragraphStyle = contract_paragraph_style();
    TextStyle defaultStyle = contract_text_style(".AppleSystemUIFont", 16.0f);
    defaultStyle.setLocale(SkString("ko"));
    paragraphStyle.setTextStyle(defaultStyle);
    auto inheritingBuilder = contract_builder(fontCollection, paragraphStyle);
    inheritingBuilder->addText("\xE7\x9B\xB4");
    auto inherited = layout_built(reporter, inheritingBuilder.get(), 1000.0f);
    const SkString inheritedFace = fs_postscript_name_at(inherited.get(), 0);
    REPORTER_ASSERT(reporter,
                    inheritedFace == faces[3],
                    "got %s, want %s",
                    inheritedFace.c_str(),
                    faces[3].c_str());
}

// FS6: .AppleSystemUIFont, CupertinoSystemText and CupertinoSystemDisplay draw the system font
// before the fallback list (Flutter 147708)
DEF_TEST(CoreTextContract_FS6_SystemAliasBeatsFallback, reporter) {
    if (!has_font_families({".AppleSystemUIFont", "Courier"})) {
        return;
    }
    sk_sp<FontCollection> fontCollection = system_collection();
    for (const char* alias :
         {".AppleSystemUIFont", "CupertinoSystemText", "CupertinoSystemDisplay"}) {
        skiatest::ReporterContext context(reporter, SkString(alias));
        TextStyle style = contract_text_style(alias, 16.0f);
        style.setFontFamilies({SkString(alias), SkString("Courier")});
        auto paragraph = layout_text(
                reporter, fontCollection, contract_paragraph_style(), style, "Hello", 1000.0f);
        const SkString face = fs_postscript_name_at(paragraph.get(), 0);
        REPORTER_ASSERT(reporter, face.startsWith(".SF"), "got %s", face.c_str());
    }
}

// FS6 / R109: Flutter pushes "" ahead of fontFamilyFallback when fontFamily is null, so an empty
// leading family must fall through to Courier while standalone {""} still resolves .SF*
DEF_TEST(CoreTextContract_FS6_EmptyFamilyUsesFallbackFirst, reporter) {
    if (!has_font_families({".AppleSystemUIFont", "Courier"})) {
        return;
    }
    sk_sp<FontCollection> fontCollection = system_collection();
    TextStyle withFallback = contract_text_style("", 16.0f);
    withFallback.setFontFamilies({SkString(""), SkString("Courier")});
    auto courier = layout_text(
            reporter, fontCollection, contract_paragraph_style(), withFallback, "Hello", 1000.0f);
    const SkString courierFace = fs_postscript_name_at(courier.get(), 0);
    REPORTER_ASSERT(reporter, courierFace.startsWith("Courier"), "got %s", courierFace.c_str());

    const TextStyle onlyEmpty = contract_text_style("", 16.0f);
    auto systemDefault = layout_text(
            reporter, fontCollection, contract_paragraph_style(), onlyEmpty, "Hello", 1000.0f);
    const SkString defaultFace = fs_postscript_name_at(systemDefault.get(), 0);
    REPORTER_ASSERT(reporter, defaultFace.startsWith(".SF"), "got %s", defaultFace.c_str());
}

// FS7: Rounded, Serif and Monospaced are distinct system faces at the real point size (Flutter
// 193812): Monospaced advances are equal and opsz follows the size, which a size-0 CTFont pins
DEF_TEST(CoreTextContract_FS7_SystemDesignsDistinct, reporter) {
    if (!has_font_families({".AppleSystemUIFont"})) {
        return;
    }
    sk_sp<FontCollection> fontCollection = system_collection();
    constexpr std::array<const char*, 4> kDesigns = {".AppleSystemUIFont",
                                                     ".AppleSystemUIFontRounded",
                                                     ".AppleSystemUIFontSerif",
                                                     ".AppleSystemUIFontMonospaced"};
    std::array<SkString, 4> faces;
    for (size_t i = 0; i < kDesigns.size(); ++i) {
        auto paragraph = layout_text(reporter,
                                     fontCollection,
                                     contract_paragraph_style(),
                                     contract_text_style(kDesigns[i], 24.0f),
                                     "Hello",
                                     1000.0f);
        faces[i] = fs_postscript_name_at(paragraph.get(), 0);
        for (size_t j = 0; j < i; ++j) {
            REPORTER_ASSERT(reporter,
                            faces[i] != faces[j],
                            "%s and %s both got %s",
                            kDesigns[i],
                            kDesigns[j],
                            faces[i].c_str());
        }
    }

    const char* kMonospaced = ".AppleSystemUIFontMonospaced";
    const SkScalar narrow =
            fs_layout_face(reporter, fontCollection, kMonospaced, 24.0f, SkFontStyle(), "iiii")
                    ->getMaxIntrinsicWidth();
    const SkScalar wide =
            fs_layout_face(reporter, fontCollection, kMonospaced, 24.0f, SkFontStyle(), "WWWW")
                    ->getMaxIntrinsicWidth();
    REPORTER_ASSERT(
            reporter, SkScalarNearlyEqual(narrow, wide, 0.01f), "iiii %g, WWWW %g", narrow, wide);

    // Rounded and Monospaced have no opsz axis
    const SkFourByteTag opsz = SkSetFourByteTag('o', 'p', 's', 'z');
    for (const char* family : {".AppleSystemUIFont", ".AppleSystemUIFontSerif"}) {
        for (SkScalar size : {24.0f, 40.0f}) {
            skiatest::ReporterContext context(reporter, SkStringPrintf("%s %g", family, size));
            auto paragraph = layout_text(reporter,
                                         fontCollection,
                                         contract_paragraph_style(),
                                         contract_text_style(family, size),
                                         "Hello",
                                         1000.0f);
            const SkScalar value =
                    axis_coordinate(paragraph->getFontAtUTF16Offset(0).getTypeface(), opsz)
                            .value_or(-1.0f);
            REPORTER_ASSERT(reporter,
                            SkScalarNearlyEqual(value, size, 0.5f),
                            "got opsz %g, want %g",
                            value,
                            size);
        }
    }
}

// FS8: Arabic in the system font draws with SF Arabic as on iOS (Flutter 144099), not with the
// Geeza Pro the generic cascade picks
DEF_TEST(CoreTextContract_FS8_ArabicUsesSFArabic, reporter) {
    if (!has_font_families({".AppleSystemUIFont"})) {
        return;
    }
    auto paragraph = layout_text(reporter,
                                 system_collection(),
                                 contract_paragraph_style(),
                                 contract_text_style(".AppleSystemUIFont", 20.0f),
                                 "\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7",
                                 1000.0f);
    REPORTER_ASSERT(
            reporter, paragraph->unresolvedGlyphs() == 0, "got %d", paragraph->unresolvedGlyphs());
    const std::vector<SkString> families = fs_font_families(*paragraph);
    REPORTER_ASSERT(reporter, !families.empty());
    for (const SkString& family : families) {
        REPORTER_ASSERT(reporter, family.contains("SF Arabic"), "got %s", family.c_str());
    }
}

// FS11: a wght variation on the system font reaches the drawn face and moves advances while
// opsz follows the size; fontWeight stays default while B2 (wght vs synthetic bold) is open
DEF_TEST(CoreTextContract_FS11_WghtAxisMovesWidth, reporter) {
    if (!has_font_families({".AppleSystemUIFont"})) {
        return;
    }
    sk_sp<FontCollection> fontCollection = system_collection();
    const SkFourByteTag wght = SkSetFourByteTag('w', 'g', 'h', 't');
    const SkFourByteTag opsz = SkSetFourByteTag('o', 'p', 's', 'z');
    for (SkScalar size : {24.0f, 40.0f}) {
        std::array<SkScalar, 2> widths = {};
        constexpr std::array<SkScalar, 2> kWeights = {300.0f, 700.0f};
        for (size_t i = 0; i < kWeights.size(); ++i) {
            skiatest::ReporterContext context(reporter,
                                              SkStringPrintf("size %g wght %g", size, kWeights[i]));
            TextStyle style = contract_text_style(".AppleSystemUIFont", size);
            const SkFontArguments::VariationPosition::Coordinate coordinate = {wght, kWeights[i]};
            SkFontArguments arguments;
            arguments.setVariationDesignPosition({&coordinate, 1});
            style.setFontArguments(arguments);
            auto paragraph = layout_text(reporter,
                                         fontCollection,
                                         contract_paragraph_style(),
                                         style,
                                         "Hamburgefonstiv",
                                         1000.0f);
            widths[i] = paragraph->getMaxIntrinsicWidth();
            const SkScalar drawnWght =
                    axis_coordinate(paragraph->getFontAtUTF16Offset(0).getTypeface(), wght)
                            .value_or(-1.0f);
            const SkScalar drawnOpsz =
                    axis_coordinate(paragraph->getFontAtUTF16Offset(0).getTypeface(), opsz)
                            .value_or(-1.0f);
            REPORTER_ASSERT(reporter,
                            SkScalarNearlyEqual(drawnWght, kWeights[i], 0.5f),
                            "got wght %g",
                            drawnWght);
            REPORTER_ASSERT(
                    reporter, SkScalarNearlyEqual(drawnOpsz, size, 0.5f), "got opsz %g", drawnOpsz);
        }
        REPORTER_ASSERT(reporter,
                        widths[0] + 5.0f < widths[1],
                        "size %g: wght 300 width %g, wght 700 width %g",
                        size,
                        widths[0],
                        widths[1]);
    }
}

// FS12: with fallback on, a cluster whose mark the requested font lacks goes whole to one
// fallback face, so base and mark never split across fonts and the base has no caret box alone
DEF_TEST(CoreTextContract_FS12_ClusterOneFallbackFont, reporter) {
    if (!has_font_families({"Courier"})) {
        return;
    }
    // Courier has "a" but not U+20DD COMBINING ENCLOSING CIRCLE
    auto paragraph = layout_text(reporter,
                                 system_collection(),
                                 contract_paragraph_style(),
                                 contract_text_style("Courier", 20.0f),
                                 "a\xE2\x83\x9D",
                                 1000.0f);
    const SkString base = fs_postscript_name_at(paragraph.get(), 0);
    const SkString mark = fs_postscript_name_at(paragraph.get(), 1);
    REPORTER_ASSERT(reporter, base == mark, "base %s, mark %s", base.c_str(), mark.c_str());
    REPORTER_ASSERT(reporter, !base.startsWith("Courier"), "got %s", base.c_str());
    REPORTER_ASSERT(
            reporter, paragraph->unresolvedGlyphs() == 0, "got %d", paragraph->unresolvedGlyphs());
    const auto baseBoxes =
            paragraph->getRectsForRange(0, 1, RectHeightStyle::kTight, RectWidthStyle::kTight);
    const auto clusterBoxes =
            paragraph->getRectsForRange(0, 2, RectHeightStyle::kTight, RectWidthStyle::kTight);
    REPORTER_ASSERT(reporter, baseBoxes.empty(), "got %zu boxes", baseBoxes.size());
    REPORTER_ASSERT(reporter, clusterBoxes.size() == 1, "got %zu boxes", clusterBoxes.size());
}

// FS13: OpenType features from TextStyle reach the system font and change what is drawn: tnum
// gives every digit one advance and smcp swaps every glyph, which a dropped tag would not
DEF_TEST(CoreTextContract_FS13_FeaturesChangeGlyphs, reporter) {
    if (!has_font_families({".AppleSystemUIFont"})) {
        return;
    }
    sk_sp<FontCollection> fontCollection = system_collection();
    const char* family = ".AppleSystemUIFont";
    // SF figures are proportional by default, otherwise tnum would change nothing
    const std::array<SkScalar, 10> proportional =
            fs_digit_widths(reporter, fontCollection, family, nullptr);
    REPORTER_ASSERT(reporter,
                    proportional[1] + 1.0f < proportional[0],
                    "\"1\" %g, \"0\" %g",
                    proportional[1],
                    proportional[0]);
    const std::array<SkScalar, 10> tabular =
            fs_digit_widths(reporter, fontCollection, family, "tnum");
    for (int i = 0; i < 10; ++i) {
        REPORTER_ASSERT(reporter,
                        tabular[i] > 0 && SkScalarNearlyEqual(tabular[i], tabular[0], 0.01f),
                        "digit %d: got %g, want %g",
                        i,
                        tabular[i],
                        tabular[0]);
    }

    auto lowercase = fs_layout_with_feature(reporter, fontCollection, family, "abc", nullptr, 0);
    auto smallCaps = fs_layout_with_feature(reporter, fontCollection, family, "abc", "smcp", 1);
    const std::vector<SkGlyphID> lowercaseGlyphs = drawn_glyph_ids(lowercase.get());
    const std::vector<SkGlyphID> smallCapsGlyphs = drawn_glyph_ids(smallCaps.get());
    REPORTER_ASSERT(reporter,
                    lowercaseGlyphs.size() == 3 && smallCapsGlyphs.size() == 3,
                    "got %zu and %zu glyphs",
                    lowercaseGlyphs.size(),
                    smallCapsGlyphs.size());
    for (size_t i = 0; i < std::min(lowercaseGlyphs.size(), smallCapsGlyphs.size()); ++i) {
        REPORTER_ASSERT(reporter,
                        lowercaseGlyphs[i] != smallCapsGlyphs[i],
                        "glyph %zu stays %d",
                        i,
                        lowercaseGlyphs[i]);
    }
    const SkScalar lowercaseWidth = lowercase->getMaxIntrinsicWidth();
    const SkScalar smallCapsWidth = smallCaps->getMaxIntrinsicWidth();
    REPORTER_ASSERT(reporter,
                    !SkScalarNearlyEqual(lowercaseWidth, smallCapsWidth, 0.1f),
                    "both %g",
                    lowercaseWidth);
}

// FS13: features reach app fonts too, whose CTFonts come from font data rather than installed
// descriptors: frac swaps the digits and slash of "1/2", and liga=0 splits Roboto's "fi"
DEF_TEST(CoreTextContract_FS13_FeaturesReachAppFont, reporter) {
    sk_sp<FontCollection> fontCollection =
            asset_collection({{"fonts/Roboto-Regular.ttf", "Roboto"}});
    if (!fontCollection) {
        return;
    }
    const std::vector<SkGlyphID> half = drawn_glyph_ids(
            fs_layout_with_feature(reporter, fontCollection, "Roboto", "1/2", nullptr, 0).get());
    const std::vector<SkGlyphID> fraction = drawn_glyph_ids(
            fs_layout_with_feature(reporter, fontCollection, "Roboto", "1/2", "frac", 1).get());
    REPORTER_ASSERT(reporter,
                    half.size() == 3 && fraction.size() == 3,
                    "got %zu and %zu glyphs",
                    half.size(),
                    fraction.size());
    for (size_t i = 0; i < std::min(half.size(), fraction.size()); ++i) {
        REPORTER_ASSERT(reporter, half[i] != fraction[i], "glyph %zu stays %d", i, half[i]);
    }

    const size_t ligated =
            drawn_glyph_ids(
                    fs_layout_with_feature(reporter, fontCollection, "Roboto", "fi", nullptr, 0)
                            .get())
                    .size();
    const size_t split =
            drawn_glyph_ids(
                    fs_layout_with_feature(reporter, fontCollection, "Roboto", "fi", "liga", 0)
                            .get())
                    .size();
    REPORTER_ASSERT(reporter, ligated == 1, "got %zu glyphs", ligated);
    REPORTER_ASSERT(reporter, split == 2, "got %zu glyphs", split);
}

// FS13 / R131: fontFeature kern=0 disables pair kerning via kCTKernAttributeName on installed and
// app fonts (including the ellipsis), while a later kern=1 restores kerning
DEF_TEST(CoreTextContract_FS13_KernZeroDisablesPairKerning, reporter) {
    sk_sp<FontCollection> appFonts = asset_collection({{"fonts/Roboto-Regular.ttf", "Roboto"}});
    if (!appFonts || !has_font_families({"Helvetica"})) {
        return;
    }
    struct FontCase {
        const char* label;
        sk_sp<FontCollection> collection;
    };
    const std::array<FontCase, 2> kCases = {{
            {"Helvetica", system_collection()},
            {"Roboto", appFonts},
    }};
    for (const FontCase& c : kCases) {
        skiatest::ReporterContext context(reporter, SkString(c.label));
        const TextStyle base = contract_text_style(c.label, 24.0f);
        TextStyle kernOff = base;
        kernOff.addFontFeature(SkString("kern"), 0);
        TextStyle kernRestored = kernOff;
        kernRestored.addFontFeature(SkString("kern"), 1);

        auto pBase = layout_text(
                reporter, c.collection, contract_paragraph_style(), base, "AVAV", 1000.0f);
        auto pOff = layout_text(
                reporter, c.collection, contract_paragraph_style(), kernOff, "AVAV", 1000.0f);
        auto pRestored = layout_text(
                reporter, c.collection, contract_paragraph_style(), kernRestored, "AVAV", 1000.0f);
        const SkScalar baseWidth = pBase->getMaxIntrinsicWidth();
        const SkScalar offWidth = pOff->getMaxIntrinsicWidth();
        const SkScalar restoredWidth = pRestored->getMaxIntrinsicWidth();
        REPORTER_ASSERT(reporter,
                        offWidth > baseWidth + 0.5f,
                        "got %g, want > %g + 0.5",
                        offWidth,
                        baseWidth);
        REPORTER_ASSERT(reporter,
                        SkScalarNearlyEqual(restoredWidth, baseWidth, 0.05f),
                        "got %g, want %g",
                        restoredWidth,
                        baseWidth);
    }

    const TextStyle helv = contract_text_style("Helvetica", 24.0f);
    TextStyle helvKernOff = helv;
    helvKernOff.addFontFeature(SkString("kern"), 0);
    auto oneW = layout_text(
            reporter, system_collection(), contract_paragraph_style(), helv, "W", 1000.0f);
    auto unkernedAvav = layout_text(
            reporter, system_collection(), contract_paragraph_style(), helvKernOff, "AVAV", 1000.f);
    // Fit one 'W' plus the "AVAV" ellipsis in both runs so only ellipsis kerning changes fWidth
    const SkScalar capWidth =
            oneW->getMaxIntrinsicWidth() + unkernedAvav->getMaxIntrinsicWidth() + 1.0f;
    ParagraphStyle ellipStyle = contract_paragraph_style();
    ellipStyle.setMaxLines(1);
    ellipStyle.setEllipsis(SkString("AVAV"));
    auto ellipBase =
            layout_text(reporter, system_collection(), ellipStyle, helv, "WWWWWWWW", capWidth);
    auto ellipOff = layout_text(
            reporter, system_collection(), ellipStyle, helvKernOff, "WWWWWWWW", capWidth);
    const std::vector<LineMetrics> linesBase = line_metrics(ellipBase.get());
    const std::vector<LineMetrics> linesOff = line_metrics(ellipOff.get());
    REPORTER_ASSERT(reporter,
                    linesBase.size() == 1 && linesOff.size() == 1,
                    "got %zu and %zu lines",
                    linesBase.size(),
                    linesOff.size());
    if (linesBase.size() == 1 && linesOff.size() == 1) {
        REPORTER_ASSERT(reporter,
                        ellipOff->getLongestLine() > ellipBase->getLongestLine() + 0.5f,
                        "got %g, want > %g + 0.5",
                        ellipOff->getLongestLine(),
                        ellipBase->getLongestLine());
    }

    if (has_font_families({".AppleSystemUIFont"})) {
        const TextStyle sysBase = contract_text_style(".AppleSystemUIFont", 11.0f);
        TextStyle sysKernOff = sysBase;
        sysKernOff.addFontFeature(SkString("kern"), 0);
        auto pSysBase = layout_text(reporter,
                                    system_collection(),
                                    contract_paragraph_style(),
                                    sysBase,
                                    "iiiiiiiiii",
                                    1000.0f);
        auto pSysOff = layout_text(reporter,
                                   system_collection(),
                                   contract_paragraph_style(),
                                   sysKernOff,
                                   "iiiiiiiiii",
                                   1000.0f);
        SkUniqueCFRef<CTFontRef> untracked =
                untracked_copy(native_font(".AppleSystemUIFont", 11.0f).get());
        const SkScalar sysBaseWidth = pSysBase->getMaxIntrinsicWidth();
        const SkScalar sysOffWidth = pSysOff->getMaxIntrinsicWidth();
        const SkScalar untrackedWidth =
                native_line_width(native_line(untracked.get(), "iiiiiiiiii", 0).get());
        REPORTER_ASSERT(reporter,
                        !SkScalarNearlyEqual(sysBaseWidth, untrackedWidth, 0.5f),
                        "system 11pt %g matches untracked %g",
                        sysBaseWidth,
                        untrackedWidth);
        REPORTER_ASSERT(reporter,
                        SkScalarNearlyEqual(sysOffWidth, sysBaseWidth, 0.01f),
                        "kern=0 got %g, base %g",
                        sysOffWidth,
                        sysBaseWidth);
    }
}

// FS14: after clearCaches, as Flutter's font loading does, an app font is found by alias or
// own family name, several files under one alias by weight, and a .ttc face draws
DEF_TEST(CoreTextContract_FS14_RegisteredFaceVisible, reporter) {
    sk_sp<SkTypeface> roboto = ToolUtils::CreateTypefaceFromResource("fonts/Roboto-Regular.ttf");
    sk_sp<SkTypeface> boldItalic =
            ToolUtils::CreateTypefaceFromResource("fonts/cond-bold-italic.ttf");
    sk_sp<SkTypeface> collectionFace = ToolUtils::CreateTypefaceFromResource("fonts/test.ttc", 0);
    if (!roboto || !boldItalic || !collectionFace) {
        return;
    }
    auto provider = sk_make_sp<TypefaceFontProvider>();
    auto fontCollection = sk_make_sp<FontCollection>();
    fontCollection->setAssetFontManager(provider);
    fontCollection->disableFontFallback();
    // A lookup before registration caches the miss, as in an app that draws before loading
    const int unresolved =
            fs_layout_face(reporter, fontCollection, "MyFont", 20.0f, SkFontStyle(), "abc")
                    ->unresolvedGlyphs();
    REPORTER_ASSERT(reporter, unresolved == 3, "got %d", unresolved);
    provider->registerTypeface(roboto, SkString("MyFont"));
    provider->registerTypeface(roboto);
    provider->registerTypeface(roboto, SkString("Multi"));
    provider->registerTypeface(boldItalic, SkString("Multi"));
    provider->registerTypeface(collectionFace, SkString("Collection"));
    fontCollection->clearCaches();

    struct RegisteredCase {
        const char* family;
        SkFontStyle fontStyle;
        const char* postscriptName;
    };
    const std::array<RegisteredCase, 4> kCases = {{
            {"MyFont", SkFontStyle(), "Roboto-Regular"},
            {"Roboto", SkFontStyle(), "Roboto-Regular"},
            {"Multi", SkFontStyle(), "Roboto-Regular"},
            {"Multi", SkFontStyle::Bold(), "RobotoCondensed-BoldItalic"},
    }};
    for (const RegisteredCase& c : kCases) {
        skiatest::ReporterContext context(reporter,
                                          SkStringPrintf("%s w%d", c.family, c.fontStyle.weight()));
        const SkString got = fs_postscript_name_at(
                fs_layout_face(reporter, fontCollection, c.family, 20.0f, c.fontStyle, "A").get(),
                0);
        REPORTER_ASSERT(reporter, got.equals(c.postscriptName), "got %s", got.c_str());
    }
    // test.ttc maps only "A" and "0"
    auto fromCollection =
            fs_layout_face(reporter, fontCollection, "Collection", 20.0f, SkFontStyle(), "A0");
    REPORTER_ASSERT(reporter,
                    fromCollection->unresolvedGlyphs() == 0,
                    "got %d",
                    fromCollection->unresolvedGlyphs());
    for (SkGlyphID glyph : drawn_glyph_ids(fromCollection.get())) {
        REPORTER_ASSERT(reporter, glyph != 0);
    }
}

// FS15: pop restores the parent span's style and a child span's family list replaces the
// parent's instead of extending it, so text after a nested span looks like text before it
DEF_TEST(CoreTextContract_FS15_PopRestoresParent, reporter) {
    sk_sp<FontCollection> fontCollection =
            asset_collection({{"fonts/ahem.ttf", "Ahem"}, {"fonts/Roboto-Regular.ttf", "Roboto"}});
    if (!fontCollection) {
        return;
    }
    auto builder = contract_builder(fontCollection, contract_paragraph_style());
    builder->pushStyle(contract_text_style("Ahem", 20.0f));
    builder->addText("a");
    builder->pushStyle(contract_text_style("Roboto", 10.0f));
    builder->addText("b");
    builder->pop();
    builder->addText("c");
    auto paragraph = layout_built(reporter, builder.get(), 1000.0f);

    struct SpanCase {
        const char* postscriptName;
        SkScalar size;
    };
    constexpr std::array<SpanCase, 3> kSpans = {
            {{"Ahem", 20.0f}, {"Roboto-Regular", 10.0f}, {"Ahem", 20.0f}}};
    for (size_t i = 0; i < kSpans.size(); ++i) {
        skiatest::ReporterContext context(reporter, SkStringPrintf("offset %zu", i));
        const SkString face = fs_postscript_name_at(paragraph.get(), i);
        const SkScalar size = paragraph->getFontAtUTF16Offset(i).getSize();
        REPORTER_ASSERT(reporter, face.equals(kSpans[i].postscriptName), "got %s", face.c_str());
        REPORTER_ASSERT(reporter, size == kSpans[i].size, "got %g, want %g", size, kSpans[i].size);
    }
}

// FS15: a child span whose families are all missing gets the first app font, same as kDefault,
// not the parent's family: the child list replaces the parent's (user decision Nov 14 2026)
DEF_TEST(CoreTextContract_FS15_MissingChildFamilyTakesFirstAppFont, reporter) {
    sk_sp<FontCollection> fontCollection =
            asset_collection({{"fonts/ahem.ttf", "Ahem"}, {"fonts/Roboto-Regular.ttf", "Roboto"}});
    if (!fontCollection) {
        return;
    }
    auto builder = contract_builder(fontCollection, contract_paragraph_style());
    add_missing_child_family_spans(builder.get());
    auto paragraph = layout_built(reporter, builder.get(), 1000.0f);
    std::vector<Paragraph*> paragraphs = {paragraph.get()};
#if !defined(SK_PARAGRAPH_DISABLE_HARFBUZZ)
    ParagraphStyle defaultStyle = contract_paragraph_style();
    defaultStyle.setTextShapingEngine(TextShapingEngine::kDefault);
    ParagraphBuilderImpl defaultBuilder(defaultStyle, fontCollection, contract_unicode());
    add_missing_child_family_spans(&defaultBuilder);
    std::unique_ptr<Paragraph> expected = defaultBuilder.Build();
    expected->layout(1000.0f);
    paragraphs.push_back(expected.get());
#endif
    for (size_t i = 0; i < paragraphs.size(); ++i) {
        skiatest::ReporterContext context(reporter, SkString(i == 0 ? "kCoreText" : "kDefault"));
        const SkString parent = fs_postscript_name_at(paragraphs[i], 0);
        const SkString child = fs_postscript_name_at(paragraphs[i], 1);
        REPORTER_ASSERT(reporter, parent.equals("Roboto-Regular"), "got %s", parent.c_str());
        REPORTER_ASSERT(reporter, child.equals("Ahem"), "got %s", child.c_str());
    }
}

// FS7: cloning a system UI face with FontArguments renames .AppleSystemUIFont* to .SF NS* /
// .New York, yet Rounded, Serif and Monospaced keep their UI design and system cascade
DEF_TEST(CoreTextContract_FS7_SystemDesignPreservedAcrossFontArguments, reporter) {
    if (!has_font_families({".AppleSystemUIFont"})) {
        return;
    }
    const sk_sp<FontCollection> fonts = system_collection();
    const SkFontArguments::VariationPosition::Coordinate opsz20 = {
            SkSetFourByteTag('o', 'p', 's', 'z'), 20.0f};
    const SkFontArguments::VariationPosition::Coordinate wght600 = {
            SkSetFourByteTag('w', 'g', 'h', 't'), 600.0f};
    SkFontArguments opszArgs;
    opszArgs.setVariationDesignPosition({&opsz20, 1});
    SkFontArguments wghtArgs;
    wghtArgs.setVariationDesignPosition({&wght600, 1});
    constexpr std::array<const char*, 4> kDesigns = {".AppleSystemUIFont",
                                                     ".AppleSystemUIFontRounded",
                                                     ".AppleSystemUIFontSerif",
                                                     ".AppleSystemUIFontMonospaced"};
    std::array<SkString, 4> clonedFaces;
    std::array<SkString, 4> wghtFaces;
    for (size_t i = 0; i < kDesigns.size(); ++i) {
        skiatest::ReporterContext context(reporter, SkString(kDesigns[i]));
        const TextStyle plainStyle = contract_text_style(kDesigns[i], 20.0f);
        TextStyle clonedStyle = plainStyle;
        clonedStyle.setFontArguments(opszArgs);
        auto plain =
                layout_text(reporter, fonts, contract_paragraph_style(), plainStyle, "Hello", 1000);
        auto cloned = layout_text(
                reporter, fonts, contract_paragraph_style(), clonedStyle, "Hello", 1000);
        clonedFaces[i] = fs_postscript_name_at(cloned.get(), 0);
        for (size_t j = 0; j < i; ++j) {
            REPORTER_ASSERT(reporter,
                            clonedFaces[i] != clonedFaces[j],
                            "%s and %s both got %s",
                            kDesigns[i],
                            kDesigns[j],
                            clonedFaces[i].c_str());
        }
        REPORTER_ASSERT(
                reporter,
                SkScalarNearlyEqual(
                        cloned->getMaxIntrinsicWidth(), plain->getMaxIntrinsicWidth(), 0.01f),
                "cloned width %g, plain width %g",
                cloned->getMaxIntrinsicWidth(),
                plain->getMaxIntrinsicWidth());

        // Rounded and Monospaced only rename to .SF NS Rounded / .SF NS Mono when wght varies
        TextStyle wghtStyle = plainStyle;
        wghtStyle.setFontArguments(wghtArgs);
        auto wghtLatin =
                layout_text(reporter, fonts, contract_paragraph_style(), wghtStyle, "Hello", 1000);
        wghtFaces[i] = fs_postscript_name_at(wghtLatin.get(), 0);
        for (size_t j = 0; j < i; ++j) {
            REPORTER_ASSERT(reporter,
                            wghtFaces[i] != wghtFaces[j],
                            "%s and %s both got %s",
                            kDesigns[i],
                            kDesigns[j],
                            wghtFaces[i].c_str());
        }
        constexpr const char* kArabic = "\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7";
        auto plainArabic = layout_text(
                reporter, fonts, contract_paragraph_style(), plainStyle, kArabic, 1000.0f);
        auto wghtArabic = layout_text(
                reporter, fonts, contract_paragraph_style(), wghtStyle, kArabic, 1000.0f);
        REPORTER_ASSERT(reporter,
                        wghtArabic->unresolvedGlyphs() == 0,
                        "got %d",
                        wghtArabic->unresolvedGlyphs());
        const std::vector<SkString> plainFamilies = fs_font_families(*plainArabic);
        const std::vector<SkString> wghtFamilies = fs_font_families(*wghtArabic);
        REPORTER_ASSERT(reporter, !plainFamilies.empty());
        REPORTER_ASSERT(reporter,
                        wghtFamilies == plainFamilies,
                        "got %s, want %s",
                        wghtFamilies.empty() ? "<empty>" : wghtFamilies[0].c_str(),
                        plainFamilies.empty() ? "<empty>" : plainFamilies[0].c_str());
    }
}

// FS9: an explicit TextStyle::setTypeface keeps its own weight and slant (including system UI
// italic, whose descriptor omits kCTFontSlantTrait) instead of restyling to TextStyle's default
DEF_TEST(CoreTextContract_FS9_ExplicitTypefaceKeepsOwnStyle, reporter) {
    if (!has_font_families({".AppleSystemUIFont"})) {
        return;
    }
    const sk_sp<FontCollection> fonts = system_collection();
    SkUniqueCFRef<CTFontRef> ctRegular(
            CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, 20.0, nullptr));
    SkUniqueCFRef<CTFontRef> ctBold(
            CTFontCreateUIFontForLanguage(kCTFontUIFontEmphasizedSystem, 20.0, nullptr));
    SkUniqueCFRef<CTFontRef> ctItalic(
            ctRegular ? CTFontCreateCopyWithSymbolicTraits(ctRegular.get(),
                                                           20.0,
                                                           nullptr,
                                                           kCTFontTraitItalic,
                                                           kCTFontTraitItalic)
                      : nullptr);
    REPORTER_ASSERT(reporter, ctRegular && ctBold && ctItalic);
    if (!ctRegular || !ctBold || !ctItalic) {
        return;
    }
    sk_sp<SkTypeface> boldTf = SkMakeTypefaceFromCTFont(ctBold.get());
    sk_sp<SkTypeface> italicTf = SkMakeTypefaceFromCTFont(ctItalic.get());
    REPORTER_ASSERT(reporter, boldTf && italicTf);
    if (!boldTf || !italicTf) {
        return;
    }

    auto regRef = fs_layout_face(
            reporter, fonts, ".AppleSystemUIFont", 20.0f, SkFontStyle(), "Hamburgefonstiv");
    auto boldRef = fs_layout_face(
            reporter, fonts, ".AppleSystemUIFont", 20.0f, SkFontStyle::Bold(), "Hamburgefonstiv");
    auto italicRef = fs_layout_face(
            reporter, fonts, ".AppleSystemUIFont", 20.0f, SkFontStyle::Italic(), "Hamburgefonstiv");

    TextStyle explicitBold;
    explicitBold.setFontSize(20.0f);
    explicitBold.setTypeface(boldTf);
    auto boldGot = layout_text(
            reporter, fonts, contract_paragraph_style(), explicitBold, "Hamburgefonstiv", 1000.0f);
    const SkString boldFace = fs_postscript_name_at(boldGot.get(), 0);
    const SkString boldWant = fs_postscript_name_at(boldRef.get(), 0);
    REPORTER_ASSERT(
            reporter, boldFace == boldWant, "got %s, want %s", boldFace.c_str(), boldWant.c_str());
    REPORTER_ASSERT(reporter,
                    boldGot->getMaxIntrinsicWidth() > regRef->getMaxIntrinsicWidth() + 2.0f,
                    "bold %g, regular %g",
                    boldGot->getMaxIntrinsicWidth(),
                    regRef->getMaxIntrinsicWidth());

    TextStyle explicitItalic;
    explicitItalic.setFontSize(20.0f);
    explicitItalic.setTypeface(italicTf);
    auto italicGot = layout_text(reporter,
                                 fonts,
                                 contract_paragraph_style(),
                                 explicitItalic,
                                 "Hamburgefonstiv",
                                 1000.0f);
    const SkString italicFace = fs_postscript_name_at(italicGot.get(), 0);
    const SkString italicWant = fs_postscript_name_at(italicRef.get(), 0);
    const SkString regFace = fs_postscript_name_at(regRef.get(), 0);
    REPORTER_ASSERT(reporter,
                    italicFace == italicWant && italicFace != regFace,
                    "got %s, want %s (regular %s)",
                    italicFace.c_str(),
                    italicWant.c_str(),
                    regFace.c_str());
}

// FS11: when .AppleSystemUIFont sits in the fallback cascade behind another primary family,
// FontArguments variation coordinates still reach the cascade system font without pinning opsz
DEF_TEST(CoreTextContract_FS11_CascadeSystemFallbackKeepsVariation, reporter) {
    sk_sp<SkTypeface> onlyA0 = ToolUtils::CreateTypefaceFromResource("fonts/test.ttc", 0);
    if (!onlyA0 || !has_font_families({".AppleSystemUIFont"})) {
        return;
    }
    auto provider = sk_make_sp<TypefaceFontProvider>();
    provider->registerTypeface(onlyA0, SkString("OnlyA0"));
    auto fonts = sk_make_sp<FontCollection>();
    fonts->setAssetFontManager(provider);
    fonts->setDefaultFontManager(ToolUtils::TestFontMgr());
    fonts->enableFontFallback();

    const SkFourByteTag wght = SkSetFourByteTag('w', 'g', 'h', 't');
    const SkFourByteTag opsz = SkSetFourByteTag('o', 'p', 's', 'z');
    constexpr std::array<SkScalar, 2> kWeights = {200.0f, 800.0f};
    std::array<SkScalar, 2> widths = {};
    for (size_t i = 0; i < kWeights.size(); ++i) {
        skiatest::ReporterContext context(reporter, SkStringPrintf("wght %g", kWeights[i]));
        TextStyle style = contract_text_style("OnlyA0", 24.0f);
        style.setFontFamilies({SkString("OnlyA0"), SkString(".AppleSystemUIFont")});
        const SkFontArguments::VariationPosition::Coordinate coordinate = {wght, kWeights[i]};
        SkFontArguments arguments;
        arguments.setVariationDesignPosition({&coordinate, 1});
        style.setFontArguments(arguments);
        auto paragraph = layout_text(
                reporter, fonts, contract_paragraph_style(), style, "Hamburgefonstiv", 1000.0f);
        widths[i] = paragraph->getMaxIntrinsicWidth();
        const SkTypeface* drawn = paragraph->getFontAtUTF16Offset(0).getTypeface();
        const SkScalar drawnWght = axis_coordinate(drawn, wght).value_or(-1.0f);
        const SkScalar drawnOpsz = axis_coordinate(drawn, opsz).value_or(-1.0f);
        REPORTER_ASSERT(reporter,
                        SkScalarNearlyEqual(drawnWght, kWeights[i], 0.5f),
                        "got wght %g, want %g",
                        drawnWght,
                        kWeights[i]);
        REPORTER_ASSERT(reporter,
                        SkScalarNearlyEqual(drawnOpsz, 24.0f, 0.5f),
                        "got opsz %g, want 24",
                        drawnOpsz);
    }
    REPORTER_ASSERT(reporter,
                    widths[0] + 5.0f < widths[1],
                    "wght 200 width %g, wght 800 width %g",
                    widths[0],
                    widths[1]);
}

// FS14: a system family miss or unregistration is not pinned permanently, so registering or
// unregistering a process font and calling FontCollection::clearCaches updates resolution
DEF_SERIAL_TEST(CoreTextContract_FS14_SystemFontRegisteredAfterMiss, reporter) {
    if (!has_font_families({".AppleSystemUIFont"})) {
        return;
    }
    const SkString fontPath = GetResourcePath("fonts/ReallyBigA.ttf");
    sk_sp<SkTypeface> face = ToolUtils::CreateTypefaceFromResource("fonts/ReallyBigA.ttf");
    REPORTER_ASSERT(reporter, !fontPath.isEmpty() && face != nullptr);
    if (fontPath.isEmpty() || !face) {
        return;
    }
    SkString family;
    SkString wantPostScript;
    face->getFamilyName(&family);
    face->getPostScriptName(&wantPostScript);
    REPORTER_ASSERT(
            reporter,
            !family.isEmpty() && !wantPostScript.isEmpty() && !has_font_families({family.c_str()}),
            "family '%s', ps '%s'",
            family.c_str(),
            wantPostScript.c_str());
    if (family.isEmpty() || wantPostScript.isEmpty() || has_font_families({family.c_str()})) {
        return;
    }

    SkUniqueCFRef<CFURLRef> fontUrl(CFURLCreateFromFileSystemRepresentation(
            kCFAllocatorDefault,
            reinterpret_cast<const UInt8*>(fontPath.c_str()),
            SkTo<CFIndex>(fontPath.size()),
            false));
    const sk_sp<FontCollection> fonts = system_collection();
    const TextStyle style = contract_text_style(family.c_str(), 20.0f);
    for (int pass = 0; pass < 2; ++pass) {
        skiatest::ReporterContext ctx(reporter, SkStringPrintf("pass %d", pass));
        const SkString beforeFace = fs_postscript_name_at(
                layout_text(reporter, fonts, contract_paragraph_style(), style, "A", 1000.0f).get(),
                0);
        REPORTER_ASSERT(reporter,
                        beforeFace != wantPostScript,
                        "already matched %s before registration",
                        beforeFace.c_str());

        const bool registered =
                fontUrl && CTFontManagerRegisterFontsForURL(
                                   fontUrl.get(), kCTFontManagerScopeProcess, nullptr);
        REPORTER_ASSERT(reporter, registered);
        if (!registered) {
            return;
        }
        struct ScopedProcessFont {
            CFURLRef fUrl;
            FontCollection* fFonts;
            ~ScopedProcessFont() {
                CTFontManagerUnregisterFontsForURL(fUrl, kCTFontManagerScopeProcess, nullptr);
                fFonts->clearCaches();
            }
        } unregister{fontUrl.get(), fonts.get()};

        fonts->clearCaches();
        const SkString afterFace = fs_postscript_name_at(
                layout_text(reporter, fonts, contract_paragraph_style(), style, "A", 1000.0f).get(),
                0);
        REPORTER_ASSERT(reporter,
                        afterFace == wantPostScript,
                        "got %s, want %s",
                        afterFace.c_str(),
                        wantPostScript.c_str());
    }
}

namespace {
// Wraps a variable typeface while hiding its CTFontRef (like Fontations) and serving raw unvaried
// SFNT bytes from onOpenStream so CoreText must read variation coordinates explicitly
class NonCtVariableTypeface final : public SkTypeface {
public:
    static sk_sp<SkTypeface> Make(sk_sp<SkTypeface> real, sk_sp<SkData> rawSfnt) {
        return sk_sp<SkTypeface>(new NonCtVariableTypeface(std::move(real), std::move(rawSfnt)));
    }

protected:
    NonCtVariableTypeface(sk_sp<SkTypeface> real, sk_sp<SkData> rawSfnt)
            : SkTypeface(real->fontStyle(), real->isFixedPitch())
            , fReal(std::move(real))
            , fRawSfnt(std::move(rawSfnt)) {}

    sk_sp<SkTypeface> onMakeClone(const SkFontArguments& args) const override {
        sk_sp<SkTypeface> cloned = fReal->makeClone(args);
        return cloned ? Make(std::move(cloned), fRawSfnt) : nullptr;
    }
    std::unique_ptr<SkStreamAsset> onOpenStream(int* ttcIndex) const override {
        *ttcIndex = 0;
        return SkMemoryStream::Make(fRawSfnt);
    }
    int onGetVariationDesignPosition(
            SkSpan<SkFontArguments::VariationPosition::Coordinate> coords) const override {
        return fReal->getVariationDesignPosition(coords);
    }
    int onGetVariationDesignParameters(SkSpan<SkFontParameters::Variation::Axis> a) const override {
        return fReal->getVariationDesignParameters(a);
    }
    void onGetFamilyName(SkString* name) const override { fReal->getFamilyName(name); }
    bool onGetPostScriptName(SkString* name) const override {
        return fReal->getPostScriptName(name);
    }
    SkTypeface::LocalizedStrings* onCreateFamilyNameIterator() const override {
        return fReal->createFamilyNameIterator();
    }
    std::unique_ptr<SkScalerContext> onCreateScalerContext(const SkScalerContextEffects&,
                                                           const SkDescriptor*) const override {
        SK_ABORT("unreachable");
    }
    void onFilterRec(SkScalerContextRec*) const override {}
    std::unique_ptr<SkAdvancedTypefaceMetrics> onGetAdvancedMetrics() const override {
        return nullptr;
    }
    void onGetFontDescriptor(SkFontDescriptor*, bool*) const override {}
    void onCharsToGlyphs(SkSpan<const SkUnichar> c, SkSpan<SkGlyphID> g) const override {
        fReal->unicharsToGlyphs(c, g);
    }
    int onCountGlyphs() const override { return fReal->countGlyphs(); }
    void getPostScriptGlyphNames(SkString*) const override {}
    void getGlyphToUnicodeMap(SkSpan<SkUnichar>) const override {}
    int onGetUPEM() const override { return fReal->getUnitsPerEm(); }
    bool onGlyphMaskNeedsCurrentColor() const override { return false; }
    int onGetTableTags(SkSpan<SkFontTableTag>) const override { return 0; }
    size_t onGetTableData(SkFontTableTag, size_t, size_t, void*) const override { return 0; }

private:
    sk_sp<SkTypeface> fReal;
    sk_sp<SkData> fRawSfnt;
};

SkUniqueCFRef<CTFontRef> make_width_system_ct_font(CGFloat ctWidth, CGFloat size) {
    SkUniqueCFRef<CTFontRef> base(
            CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, size, nullptr));
    if (!base) {
        return nullptr;
    }
    SkUniqueCFRef<CFNumberRef> widthNum(
            CFNumberCreate(kCFAllocatorDefault, kCFNumberCGFloatType, &ctWidth));
    const void* traitKey = kCTFontWidthTrait;
    const void* traitVal = widthNum.get();
    SkUniqueCFRef<CFDictionaryRef> traits(CFDictionaryCreate(kCFAllocatorDefault,
                                                             &traitKey,
                                                             &traitVal,
                                                             1,
                                                             &kCFTypeDictionaryKeyCallBacks,
                                                             &kCFTypeDictionaryValueCallBacks));
    const void* attrKey = kCTFontTraitsAttribute;
    const void* attrVal = traits.get();
    SkUniqueCFRef<CFDictionaryRef> attrs(CFDictionaryCreate(kCFAllocatorDefault,
                                                            &attrKey,
                                                            &attrVal,
                                                            1,
                                                            &kCFTypeDictionaryKeyCallBacks,
                                                            &kCFTypeDictionaryValueCallBacks));
    SkUniqueCFRef<CTFontDescriptorRef> desc(CTFontDescriptorCreateWithAttributes(attrs.get()));
    return SkUniqueCFRef<CTFontRef>(
            CTFontCreateCopyWithAttributes(base.get(), size, nullptr, desc.get()));
}
}  // namespace

// FS9: a subsetted CTFont-backed SkTypeface with no SFNT name table (null CTFontCopyFamilyName)
// shapes without crashing in system-font classification, and resolves with fallback disabled
DEF_TEST(CoreTextContract_FS9_UnnamedCtFontTypeface, reporter) {
    sk_sp<SkData> rawAhem = GetResourceAsData("fonts/ahem.ttf");
    REPORTER_ASSERT(reporter, rawAhem && rawAhem->size() > 16);
    if (!rawAhem || rawAhem->size() <= 16) {
        return;
    }
    sk_sp<SkData> nameless = SkData::MakeWithCopy(rawAhem->data(), rawAhem->size());
    uint8_t* bytes = static_cast<uint8_t*>(nameless->writable_data());
    const uint16_t numTables = (uint16_t(bytes[4]) << 8) | bytes[5];
    for (uint16_t i = 0; i < numTables && 12u + (i + 1u) * 16u <= nameless->size(); ++i) {
        uint8_t* rec = bytes + 12 + i * 16;
        if (memcmp(rec, "name", 4) == 0) {
            memcpy(rec, "nam_", 4);
        }
    }
    SkUniqueCFRef<CFDataRef> cfData(
            CFDataCreate(kCFAllocatorDefault, bytes, SkTo<CFIndex>(nameless->size())));
    SkUniqueCFRef<CGDataProviderRef> provider(CGDataProviderCreateWithCFData(cfData.get()));
    SkUniqueCFRef<CGFontRef> cgFont(CGFontCreateWithDataProvider(provider.get()));
    SkUniqueCFRef<CTFontRef> ctFont(
            cgFont ? CTFontCreateWithGraphicsFont(cgFont.get(), 14.0, nullptr, nullptr) : nullptr);
    REPORTER_ASSERT(reporter, ctFont != nullptr);
    if (!ctFont) {
        return;
    }
    SkUniqueCFRef<CFStringRef> familyName(CTFontCopyFamilyName(ctFont.get()));
    REPORTER_ASSERT(reporter, familyName == nullptr);
    sk_sp<SkTypeface> unnamedTf = SkMakeTypefaceFromCTFont(ctFont.get());
    REPORTER_ASSERT(reporter, unnamedTf != nullptr);
    if (!unnamedTf) {
        return;
    }

    TextStyle style;
    style.setFontSize(14.0f);
    style.setTypeface(unnamedTf);
    // Disabled fallback allows only the style's faces. CoreText synthesizes a PostScript name
    // (font<hex>) for the nameless face, and the span font shares it with this typeface
    auto unnamedProvider = sk_make_sp<TypefaceFontProvider>();
    unnamedProvider->registerTypeface(unnamedTf, SkString("Unnamed"));
    auto noFallback = sk_make_sp<FontCollection>();
    noFallback->setAssetFontManager(unnamedProvider);
    noFallback->disableFontFallback();
    for (const sk_sp<FontCollection>& fonts : {system_collection(), noFallback}) {
        skiatest::ReporterContext ctx(
                reporter, SkStringPrintf("fallback %d", fonts->fontFallbackEnabled()));
        auto paragraph =
                layout_text(reporter, fonts, contract_paragraph_style(), style, "abc", 1000.0f);
        REPORTER_ASSERT(reporter,
                        paragraph->unresolvedGlyphs() == 0,
                        "got %d unresolved",
                        paragraph->unresolvedGlyphs());
        REPORTER_ASSERT(reporter,
                        SkScalarNearlyEqual(paragraph->getMaxIntrinsicWidth(), 42.0f, 0.05f),
                        "got %g, want 42",
                        paragraph->getMaxIntrinsicWidth());
    }
}

// FS11: non-CTFont variable faces keep their coordinates both as explicit primary typefaces and in
// the fallback cascade, and explicit system UI faces preserve condensed/expanded width traits
DEF_TEST(CoreTextContract_FS11_NonCtFontAndSystemWidthVariations, reporter) {
    sk_sp<SkData> rawDistortable = GetResourceAsData("fonts/Distortable.ttf");
    sk_sp<SkTypeface> ctDistortable =
            ToolUtils::CreateTypefaceFromResource("fonts/Distortable.ttf");
    sk_sp<SkTypeface> onlyA0 = ToolUtils::CreateTypefaceFromResource("fonts/test.ttc", 0);
    if (!rawDistortable || !ctDistortable || !onlyA0) {
        return;
    }
    const SkFourByteTag wght = SkSetFourByteTag('w', 'g', 'h', 't');
    const SkFontArguments::VariationPosition::Coordinate coord2 = {wght, 2.0f};
    SkFontArguments args2;
    args2.setVariationDesignPosition({&coord2, 1});

    sk_sp<SkTypeface> nonCtBase = NonCtVariableTypeface::Make(ctDistortable, rawDistortable);
    sk_sp<SkTypeface> nonCtVaried = nonCtBase->makeClone(args2);
    REPORTER_ASSERT(reporter, nonCtVaried && SkTypeface_GetCTFontRef(nonCtVaried.get()) == nullptr);

    TextStyle primaryVariedStyle;
    primaryVariedStyle.setFontSize(20.0f);
    primaryVariedStyle.setTypeface(nonCtVaried);
    auto pPrimary = layout_text(reporter,
                                system_collection(),
                                contract_paragraph_style(),
                                primaryVariedStyle,
                                "ab",
                                1000.0f);
    const SkScalar primaryWght =
            axis_coordinate(pPrimary->getFontAtUTF16Offset(0).getTypeface(), wght).value_or(-1.0f);
    REPORTER_ASSERT(reporter,
                    SkScalarNearlyEqual(primaryWght, 2.0f, 0.01f),
                    "primary got wght %g, want 2",
                    primaryWght);

    auto provider = sk_make_sp<TypefaceFontProvider>();
    provider->registerTypeface(onlyA0, SkString("OnlyA0"));
    provider->registerTypeface(nonCtBase, SkString("NonCtDistortable"));
    auto cascadeFonts = sk_make_sp<FontCollection>();
    cascadeFonts->setAssetFontManager(provider);
    cascadeFonts->disableFontFallback();
    TextStyle cascadeStyle = contract_text_style("OnlyA0", 20.0f);
    cascadeStyle.setFontFamilies({SkString("OnlyA0"), SkString("NonCtDistortable")});
    cascadeStyle.setFontArguments(args2);
    auto pCascade = layout_text(
            reporter, cascadeFonts, contract_paragraph_style(), cascadeStyle, "ab", 1000.0f);
    const SkScalar cascadeWght =
            axis_coordinate(pCascade->getFontAtUTF16Offset(0).getTypeface(), wght).value_or(-1.0f);
    REPORTER_ASSERT(reporter,
                    SkScalarNearlyEqual(cascadeWght, 2.0f, 0.01f),
                    "cascade got wght %g, want 2",
                    cascadeWght);

    if (!has_font_families({".AppleSystemUIFont"})) {
        return;
    }
    SkUniqueCFRef<CTFontRef> ctCond = make_width_system_ct_font(-0.2, 20.0);
    SkUniqueCFRef<CTFontRef> ctExp = make_width_system_ct_font(0.2, 20.0);
    sk_sp<SkTypeface> condTf = ctCond ? SkMakeTypefaceFromCTFont(ctCond.get()) : nullptr;
    sk_sp<SkTypeface> expTf = ctExp ? SkMakeTypefaceFromCTFont(ctExp.get()) : nullptr;
    REPORTER_ASSERT(reporter, condTf && expTf);
    if (!condTf || !expTf) {
        return;
    }
    TextStyle condStyle;
    condStyle.setFontSize(20.0f);
    condStyle.setTypeface(condTf);
    TextStyle expStyle;
    expStyle.setFontSize(20.0f);
    expStyle.setTypeface(expTf);
    auto pCond = layout_text(reporter,
                             system_collection(),
                             contract_paragraph_style(),
                             condStyle,
                             "Hamburgefonstiv",
                             1000.0f);
    auto pExp = layout_text(reporter,
                            system_collection(),
                            contract_paragraph_style(),
                            expStyle,
                            "Hamburgefonstiv",
                            1000.0f);
    REPORTER_ASSERT(reporter,
                    pCond->getMaxIntrinsicWidth() + 2.0f < pExp->getMaxIntrinsicWidth(),
                    "condensed %g, expanded %g",
                    pCond->getMaxIntrinsicWidth(),
                    pExp->getMaxIntrinsicWidth());
}

// FS11: an explicit system UI typeface resolves under disabled fallback with FontArguments
// (default or varied wght, point-size opsz), and a pre-cloned one keeps its cloned axes
DEF_TEST(CoreTextContract_FS11_ExplicitSystemTypefaceVariation, reporter) {
    if (!has_font_families({".AppleSystemUIFont"})) {
        return;
    }
    SkUniqueCFRef<CTFontRef> ctSys(
            CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, 20.0, nullptr));
    sk_sp<SkTypeface> sysTf = ctSys ? SkMakeTypefaceFromCTFont(ctSys.get()) : nullptr;
    if (!sysTf) {
        return;
    }
    auto noFallback = sk_make_sp<FontCollection>();
    noFallback->disableFontFallback();
    const SkFourByteTag wght = SkSetFourByteTag('w', 'g', 'h', 't');
    const SkFourByteTag opsz = SkSetFourByteTag('o', 'p', 's', 'z');
    std::array<SkScalar, 2> widths = {};
    constexpr std::array<SkScalar, 2> kWeights = {400.0f, 700.0f};
    for (size_t i = 0; i < kWeights.size(); ++i) {
        skiatest::ReporterContext ctx(reporter, SkStringPrintf("wght %g", kWeights[i]));
        const Coordinate coord = {wght, kWeights[i]};
        SkFontArguments args;
        args.setVariationDesignPosition({&coord, 1});
        TextStyle style;
        style.setFontSize(40.0f);
        style.setTypeface(sysTf);
        style.setFontArguments(args);
        auto p = layout_text(reporter,
                             noFallback,
                             contract_paragraph_style(),
                             style,
                             "Hamburgefonstiv",
                             1000.0f);
        REPORTER_ASSERT(reporter, p->unresolvedGlyphs() == 0, "got %d", p->unresolvedGlyphs());
        widths[i] = p->getMaxIntrinsicWidth();
        const SkTypeface* drawn = p->getFontAtUTF16Offset(0).getTypeface();
        const SkScalar gotWght = axis_coordinate(drawn, wght).value_or(-1.0f);
        const SkScalar gotOpsz = axis_coordinate(drawn, opsz).value_or(-1.0f);
        REPORTER_ASSERT(
                reporter, SkScalarNearlyEqual(gotWght, kWeights[i], 0.5f), "got %g", gotWght);
        REPORTER_ASSERT(reporter, SkScalarNearlyEqual(gotOpsz, 40.0f, 0.5f), "got %g", gotOpsz);
    }
    REPORTER_ASSERT(reporter, widths[0] + 5.0f < widths[1], "%g vs %g", widths[0], widths[1]);

    const std::array<Coordinate, 2> cloneCoords = {{{wght, 650.0f}, {opsz, 40.0f}}};
    SkFontArguments cloneArgs;
    cloneArgs.setVariationDesignPosition({cloneCoords.data(), SkToInt(cloneCoords.size())});
    sk_sp<SkTypeface> preVaried = sysTf->makeClone(cloneArgs);
    REPORTER_ASSERT(reporter, preVaried != nullptr);
    if (!preVaried) {
        return;
    }
    TextStyle cloneStyle;
    cloneStyle.setFontSize(14.0f);
    cloneStyle.setTypeface(preVaried);
    auto pClone = layout_text(
            reporter, noFallback, contract_paragraph_style(), cloneStyle, "ab", 1000.0f);
    REPORTER_ASSERT(
            reporter, pClone->unresolvedGlyphs() == 0, "got %d", pClone->unresolvedGlyphs());
    const SkTypeface* cloneDrawn = pClone->getFontAtUTF16Offset(0).getTypeface();
    const SkScalar cloneOpsz = axis_coordinate(cloneDrawn, opsz).value_or(-1.0f);
    const SkScalar cloneWght = axis_coordinate(cloneDrawn, wght).value_or(-1.0f);
    REPORTER_ASSERT(reporter, SkScalarNearlyEqual(cloneOpsz, 40.0f, 0.5f), "got %g", cloneOpsz);
    REPORTER_ASSERT(reporter, SkScalarNearlyEqual(cloneWght, 650.0f, 0.5f), "got %g", cloneWght);

    // A pre-cloned system UI face whose opsz equals the fvar default (which CTFontCopyVariation
    // omits on macOS 10.12+) keeps that opsz at another span size, and additional FontArguments
    // layer on top of pre-cloned axes instead of resetting opsz to the span size
    const int paramCount = sysTf->getVariationDesignParameters({});
    std::vector<SkFontParameters::Variation::Axis> params(SkToSizeT(std::max(paramCount, 0)));
    sysTf->getVariationDesignParameters(params);
    REPORTER_ASSERT(reporter, !params.empty(), "got %d axes", paramCount);
    std::optional<SkScalar> defOpsz;
    for (const auto& axis : params) {
        if (axis.tag == opsz) {
            defOpsz = axis.def;
        }
    }
    REPORTER_ASSERT(reporter, defOpsz.has_value());
    if (defOpsz.has_value()) {
        const SkScalar altSize = SkScalarNearlyEqual(*defOpsz, 14.0f, 1.0f) ? 40.0f : 14.0f;
        const std::array<Coordinate, 2> defOpszCoords = {{{wght, 650.0f}, {opsz, *defOpsz}}};
        SkFontArguments defOpszArgs;
        defOpszArgs.setVariationDesignPosition(
                {defOpszCoords.data(), SkToInt(defOpszCoords.size())});
        sk_sp<SkTypeface> preDefOpsz = sysTf->makeClone(defOpszArgs);
        REPORTER_ASSERT(reporter, preDefOpsz != nullptr);
        if (preDefOpsz) {
            TextStyle defOpszStyle;
            defOpszStyle.setFontSize(altSize);
            defOpszStyle.setTypeface(preDefOpsz);
            auto pDefOpsz = layout_text(
                    reporter, noFallback, contract_paragraph_style(), defOpszStyle, "ab", 1000.0f);
            REPORTER_ASSERT(reporter,
                            pDefOpsz->unresolvedGlyphs() == 0,
                            "got %d",
                            pDefOpsz->unresolvedGlyphs());
            const SkScalar gotDefOpsz =
                    axis_coordinate(pDefOpsz->getFontAtUTF16Offset(0).getTypeface(), opsz)
                            .value_or(-1.0f);
            REPORTER_ASSERT(reporter,
                            SkScalarNearlyEqual(gotDefOpsz, *defOpsz, 0.5f),
                            "got opsz %g, want %g",
                            gotDefOpsz,
                            *defOpsz);
        }
    }

    const Coordinate wght700 = {wght, 700.0f};
    SkFontArguments extraArgs;
    extraArgs.setVariationDesignPosition({&wght700, 1});
    TextStyle layeredStyle = cloneStyle;
    layeredStyle.setFontArguments(extraArgs);
    auto pLayered = layout_text(
            reporter, noFallback, contract_paragraph_style(), layeredStyle, "ab", 1000.0f);
    REPORTER_ASSERT(
            reporter, pLayered->unresolvedGlyphs() == 0, "got %d", pLayered->unresolvedGlyphs());
    const SkTypeface* layeredDrawn = pLayered->getFontAtUTF16Offset(0).getTypeface();
    const SkScalar layeredOpsz = axis_coordinate(layeredDrawn, opsz).value_or(-1.0f);
    const SkScalar layeredWght = axis_coordinate(layeredDrawn, wght).value_or(-1.0f);
    REPORTER_ASSERT(reporter,
                    SkScalarNearlyEqual(layeredOpsz, 40.0f, 0.5f),
                    "got opsz %g, want 40",
                    layeredOpsz);
    REPORTER_ASSERT(reporter,
                    SkScalarNearlyEqual(layeredWght, 700.0f, 0.5f),
                    "got wght %g, want 700",
                    layeredWght);
}
