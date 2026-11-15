/*
 * Copyright 2026 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "include/core/SkString.h"
#include "include/core/SkTypes.h"
#include "modules/skparagraph/include/DartTypes.h"
#include "modules/skparagraph/include/TextStyle.h"
#include "modules/skparagraph/src/CoreTextAttrStringBuilder.h"
#include "modules/skparagraph/tests/coretext/CoreTextTestUtils.h"
#include "src/utils/mac/SkUniqueCFRef.h"
#include "tests/Test.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreText/CoreText.h>
#include <string.h>

#include <string>
#include <string_view>
#include <tuple>

using namespace skia::textlayout;
using namespace skia::textlayout::coretext_test;

// TextStyle font features must reach the bound CTFont as a CTFontFeatureSettingsAttribute
// array; CoreText drops settings a font lacks, so the test binds .AppleSystemUIFont, which has
// smcp and tnum
DEF_TEST(CoreTextPipeline_AttrStringBuilder_OpenTypeFeatures, reporter) {
    const char* text = "Sample Features 1234";
    size_t textLen = strlen(text);
    CoreTextAttrStringBuilder builder(std::string_view(text, textLen),
                                      TextDirection::kLtr,
                                      test_unicode());
    TextStyle style;
    // No family binds .AppleSystemUIFont, which has both features; CTFontCopyAttribute drops
    // settings the bound font does not support
    style.setFontFamilies({});
    style.addFontFeature(SkString("smcp"), 1);
    style.addFontFeature(SkString("tnum"), 1);
    builder.addStyle(TextRange(0, textLen), style);
    CoreTextRequestedFonts fonts(test_font_collection());
    auto attrStr = builder.build(&fonts);
    REPORTER_ASSERT(reporter, attrStr != nullptr);

    CTFontRef font = (CTFontRef)CFAttributedStringGetAttribute(
            attrStr.get(), 0, kCTFontAttributeName, nullptr);
    REPORTER_ASSERT(reporter, font != nullptr);

    SkUniqueCFRef<CFTypeRef> featAttr(CTFontCopyAttribute(font, kCTFontFeatureSettingsAttribute));
    REPORTER_ASSERT(reporter, featAttr != nullptr);
    CFArrayRef featArray = (CFArrayRef)featAttr.get();
    REPORTER_ASSERT(reporter, CFGetTypeID(featArray) == CFArrayGetTypeID());
    REPORTER_ASSERT(reporter,
                    CFArrayGetCount(featArray) >= 2,
                    "got %ld settings",
                    (long)CFArrayGetCount(featArray));
}

// A NaN size never equals itself, so every line of a NaN-size paragraph must still find the span
// font of the 14pt stand-in size CoreText shapes with. CoreTextRequestedFonts is read directly: no
// public API tells how many CTFonts and attribute dictionaries a paragraph built
DEF_TEST(CoreTextPipeline_AttrStringBuilder_NanSizeReusesSpanFont, reporter) {
    TextStyle style;
    style.setFontFamilies({});
    style.setFontSize(SK_FloatNaN);
    CoreTextRequestedFonts fonts(test_font_collection());
    for (std::string_view line : {std::string_view("first"), std::string_view("second")}) {
        CoreTextAttrStringBuilder builder(
                line, TextDirection::kLtr, test_unicode());
        builder.addStyle(TextRange(0, line.size()), style);
        REPORTER_ASSERT(reporter, builder.build(&fonts) != nullptr);
    }
    REPORTER_ASSERT(reporter, fonts.size() == 1, "got %d entries", fonts.size());
}

// letterSpacing skips cursive runs, so their ligatures stay (Arial's Allah ligature, Flutter
// #192891), while a Latin run of the same span still loses liga to keep its spacing visible
DEF_TEST(CoreTextPipeline_AttrStringBuilder_LetterSpacingKeepsCursiveLigatures, reporter) {
    if (!has_font_families({"Arial"})) {
        return;
    }
    // "office " then Allah (alef, lam, lam, heh): 4 UTF-16 units from index 7
    const char text[] = "office \xD8\xA7\xD9\x84\xD9\x84\xD9\x87";
    const CFIndex kArabicStart = 7;
    CoreTextAttrStringBuilder builder(std::string_view(text, sizeof(text) - 1),
                                      TextDirection::kLtr,
                                      test_unicode());
    TextStyle style;
    style.setFontFamilies({SkString("Arial")});
    style.setLetterSpacing(2.0f);
    builder.addStyle(TextRange(0, sizeof(text) - 1), style);
    CoreTextRequestedFonts fonts(test_font_collection());
    auto attr = builder.build(&fonts);
    REPORTER_ASSERT(reporter, attr != nullptr);

    SkUniqueCFRef<CTLineRef> line(CTLineCreateWithAttributedString(attr.get()));
    CFArrayRef runs = CTLineGetGlyphRuns(line.get());
    REPORTER_ASSERT(
            reporter, CFArrayGetCount(runs) > 0, "got %ld runs", (long)CFArrayGetCount(runs));
    int arabicGlyphs = 0;
    for (CFIndex i = 0; i < CFArrayGetCount(runs); ++i) {
        CTRunRef run = (CTRunRef)CFArrayGetValueAtIndex(runs, i);
        if (CTRunGetStringRange(run).location >= kArabicStart) {
            arabicGlyphs += CTRunGetGlyphCount(run);
        }
    }
    REPORTER_ASSERT(reporter, arabicGlyphs == 1, "got %d Allah glyphs", arabicGlyphs);

    CTFontRef latinFont =
            (CTFontRef)CFAttributedStringGetAttribute(attr.get(), 0, kCTFontAttributeName, nullptr);
    REPORTER_ASSERT(reporter, latinFont != nullptr);
    SkUniqueCFRef<CFArrayRef> features(
            (CFArrayRef)CTFontCopyAttribute(latinFont, kCTFontFeatureSettingsAttribute));
    bool ligaOff = false;
    for (CFIndex i = 0; features && i < CFArrayGetCount(features.get()); ++i) {
        CFDictionaryRef feature = (CFDictionaryRef)CFArrayGetValueAtIndex(features.get(), i);
        CFStringRef tag = (CFStringRef)CFDictionaryGetValue(feature, kCTFontOpenTypeFeatureTag);
        CFNumberRef value = (CFNumberRef)CFDictionaryGetValue(feature, kCTFontOpenTypeFeatureValue);
        int number = -1;
        if (tag && value && CFNumberGetValue(value, kCFNumberIntType, &number)) {
            ligaOff |= CFStringCompare(tag, CFSTR("liga"), 0) == kCFCompareEqualTo && number == 0;
        }
    }
    REPORTER_ASSERT(reporter, ligaOff);
}

// U+FEFF at the start of a segment is text, not a byte order mark: CoreTextIndexMap counts it,
// so the CFString must keep it or every attribute range lands one unit late
DEF_TEST(CoreTextPipeline_AttrStringBuilder_LeadingByteOrderMark, reporter) {
    for (const char* text : {"\xEF\xBB\xBF"
                             "ab",
                             "\xEF\xBB\xBF\xEF\xBB\xBF"
                             "a\xC2\x90"}) {
        size_t len = strlen(text);
        skiatest::ReporterContext ctx(reporter, SkStringPrintf("%zu-byte text", len));
        CoreTextAttrStringBuilder plain(std::string_view(text, len),
                                        TextDirection::kLtr,
                                        test_unicode());
        CoreTextRequestedFonts fonts(test_font_collection());
        auto plainAttr = plain.build(&fonts);
        REPORTER_ASSERT(reporter, plainAttr != nullptr);
        if (!plainAttr) {
            continue;
        }
        CFIndex u16Len = CFAttributedStringGetLength(plainAttr.get());
        REPORTER_ASSERT(reporter,
                        SkToSizeT(u16Len) == plain.indexMap().toUtf16Index(len),
                        "got %ld UTF-16 units, want %zu",
                        (long)u16Len,
                        plain.indexMap().toUtf16Index(len));
        const UniChar firstUnit = u16Len > 0
                                          ? CFStringGetCharacterAtIndex(
                                                    CFAttributedStringGetString(plainAttr.get()), 0)
                                          : 0;
        REPORTER_ASSERT(reporter,
                        u16Len > 0 && firstUnit == 0xFEFF,
                        "len %ld, first unit U+%04X, want U+FEFF",
                        static_cast<long>(u16Len),
                        firstUnit);

        CoreTextAttrStringBuilder styled(std::string_view(text, len),
                                         TextDirection::kLtr,
                                         test_unicode());
        styled.addStyle(TextRange(0, len), TextStyle());
        REPORTER_ASSERT(reporter, styled.build(&fonts) != nullptr);
    }
}

// Script runs take the UAX #24 Script of SkUnicode, so letters of any Unicode version get their
// own tag on every macOS. Marks and Common code points join the preceding script, or the next one
// at the start, so they never split a run; text of Common code points only gets Latn. An invalid
// byte, decoded as U+FFFD (Common), neither splits nor starts a script run
DEF_TEST(CoreTextPipeline_AttrStringBuilder_ScriptRunsFollowUnicodeScripts, reporter) {
    const SkFourByteTag latn = SkSetFourByteTag('L', 'a', 't', 'n');
    const SkFourByteTag arab = SkSetFourByteTag('A', 'r', 'a', 'b');
    const auto kCases = std::to_array<std::tuple<const char*, size_t, SkFourByteTag>>({
            {"A", 0, latn},
            {"\u00C0", 0, latn},
            {"\u0410", 0, SkSetFourByteTag('C', 'y', 'r', 'l')},
            {"\u0627", 0, arab},
            // Arabic Extended-B is cursive Arabic too
            {"\u0870", 0, arab},
            {"\u089F", 0, arab},
            {"\u05D0", 0, SkSetFourByteTag('H', 'e', 'b', 'r')},
            {"\u0710", 0, SkSetFourByteTag('S', 'y', 'r', 'c')},
            {"\u07C0", 0, SkSetFourByteTag('N', 'k', 'o', 'o')},
            {"\u0905", 0, SkSetFourByteTag('D', 'e', 'v', 'a')},
            {"\u0E01", 0, SkSetFourByteTag('T', 'h', 'a', 'i')},
            {"\u10D0", 0, SkSetFourByteTag('G', 'e', 'o', 'r')},
            {"\u3042", 0, SkSetFourByteTag('H', 'i', 'r', 'a')},
            {"\u30A2", 0, SkSetFourByteTag('K', 'a', 'n', 'a')},
            {"\u4E00", 0, SkSetFourByteTag('H', 'a', 'n', 'i')},
            {"\uD55C", 0, SkSetFourByteTag('H', 'a', 'n', 'g')},
            // CJK Extension I (Unicode 15.1), so script lookup does not depend on the host macOS
            // version (macOS 14 knows only Unicode 15.0)
            {"\U0002EBF0", 0, SkSetFourByteTag('H', 'a', 'n', 'i')},
            {"\u0633\u064B", 2, arab},
            {"e\u0301", 1, latn},
            {"Hi \u0633", 2, latn},
            {"Hi \u0633", 3, arab},
            {"\u0633 1", 3, arab},
            {"1 \u0633", 0, arab},
            {" 1\u00BF", 0, latn},
            {"\u0633\u0644\u0627\u0645\uE000", 8, SkSetFourByteTag('Z', 'z', 'z', 'z')},
    });
    for (const auto& [text, utf8Index, want] : kCases) {
        skiatest::ReporterContext context(reporter,
                                          SkStringPrintf("text=%s index=%zu", text, utf8Index));
        CoreTextAttrStringBuilder builder(std::string_view(text),
                                          TextDirection::kLtr,
                                          test_unicode());
        const SkFourByteTag got = builder.scriptAt(utf8Index);
        REPORTER_ASSERT(reporter, got == want, "got %08x, want %08x", got, want);
    }

    const std::string invalid =
            "\xD8\xB3"
            "\xFF"
            "\xD7\x90";
    CoreTextAttrStringBuilder builder(std::string_view(invalid.c_str(), invalid.size()),
                                      TextDirection::kLtr,
                                      test_unicode());
    REPORTER_ASSERT(reporter, builder.scriptAt(2) == arab, "got %08x", builder.scriptAt(2));
    const SkFourByteTag hebr = SkSetFourByteTag('H', 'e', 'b', 'r');
    REPORTER_ASSERT(reporter, builder.scriptAt(3) == hebr, "got %08x", builder.scriptAt(3));
    REPORTER_ASSERT(reporter,
                    builder.indexMap().toUtf16Index(3) == 2,
                    "got %zu",
                    builder.indexMap().toUtf16Index(3));
    REPORTER_ASSERT(reporter,
                    builder.indexMap().toUtf8Index(2) == 3,
                    "got %zu",
                    builder.indexMap().toUtf8Index(2));
}
