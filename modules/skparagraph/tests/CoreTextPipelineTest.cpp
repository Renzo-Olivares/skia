/*
 * Copyright 2026 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontArguments.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkFontStyle.h"
#include "include/core/SkRefCnt.h"
#include "include/core/SkScalar.h"
#include "include/core/SkStream.h"
#include "include/core/SkString.h"
#include "include/core/SkTypeface.h"
#include "include/core/SkTypes.h"
#include "modules/skparagraph/include/DartTypes.h"
#include "modules/skparagraph/include/FontCollection.h"
#include "modules/skparagraph/include/ParagraphCache.h"
#include "modules/skparagraph/include/ParagraphPainter.h"
#include "modules/skparagraph/include/ParagraphStyle.h"
#include "modules/skparagraph/include/TextStyle.h"
#include "modules/skparagraph/include/TypefaceFontProvider.h"
#include "modules/skparagraph/src/CoreTextAttrStringBuilder.h"
#include "modules/skparagraph/src/CoreTextFontRegistry.h"
#include "modules/skparagraph/src/CoreTextIndexMap.h"
#include "modules/skparagraph/src/CoreTextLineShaper.h"
#include "modules/skparagraph/src/ParagraphBuilderImpl.h"
#include "modules/skparagraph/src/ParagraphImpl.h"
#include "modules/skparagraph/src/Run.h"
#include "modules/skunicode/include/SkUnicode.h"
#include "src/core/SkAdvancedTypefaceMetrics.h"
#include "src/core/SkScalerContext.h"
#include "src/core/SkTextBlobPriv.h"
#include "src/core/SkUTF.h"
#include "src/utils/mac/SkUniqueCFRef.h"
#include "tests/Test.h"
#include "tools/Resources.h"
#include "tools/fonts/FontToolUtils.h"
#if defined(SK_UNICODE_APPLE_IMPLEMENTATION)
#include "modules/skunicode/include/SkUnicode_apple.h"
#endif
#if defined(SK_UNICODE_ICU_IMPLEMENTATION)
#include "modules/skunicode/include/SkUnicode_icu.h"
#endif

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>
#include <atomic>
#include <optional>
#include <string.h>
#include <vector>

using namespace skia::textlayout;

namespace {
// CoreText clients ship Apple's SkUnicode; ICU stands in on builds that lack it
sk_sp<SkUnicode> get_coretext_unicode() {
#if defined(SK_UNICODE_APPLE_IMPLEMENTATION)
    return SkUnicodes::Apple::Make();
#elif defined(SK_UNICODE_ICU_IMPLEMENTATION)
    return SkUnicodes::ICU::Make();
#else
    return nullptr;
#endif
}

sk_sp<FontCollection> test_font_collection() {
    auto fontCollection = sk_make_sp<FontCollection>();
    fontCollection->setDefaultFontManager(ToolUtils::TestFontMgr());
    return fontCollection;
}

// The expectations name Apple system faces; the portable font manager (--nonativeFonts) has
// none of them, so such tests skip instead of measuring substitutes
bool has_font_families(std::initializer_list<const char*> families) {
    for (const char* family : families) {
        if (!ToolUtils::TestFontMgr()->matchFamilyStyle(family, SkFontStyle())) {
            return false;
        }
    }
    return true;
}
}  // namespace

// Empty, ASCII, 2-byte and 3-byte text map both ways and round-trip ranges; a byte inside a
// scalar must resolve to that scalar's code unit, or CFRanges would split characters
DEF_TEST(CoreTextPipeline_IndexMap_AsciiAndMultibyte, reporter) {
    CoreTextIndexMap emptyMap("", 0);
    REPORTER_ASSERT(reporter, emptyMap.toUtf16Index(0) == 0);
    REPORTER_ASSERT(reporter, emptyMap.toUtf8Index(0) == 0);
    REPORTER_ASSERT(reporter, emptyMap.toUtf16Index(10) == 0);
    REPORTER_ASSERT(reporter, emptyMap.toUtf8Index(10) == 0);

    // Validate fast path for single-byte encoding and range roundtrips
    const char* ascii = "Hello, world!";
    size_t asciiLen = strlen(ascii);
    CoreTextIndexMap asciiMap(ascii, asciiLen);
    for (size_t i = 0; i <= asciiLen; ++i) {
        REPORTER_ASSERT(reporter, asciiMap.toUtf16Index(i) == i);
        REPORTER_ASSERT(reporter, asciiMap.toUtf8Index(i) == i);
    }
    CFRange cfRange = asciiMap.utf8RangeToCFRange(TextRange(0, 5));
    REPORTER_ASSERT(reporter, cfRange.location == 0 && cfRange.length == 5);
    TextRange u8Range = asciiMap.cfrangeToTextRange(cfRange);
    REPORTER_ASSERT(reporter, u8Range.start == 0 && u8Range.end == 5);

    // Cyrillic code points expand to 2 bytes in UTF-8 but 1 code unit in UTF-16
    const char* cyrillic = "Привет";
    size_t cyrBytes = strlen(cyrillic);
    REPORTER_ASSERT(reporter, cyrBytes == 12);
    CoreTextIndexMap cyrMap(cyrillic, cyrBytes);
    for (size_t u16 = 0; u16 < 6; ++u16) {
        REPORTER_ASSERT(reporter, cyrMap.toUtf8Index(u16) == u16 * 2);
        REPORTER_ASSERT(reporter, cyrMap.toUtf16Index(u16 * 2) == u16);
        REPORTER_ASSERT(reporter, cyrMap.toUtf16Index(u16 * 2 + 1) == u16);
    }
    CFRange cyrCf = cyrMap.utf8RangeToCFRange(TextRange(0, 4));
    REPORTER_ASSERT(reporter, cyrCf.location == 0 && cyrCf.length == 2);
    TextRange cyrBack = cyrMap.cfrangeToTextRange(cyrCf);
    REPORTER_ASSERT(reporter, cyrBack.start == 0 && cyrBack.end == 4);

    const char* cjk = "日本語";
    size_t cjkBytes = strlen(cjk);
    REPORTER_ASSERT(reporter, cjkBytes == 9);
    CoreTextIndexMap cjkMap(cjk, cjkBytes);
    for (size_t u16 = 0; u16 < 3; ++u16) {
        REPORTER_ASSERT(reporter, cjkMap.toUtf8Index(u16) == u16 * 3);
        REPORTER_ASSERT(reporter, cjkMap.toUtf16Index(u16 * 3) == u16);
        REPORTER_ASSERT(reporter, cjkMap.toUtf16Index(u16 * 3 + 1) == u16);
        REPORTER_ASSERT(reporter, cjkMap.toUtf16Index(u16 * 3 + 2) == u16);
    }
}

// A 4-byte scalar takes two UTF-16 units, and both map back to its first byte; 400 bytes also
// check the tables past the 256-entry stack buffer
DEF_TEST(CoreTextPipeline_IndexMap_EmojiAndSurrogatePairs, reporter) {
    // Emoji code points require 4 UTF-8 bytes and surrogate pairs in UTF-16
    const char* singleEmoji = "😀";
    size_t emojiBytes = strlen(singleEmoji);
    REPORTER_ASSERT(reporter, emojiBytes == 4);
    CoreTextIndexMap emojiMap(singleEmoji, emojiBytes);

    for (size_t b = 0; b < 4; ++b) {
        REPORTER_ASSERT(reporter, emojiMap.toUtf16Index(b) == 0);
    }
    REPORTER_ASSERT(reporter, emojiMap.toUtf16Index(4) == 2);
    REPORTER_ASSERT(reporter, emojiMap.toUtf8Index(0) == 0);
    REPORTER_ASSERT(reporter, emojiMap.toUtf8Index(1) == 0);
    REPORTER_ASSERT(reporter, emojiMap.toUtf8Index(2) == 4);

    CFRange cfEmoji = emojiMap.utf8RangeToCFRange(TextRange(0, 4));
    REPORTER_ASSERT(reporter, cfEmoji.location == 0 && cfEmoji.length == 2);
    TextRange u8Emoji = emojiMap.cfrangeToTextRange(cfEmoji);
    REPORTER_ASSERT(reporter, u8Emoji.start == 0 && u8Emoji.end == 4);

    const char* mixed = "A😀B";
    size_t mixedBytes = strlen(mixed);
    REPORTER_ASSERT(reporter, mixedBytes == 6);
    CoreTextIndexMap mixedMap(mixed, mixedBytes);

    REPORTER_ASSERT(reporter, mixedMap.toUtf16Index(0) == 0);
    REPORTER_ASSERT(reporter, mixedMap.toUtf16Index(1) == 1);
    REPORTER_ASSERT(reporter, mixedMap.toUtf16Index(5) == 3);
    REPORTER_ASSERT(reporter, mixedMap.toUtf16Index(6) == 4);

    REPORTER_ASSERT(reporter, mixedMap.toUtf8Index(0) == 0);
    REPORTER_ASSERT(reporter, mixedMap.toUtf8Index(1) == 1);
    REPORTER_ASSERT(reporter, mixedMap.toUtf8Index(2) == 1);
    REPORTER_ASSERT(reporter, mixedMap.toUtf8Index(3) == 5);
    REPORTER_ASSERT(reporter, mixedMap.toUtf8Index(4) == 6);

    CFRange cfB = mixedMap.utf8RangeToCFRange(TextRange(5, 6));
    REPORTER_ASSERT(reporter, cfB.location == 3 && cfB.length == 1);
    TextRange u8B = mixedMap.cfrangeToTextRange(cfB);
    REPORTER_ASSERT(reporter, u8B.start == 5 && u8B.end == 6);

    // Verify dynamic buffer expansion when input exceeds the 256-element stack threshold
    SkString manyEmojis;
    constexpr int kEmojiCount = 100;
    for (int i = 0; i < kEmojiCount; ++i) {
        manyEmojis.append(singleEmoji);
    }
    REPORTER_ASSERT(reporter, manyEmojis.size() == 400);
    CoreTextIndexMap heapMap(manyEmojis.c_str(), manyEmojis.size());
    for (int i = 0; i < kEmojiCount; ++i) {
        REPORTER_ASSERT(reporter, heapMap.toUtf16Index(i * 4) == static_cast<size_t>(i * 2));
        REPORTER_ASSERT(reporter, heapMap.toUtf8Index(i * 2) == static_cast<size_t>(i * 4));
    }
}

// System UI names and Cupertino aliases count as Apple system fonts while look-alikes do not;
// a wrong answer sends a family down the wrong descriptor path. Repeated lookups stay non-null
DEF_TEST(CoreTextPipeline_FontRegistry_DescriptorCacheAndFallback, reporter) {
    // Apple system font detection and descriptor caching
    CoreTextFontRegistry& registry = CoreTextFontRegistry::Instance();

    REPORTER_ASSERT(
            reporter,
            CoreTextFontRegistry::isAppleSystemFont(static_cast<const SkTypeface*>(nullptr)));
    REPORTER_ASSERT(reporter, CoreTextFontRegistry::isAppleSystemFont(".AppleSystemUIFont"));
    REPORTER_ASSERT(reporter, CoreTextFontRegistry::isAppleSystemFont(".AppleSystemUIFontRounded"));
    REPORTER_ASSERT(reporter, CoreTextFontRegistry::isAppleSystemFont(".AppleSystemUIFontSerif"));
    REPORTER_ASSERT(
            reporter, CoreTextFontRegistry::isAppleSystemFont(".AppleSystemUIFontMonospaced"));
    REPORTER_ASSERT(reporter, CoreTextFontRegistry::isAppleSystemFont("CupertinoSystemText"));
    REPORTER_ASSERT(reporter, CoreTextFontRegistry::isAppleSystemFont("CupertinoSystemDisplay"));
    REPORTER_ASSERT(reporter, CoreTextFontRegistry::isAppleSystemFont(".SF NS"));
    REPORTER_ASSERT(reporter, !CoreTextFontRegistry::isAppleSystemFont("SF Pro"));
    REPORTER_ASSERT(reporter, !CoreTextFontRegistry::isAppleSystemFont("system-ui"));
    REPORTER_ASSERT(reporter, !CoreTextFontRegistry::isAppleSystemFont("-apple-system"));
    REPORTER_ASSERT(reporter, !CoreTextFontRegistry::isAppleSystemFont("CourierNewCustom"));

    auto sysFont = registry.createFont(nullptr, SkFontStyle(), 18.0f);
    REPORTER_ASSERT(reporter, sysFont != nullptr);
    REPORTER_ASSERT(reporter, CTFontGetSize(sysFont.get()) == 18.0f);

    auto tf = ToolUtils::DefaultPortableTypeface();
    if (tf) {
        auto desc1 = registry.getDescriptor(tf.get());
        REPORTER_ASSERT(reporter, desc1 != nullptr);
        auto desc2 = registry.getDescriptor(tf.get());
        REPORTER_ASSERT(reporter, desc2 != nullptr);
    }
}

// The paragraph's TextDirection must reach CoreText as the base writing direction, or the
// BiDi levels of neutral characters would follow CoreText's natural direction instead
DEF_TEST(CoreTextPipeline_AttrStringBuilder_BaseDirectionBiDi, reporter) {
    // CoreText paragraph style must encode LTR and RTL base embedding direction
    const char* ltrText = "Hello world";
    size_t ltrLen = strlen(ltrText);
    CoreTextAttrStringBuilder ltrBuilder(
            ltrText, ltrLen, TextDirection::kLtr, test_font_collection());
    TextStyle ltrStyle;
    ltrBuilder.addStyle(TextRange(0, ltrLen), ltrStyle);
    auto ltrAttr = ltrBuilder.build();
    REPORTER_ASSERT(reporter, ltrAttr != nullptr);

    CTParagraphStyleRef ltrPStyle = (CTParagraphStyleRef)CFAttributedStringGetAttribute(
        ltrAttr.get(), 0, kCTParagraphStyleAttributeName, nullptr);
    REPORTER_ASSERT(reporter, ltrPStyle != nullptr);
    CTWritingDirection ltrDir = kCTWritingDirectionNatural;
    bool ok = CTParagraphStyleGetValueForSpecifier(
        ltrPStyle, kCTParagraphStyleSpecifierBaseWritingDirection, sizeof(ltrDir), &ltrDir);
    REPORTER_ASSERT(reporter, ok);
    REPORTER_ASSERT(reporter, ltrDir == kCTWritingDirectionLeftToRight);

    const char* rtlText = "سلام";
    size_t rtlLen = strlen(rtlText);
    CoreTextAttrStringBuilder rtlBuilder(
            rtlText, rtlLen, TextDirection::kRtl, test_font_collection());
    TextStyle rtlStyle;
    rtlBuilder.addStyle(TextRange(0, rtlLen), rtlStyle);
    auto rtlAttr = rtlBuilder.build();
    REPORTER_ASSERT(reporter, rtlAttr != nullptr);

    CTParagraphStyleRef rtlPStyle = (CTParagraphStyleRef)CFAttributedStringGetAttribute(
        rtlAttr.get(), 0, kCTParagraphStyleAttributeName, nullptr);
    REPORTER_ASSERT(reporter, rtlPStyle != nullptr);
    CTWritingDirection rtlDir = kCTWritingDirectionNatural;
    ok = CTParagraphStyleGetValueForSpecifier(
        rtlPStyle, kCTParagraphStyleSpecifierBaseWritingDirection, sizeof(rtlDir), &rtlDir);
    REPORTER_ASSERT(reporter, ok);
    REPORTER_ASSERT(reporter, rtlDir == kCTWritingDirectionRightToLeft);
}

// TextStyle font features must reach the bound CTFont as feature settings; CoreText drops
// settings a font lacks, so the test binds .AppleSystemUIFont, which has smcp and tnum
DEF_TEST(CoreTextPipeline_AttrStringBuilder_OpenTypeFeatures, reporter) {
    // OpenType features must be packed into CTFontFeatureSettingsAttribute dictionary array
    const char* text = "Sample Features 1234";
    size_t textLen = strlen(text);
    CoreTextAttrStringBuilder builder(text, textLen, TextDirection::kLtr, test_font_collection());
    TextStyle style;
    // No family binds .AppleSystemUIFont, which has both features; CTFontCopyAttribute drops
    // settings the bound font does not support
    style.setFontFamilies({});
    style.addFontFeature(SkString("smcp"), 1);
    style.addFontFeature(SkString("tnum"), 1);
    builder.addStyle(TextRange(0, textLen), style);
    auto attrStr = builder.build();
    REPORTER_ASSERT(reporter, attrStr != nullptr);

    CTFontRef font = (CTFontRef)CFAttributedStringGetAttribute(
        attrStr.get(), 0, kCTFontAttributeName, nullptr);
    REPORTER_ASSERT(reporter, font != nullptr);

    SkUniqueCFRef<CFTypeRef> featAttr(CTFontCopyAttribute(font, kCTFontFeatureSettingsAttribute));
    REPORTER_ASSERT(reporter, featAttr != nullptr);
    CFArrayRef featArray = (CFArrayRef)featAttr.get();
    REPORTER_ASSERT(reporter, CFGetTypeID(featArray) == CFArrayGetTypeID());
    REPORTER_ASSERT(reporter, CFArrayGetCount(featArray) >= 2);
}

// SkParagraph spaces the shaped clusters itself, as on the HarfBuzz path; CTTracking on the
// string would add a second spacing, place it before the glyph in RTL lines and skip RTL runs
DEF_TEST(CoreTextPipeline_AttrStringBuilder_LetterSpacingNotTracked, reporter) {
    const CFStringRef kTrackingKey = CFSTR("CTTracking");
    for (const char* text :
         {"ABCDEF", "\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D", "Hi \xD8\xB3\xD9\x84 Hi"}) {
        size_t len = strlen(text);
        CoreTextAttrStringBuilder builder(text, len, TextDirection::kLtr, test_font_collection());
        TextStyle style;
        style.setLetterSpacing(3.5f);
        builder.addStyle(TextRange(0, len), style);
        auto attr = builder.build();
        REPORTER_ASSERT(reporter, attr != nullptr);
        CFIndex u16Len = CFAttributedStringGetLength(attr.get());
        for (CFIndex i = 0; i < u16Len; ++i) {
            REPORTER_ASSERT(reporter,
                            CFAttributedStringGetAttribute(attr.get(), i, kTrackingKey, nullptr) ==
                                    nullptr,
                            "text=%s index=%ld", text, (long)i);
        }
    }
}

// Without letterSpacing the string carries no CTTracking attribute, so CoreText applies only
// the font's own trak and widths stay the ones native text gets
DEF_TEST(CoreTextPipeline_AttrStringBuilder_NoTrackingByDefault, reporter) {
    // Invariant: without explicit letter spacing, no synthetic tracking is applied
    const CFStringRef kTrackingKey = CFSTR("CTTracking");
    const char* text = "No tracking check";
    size_t textLen = strlen(text);
    CoreTextAttrStringBuilder builder(text, textLen, TextDirection::kLtr, test_font_collection());
    TextStyle style;
    style.setFontSize(16.0f);
    builder.addStyle(TextRange(0, textLen), style);
    auto attrStr = builder.build();
    REPORTER_ASSERT(reporter, attrStr != nullptr);

    CFNumberRef trackingNum = (CFNumberRef)CFAttributedStringGetAttribute(
        attrStr.get(), 0, kTrackingKey, nullptr);
    REPORTER_ASSERT(reporter, trackingNum == nullptr);
}

// U+FEFF at the start of a segment is text, not a byte order mark: CoreTextIndexMap counts it,
// so the CFString must keep it or every attribute range lands one unit late
DEF_TEST(CoreTextPipeline_AttrStringBuilder_LeadingByteOrderMark, reporter) {
    for (const char* text : {"\xEF\xBB\xBF" "ab", "\xEF\xBB\xBF\xEF\xBB\xBF" "a\xC2\x90"}) {
        size_t len = strlen(text);
        CoreTextAttrStringBuilder plain(text, len, TextDirection::kLtr, test_font_collection());
        auto plainAttr = plain.build();
        REPORTER_ASSERT(reporter, plainAttr != nullptr);
        if (!plainAttr) {
            continue;
        }
        CFIndex u16Len = CFAttributedStringGetLength(plainAttr.get());
        REPORTER_ASSERT(reporter, SkToSizeT(u16Len) == plain.indexMap().toUtf16Index(len),
                        "text=%s: CFString %ld, index map %zu", text, (long)u16Len,
                        plain.indexMap().toUtf16Index(len));
        REPORTER_ASSERT(reporter, u16Len > 0 &&
                                  CFStringGetCharacterAtIndex(
                                          CFAttributedStringGetString(plainAttr.get()), 0) ==
                                          0xFEFF);

        CoreTextAttrStringBuilder styled(text, len, TextDirection::kLtr, test_font_collection());
        styled.addStyle(TextRange(0, len), TextStyle());
        REPORTER_ASSERT(reporter, styled.build() != nullptr);
    }
}

// Block table edges and Common code points map to the right script tags, and an invalid byte,
// decoded as U+FFFD, neither splits nor starts a script run, so itemization stays per script
DEF_TEST(CoreTextPipeline_AttrStringBuilder_ResolveCodepointScript, reporter) {
    auto script = CoreTextAttrStringBuilder::ResolveCodepointScript;
    REPORTER_ASSERT(reporter, script('A') == SkSetFourByteTag('L', 'a', 't', 'n'));
    REPORTER_ASSERT(reporter, script(0x0410) == SkSetFourByteTag('C', 'y', 'r', 'l'));
    REPORTER_ASSERT(reporter, script(0x0627) == SkSetFourByteTag('A', 'r', 'a', 'b'));
    REPORTER_ASSERT(reporter, script(0x0710) == SkSetFourByteTag('S', 'y', 'r', 'c'));
    REPORTER_ASSERT(reporter, script(0x4E00) == SkSetFourByteTag('H', 'a', 'n', 'i'));
    REPORTER_ASSERT(reporter, script(' ') == 0);
    REPORTER_ASSERT(reporter, script('1') == 0);
    REPORTER_ASSERT(reporter, script(0x00BF) == 0);
    REPORTER_ASSERT(reporter, script(0x00C0) == SkSetFourByteTag('L', 'a', 't', 'n'));
    REPORTER_ASSERT(reporter, script(0x2EBEF) == SkSetFourByteTag('H', 'a', 'n', 'i'));
    REPORTER_ASSERT(reporter, script(0x2EBF0) == SkSetFourByteTag('Z', 'z', 'z', 'z'));
    REPORTER_ASSERT(reporter, script(0xFFFD) == 0);

    // An invalid byte becomes U+FFFD, which neither splits a script run nor starts one
    const char text[] = "\xD8\xB3" "\xFF" "\xD7\x90";
    CoreTextAttrStringBuilder builder(text, sizeof(text) - 1, TextDirection::kLtr,
                                      test_font_collection());
    REPORTER_ASSERT(reporter, builder.scriptAt(2) == SkSetFourByteTag('A', 'r', 'a', 'b'));
    REPORTER_ASSERT(reporter, builder.scriptAt(3) == SkSetFourByteTag('H', 'e', 'b', 'r'));
    REPORTER_ASSERT(reporter, builder.indexMap().toUtf16Index(3) == 2);
    REPORTER_ASSERT(reporter, builder.indexMap().toUtf8Index(2) == 3);
}

// A letter of a script the block table does not list starts its own non-cursive run instead of
// joining a cursive neighbour, so Bengali, Khmer or Ethiopic after Arabic still get letterSpacing
DEF_TEST(CoreTextPipeline_AttrStringBuilder_UnlistedScriptsStartNonCursiveRuns, reporter) {
    const SkFourByteTag arab = SkSetFourByteTag('A', 'r', 'a', 'b');
    auto scriptAt = [](const std::string& text, size_t utf8Index) {
        CoreTextAttrStringBuilder builder(
                text.c_str(), text.size(), TextDirection::kLtr, test_font_collection());
        return builder.scriptAt(utf8Index);
    };
    const std::string arabic = "\u0633\u0644\u0627\u0645";
    for (const char* unlisted : {"\u09AC\u09BE\u0982\u09B2\u09BE",
                                 "\u1781\u17D2\u1798\u17C2\u179A",
                                 "\u12A0\u121B\u122D\u129B"}) {
        const std::string text = arabic + unlisted;
        REPORTER_ASSERT(reporter, scriptAt(text, 0) == arab);
        const SkFourByteTag tag = scriptAt(text, arabic.size());
        REPORTER_ASSERT(reporter, tag != 0 && tag != arab, "%s: tag %08x", unlisted, tag);
    }
    // Combining marks still join their base, inside and outside the table
    REPORTER_ASSERT(reporter, scriptAt("\u0633\u064B", 2) == arab);
    REPORTER_ASSERT(reporter, scriptAt("e\u0301", 1) == SkSetFourByteTag('L', 'a', 't', 'n'));
    // Arabic Extended-B is cursive Arabic too
    REPORTER_ASSERT(reporter, CoreTextAttrStringBuilder::ResolveCodepointScript(0x0870) == arab);
    REPORTER_ASSERT(reporter, CoreTextAttrStringBuilder::ResolveCodepointScript(0x089F) == arab);
}

// An LTR line with an Arabic word must come back from CTLine as LTR, RTL, LTR runs; run
// direction is what ParagraphImpl turns into BiDi levels
DEF_TEST(CoreTextPipeline_LineShapingAndBiDiLevels, reporter) {
    // CTLine separates runs by direction with correct kCTRunStatusRightToLeft flags
    const char* bidiText = "Hello سلام World";
    size_t bidiLen = strlen(bidiText);
    CoreTextAttrStringBuilder builder(
            bidiText, bidiLen, TextDirection::kLtr, test_font_collection());
    TextStyle style;
    style.setFontSize(14.0f);
    builder.addStyle(TextRange(0, bidiLen), style);
    auto attrStr = builder.build();
    REPORTER_ASSERT(reporter, attrStr != nullptr);

    SkUniqueCFRef<CTLineRef> line(CTLineCreateWithAttributedString(attrStr.get()));
    REPORTER_ASSERT(reporter, line != nullptr);

    CFArrayRef runs = CTLineGetGlyphRuns(line.get());
    REPORTER_ASSERT(reporter, runs != nullptr);
    CFIndex runCount = CFArrayGetCount(runs);
    REPORTER_ASSERT(reporter, runCount >= 3);

    CTRunRef run0 = (CTRunRef)CFArrayGetValueAtIndex(runs, 0);
    CTRunRef run1 = (CTRunRef)CFArrayGetValueAtIndex(runs, 1);
    CTRunRef run2 = (CTRunRef)CFArrayGetValueAtIndex(runs, 2);

    REPORTER_ASSERT(reporter, (CTRunGetStatus(run0) & kCTRunStatusRightToLeft) == 0);
    REPORTER_ASSERT(reporter, (CTRunGetStatus(run1) & kCTRunStatusRightToLeft) != 0);
    REPORTER_ASSERT(reporter, (CTRunGetStatus(run2) & kCTRunStatusRightToLeft) == 0);

    REPORTER_ASSERT(reporter, CTRunGetGlyphCount(run0) > 0);
    REPORTER_ASSERT(reporter, CTRunGetGlyphCount(run1) > 0);
    REPORTER_ASSERT(reporter, CTRunGetGlyphCount(run2) > 0);
}

// Marks, joiners, BiDi controls and variation selectors count as zero width, letters and
// spaces do not, so a .notdef fallback run does not get a box for an invisible code point
DEF_TEST(CoreTextPipeline_LineShaper_ZeroWidthCodepoints, reporter) {
    REPORTER_ASSERT(reporter, CoreTextLineShaper::IsZeroWidthCodepoint(0x00AD));
    REPORTER_ASSERT(reporter, CoreTextLineShaper::IsZeroWidthCodepoint(0x0300));
    REPORTER_ASSERT(reporter, CoreTextLineShaper::IsZeroWidthCodepoint(0x034F));
    REPORTER_ASSERT(reporter, CoreTextLineShaper::IsZeroWidthCodepoint(0x036F));
    REPORTER_ASSERT(reporter, CoreTextLineShaper::IsZeroWidthCodepoint(0x061C));
    REPORTER_ASSERT(reporter, CoreTextLineShaper::IsZeroWidthCodepoint(0x1AB0));
    REPORTER_ASSERT(reporter, CoreTextLineShaper::IsZeroWidthCodepoint(0x1DC0));
    REPORTER_ASSERT(reporter, CoreTextLineShaper::IsZeroWidthCodepoint(0x200B));
    REPORTER_ASSERT(reporter, CoreTextLineShaper::IsZeroWidthCodepoint(0x200D));
    REPORTER_ASSERT(reporter, CoreTextLineShaper::IsZeroWidthCodepoint(0x202A));
    REPORTER_ASSERT(reporter, CoreTextLineShaper::IsZeroWidthCodepoint(0x2060));
    REPORTER_ASSERT(reporter, CoreTextLineShaper::IsZeroWidthCodepoint(0x2069));
    REPORTER_ASSERT(reporter, CoreTextLineShaper::IsZeroWidthCodepoint(0x20D0));
    REPORTER_ASSERT(reporter, CoreTextLineShaper::IsZeroWidthCodepoint(0xFE0F));
    REPORTER_ASSERT(reporter, CoreTextLineShaper::IsZeroWidthCodepoint(0xFE20));
    REPORTER_ASSERT(reporter, CoreTextLineShaper::IsZeroWidthCodepoint(0xFEFF));
    REPORTER_ASSERT(reporter, CoreTextLineShaper::IsZeroWidthCodepoint(0xE0100));
    REPORTER_ASSERT(reporter, !CoreTextLineShaper::IsZeroWidthCodepoint('A'));
    REPORTER_ASSERT(reporter, !CoreTextLineShaper::IsZeroWidthCodepoint(' '));
    REPORTER_ASSERT(reporter, !CoreTextLineShaper::IsZeroWidthCodepoint(0x206A));
    REPORTER_ASSERT(reporter, !CoreTextLineShaper::IsZeroWidthCodepoint(0x3164));
}

// One-style Latin text becomes one LTR run over the whole text, with glyph, position and
// cluster arrays sized as the HarfBuzz path sizes them, so TextLine can read either
DEF_TEST(CoreTextPipeline_LineShaper_SinglePassShaping, reporter) {
    ParagraphStyle paragraphStyle;
    auto fontCollection = sk_make_sp<FontCollection>();
    fontCollection->setDefaultFontManager(ToolUtils::TestFontMgr());

    ParagraphBuilderImpl builder(paragraphStyle, fontCollection, get_coretext_unicode());
    TextStyle style;
    style.setFontSize(16.0f);
    builder.pushStyle(style);
    const char* text = "Hello CoreText World";
    size_t textLen = strlen(text);
    builder.addText(text, textLen);
    builder.pop();

    auto paragraph = builder.Build();
    auto impl = static_cast<ParagraphImpl*>(paragraph.get());
    REPORTER_ASSERT(reporter, impl->computeCodeUnitProperties());

    CoreTextLineShaper shaper(impl);
    REPORTER_ASSERT(reporter, shaper.shape());
    REPORTER_ASSERT(reporter, shaper.unresolvedGlyphs() == 0);
    REPORTER_ASSERT(reporter, impl->runs().size() == 1);

    auto& run = impl->runs()[0];
    REPORTER_ASSERT(reporter, run.textRange() == TextRange(0, textLen));
    REPORTER_ASSERT(reporter, run.leftToRight());
    REPORTER_ASSERT(reporter, run.bidiLevel() == 0);
    REPORTER_ASSERT(reporter, run.script() == SkSetFourByteTag('L', 'a', 't', 'n'));
    REPORTER_ASSERT(reporter, run.size() > 0);
    REPORTER_ASSERT(reporter, run.glyphs().size() == run.size());
    REPORTER_ASSERT(reporter, run.positions().size() == run.size() + 1);
    REPORTER_ASSERT(reporter,
                    run.offsets().empty() || run.offsets().size() == run.size());
    REPORTER_ASSERT(reporter, run.clusterIndexes().size() == run.size() + 1);
    REPORTER_ASSERT(reporter, run.advance().fX > 0.0f);
    REPORTER_ASSERT(reporter, run.positions()[0].fX == 0.0f);
    REPORTER_ASSERT(reporter,
                    SkScalarNearlyEqual(run.positions()[run.size()].fX, run.advance().fX));
    REPORTER_ASSERT(reporter, run.font().getSize() == 16.0f);
    REPORTER_ASSERT(reporter, !run.isCursiveScript());
    REPORTER_ASSERT(reporter, !impl->resolvedFonts().empty());
    REPORTER_ASSERT(reporter, impl->resolvedFonts()[0].fTextStart == 0);

    impl->buildClusterTable();
    REPORTER_ASSERT(reporter, impl->clusters().size() > 1);
}

// Runs come out in logical order with the BiDi levels of the HarfBuzz path, including level 2
// for LTR inside RTL; CTLine returns runs in visual order, so the shaper has to reorder them
DEF_TEST(CoreTextPipeline_LineShaper_BiDiLogicalReordering, reporter) {
    auto fontCollection = sk_make_sp<FontCollection>();
    fontCollection->setDefaultFontManager(ToolUtils::TestFontMgr());

    // Logical run order must be strictly monotonically increasing across directional transitions
    {
        ParagraphStyle ltrStyle;
        ltrStyle.setTextDirection(TextDirection::kLtr);
        ParagraphBuilderImpl builder(ltrStyle, fontCollection, get_coretext_unicode());
        TextStyle style;
        style.setFontSize(14.0f);
        builder.pushStyle(style);
        const char* bidiText = "Hello سلام World";
        builder.addText(bidiText, strlen(bidiText));
        builder.pop();

        auto paragraph = builder.Build();
        auto impl = static_cast<ParagraphImpl*>(paragraph.get());
        REPORTER_ASSERT(reporter, impl->computeCodeUnitProperties());

        CoreTextLineShaper shaper(impl);
        REPORTER_ASSERT(reporter, shaper.shape());
        REPORTER_ASSERT(reporter, impl->runs().size() >= 3);

        for (size_t i = 1; i < impl->runs().size(); ++i) {
            REPORTER_ASSERT(
                    reporter,
                    impl->runs()[i].textRange().start >= impl->runs()[i - 1].textRange().end);
        }

        REPORTER_ASSERT(reporter, impl->runs()[0].leftToRight());
        REPORTER_ASSERT(reporter, impl->runs()[0].bidiLevel() == 0);
        REPORTER_ASSERT(reporter, !impl->runs()[1].leftToRight());
        REPORTER_ASSERT(reporter, impl->runs()[1].bidiLevel() == 1);
        REPORTER_ASSERT(reporter, impl->runs()[2].leftToRight());
        REPORTER_ASSERT(reporter, impl->runs()[2].bidiLevel() == 0);
    }

    // Embedded LTR within an RTL paragraph requires nested even bidi level 2
    {
        ParagraphStyle rtlStyle;
        rtlStyle.setTextDirection(TextDirection::kRtl);
        ParagraphBuilderImpl builder(rtlStyle, fontCollection, get_coretext_unicode());
        TextStyle style;
        style.setFontSize(14.0f);
        builder.pushStyle(style);
        const char* rtlText = "سلام Hello سلام";
        builder.addText(rtlText, strlen(rtlText));
        builder.pop();

        auto paragraph = builder.Build();
        auto impl = static_cast<ParagraphImpl*>(paragraph.get());
        REPORTER_ASSERT(reporter, impl->computeCodeUnitProperties());

        CoreTextLineShaper shaper(impl);
        REPORTER_ASSERT(reporter, shaper.shape());
        REPORTER_ASSERT(reporter, impl->runs().size() == 5);

        for (size_t i = 1; i < impl->runs().size(); ++i) {
            REPORTER_ASSERT(
                    reporter,
                    impl->runs()[i].textRange().start >= impl->runs()[i - 1].textRange().end);
        }

        REPORTER_ASSERT(reporter, !impl->runs()[0].leftToRight());
        REPORTER_ASSERT(reporter, impl->runs()[0].bidiLevel() == 1);
        REPORTER_ASSERT(reporter, !impl->runs()[1].leftToRight());
        REPORTER_ASSERT(reporter, impl->runs()[1].bidiLevel() == 1);
        REPORTER_ASSERT(reporter, impl->runs()[2].leftToRight());
        REPORTER_ASSERT(reporter, impl->runs()[2].bidiLevel() == 2);
        REPORTER_ASSERT(reporter, !impl->runs()[3].leftToRight());
        REPORTER_ASSERT(reporter, impl->runs()[3].bidiLevel() == 1);
        REPORTER_ASSERT(reporter, !impl->runs()[4].leftToRight());
        REPORTER_ASSERT(reporter, impl->runs()[4].bidiLevel() == 1);
    }
}

// Each script block gets its tag, and Common code points take the surrounding script, so
// runs split where SkShaper's script iterator splits them
DEF_TEST(CoreTextPipeline_LineShaper_ScriptDetection, reporter) {
    auto scriptAt = [](const char* text, size_t utf8Index) {
        CoreTextAttrStringBuilder builder(
                text, strlen(text), TextDirection::kLtr, test_font_collection());
        return builder.scriptAt(utf8Index);
    };
    REPORTER_ASSERT(reporter, scriptAt("Hello", 0) == SkSetFourByteTag('L', 'a', 't', 'n'));
    REPORTER_ASSERT(reporter, scriptAt("Привет", 0) == SkSetFourByteTag('C', 'y', 'r', 'l'));
    REPORTER_ASSERT(reporter, scriptAt("سلام", 0) == SkSetFourByteTag('A', 'r', 'a', 'b'));
    REPORTER_ASSERT(reporter, scriptAt("שלום", 0) == SkSetFourByteTag('H', 'e', 'b', 'r'));
    REPORTER_ASSERT(reporter, scriptAt("\xD7\x90", 0) == SkSetFourByteTag('H', 'e', 'b', 'r'));
    REPORTER_ASSERT(reporter, scriptAt("\xDC\x90", 0) == SkSetFourByteTag('S', 'y', 'r', 'c'));
    REPORTER_ASSERT(reporter, scriptAt("\xDF\x80", 0) == SkSetFourByteTag('N', 'k', 'o', 'o'));
    REPORTER_ASSERT(reporter,
                    scriptAt("\xE0\xA4\x85", 0) == SkSetFourByteTag('D', 'e', 'v', 'a'));
    REPORTER_ASSERT(reporter,
                    scriptAt("\xE0\xB8\x81", 0) == SkSetFourByteTag('T', 'h', 'a', 'i'));
    REPORTER_ASSERT(reporter,
                    scriptAt("\xE6\xBC\xA2", 0) == SkSetFourByteTag('H', 'a', 'n', 'i'));
    REPORTER_ASSERT(reporter,
                    scriptAt("\xE3\x81\x82", 0) == SkSetFourByteTag('H', 'i', 'r', 'a'));
    REPORTER_ASSERT(reporter,
                    scriptAt("\xE3\x82\xA2", 0) == SkSetFourByteTag('K', 'a', 'n', 'a'));
    REPORTER_ASSERT(reporter,
                    scriptAt("\xED\x95\x9C", 0) == SkSetFourByteTag('H', 'a', 'n', 'g'));

    // Common code points join the preceding script, or the following one when leading
    REPORTER_ASSERT(reporter, scriptAt("Hi \xD8\xB3", 2) == SkSetFourByteTag('L', 'a', 't', 'n'));
    REPORTER_ASSERT(reporter, scriptAt("Hi \xD8\xB3", 3) == SkSetFourByteTag('A', 'r', 'a', 'b'));
    REPORTER_ASSERT(reporter, scriptAt("\xD8\xB3 1", 3) == SkSetFourByteTag('A', 'r', 'a', 'b'));
    REPORTER_ASSERT(reporter, scriptAt("1 \xD8\xB3", 0) == SkSetFourByteTag('A', 'r', 'a', 'b'));
    REPORTER_ASSERT(reporter, scriptAt("123", 0) == SkSetFourByteTag('L', 'a', 't', 'n'));

    auto fontCollection = sk_make_sp<FontCollection>();
    fontCollection->setDefaultFontManager(ToolUtils::TestFontMgr());

    // Cursive script detection drives run letter-spacing eligibility
    ParagraphStyle style;
    ParagraphBuilderImpl builder(style, fontCollection, get_coretext_unicode());
    TextStyle textStyle;
    textStyle.setFontSize(14.0f);
    builder.pushStyle(textStyle);
    builder.addText("سلام");
    builder.pop();

    auto paragraph = builder.Build();
    auto impl = static_cast<ParagraphImpl*>(paragraph.get());
    impl->computeCodeUnitProperties();

    CoreTextLineShaper shaper(impl);
    REPORTER_ASSERT(reporter, shaper.shape());
    REPORTER_ASSERT(reporter, impl->runs().size() == 1);
    REPORTER_ASSERT(reporter, impl->runs()[0].isCursiveScript());
    REPORTER_ASSERT(reporter, impl->runs()[0].script() == SkSetFourByteTag('A', 'r', 'a', 'b'));
}

// Cluster indexes of 2-byte Cyrillic stay on character starts and tile the run without gaps;
// an index inside a character would break hit-testing and line breaking
DEF_TEST(CoreTextPipeline_LineShaper_ClusterAtomicity, reporter) {
    auto fontCollection = sk_make_sp<FontCollection>();
    fontCollection->setDefaultFontManager(ToolUtils::TestFontMgr());

    // Multi-byte UTF-8 Cyrillic cluster boundaries must stay byte-aligned
    {
        ParagraphStyle style;
        ParagraphBuilderImpl builder(style, fontCollection, get_coretext_unicode());
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
        REPORTER_ASSERT(reporter, impl->runs().size() == 1);

        auto& run = impl->runs()[0];
        REPORTER_ASSERT(reporter, run.textRange().width() == cyrLen);
        REPORTER_ASSERT(reporter, run.clusterIndexes()[0] == 0);
        REPORTER_ASSERT(reporter, run.clusterIndexes()[run.size()] == cyrLen);

        for (size_t k = 0; k <= run.size(); ++k) {
            REPORTER_ASSERT(reporter, run.clusterIndexes()[k] % 2 == 0);
            REPORTER_ASSERT(reporter, run.clusterIndexes()[k] <= cyrLen);
        }

        size_t lastCharEnd = run.textRange().start;
        size_t clusterCount = 0;
        run.iterateThroughClustersInTextOrder([&](size_t glyphStart,
                                                  size_t glyphEnd,
                                                  size_t charStart,
                                                  size_t charEnd,
                                                  SkScalar /*width*/) {
            REPORTER_ASSERT(reporter, charStart == lastCharEnd);
            REPORTER_ASSERT(reporter, charEnd > charStart);
            REPORTER_ASSERT(reporter, glyphEnd > glyphStart);
            lastCharEnd = charEnd;
            ++clusterCount;
        });
        REPORTER_ASSERT(reporter, lastCharEnd == run.textRange().end);
        REPORTER_ASSERT(reporter, clusterCount == 7);

        impl->buildClusterTable();
        REPORTER_ASSERT(reporter, impl->clusters().size() == 8);
        for (size_t i = 0; i < cyrLen; ++i) {
            REPORTER_ASSERT(reporter, impl->clusterIndex(i) == i / 2);
        }
    }

    // RTL cluster traversal in logical text order
    {
        ParagraphStyle rtlStyle;
        rtlStyle.setTextDirection(TextDirection::kRtl);
        ParagraphBuilderImpl builder(rtlStyle, fontCollection, get_coretext_unicode());
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
        REPORTER_ASSERT(reporter, impl->runs().size() == 1);

        auto& run = impl->runs()[0];
        REPORTER_ASSERT(reporter, !run.leftToRight());

        size_t lastCharEnd = run.textRange().start;
        size_t clusterCount = 0;
        run.iterateThroughClustersInTextOrder([&](size_t /*glyphStart*/,
                                                  size_t /*glyphEnd*/,
                                                  size_t charStart,
                                                  size_t charEnd,
                                                  SkScalar /*width*/) {
            REPORTER_ASSERT(reporter, charStart == lastCharEnd);
            REPORTER_ASSERT(reporter, charEnd > charStart);
            lastCharEnd = charEnd;
            ++clusterCount;
        });
        REPORTER_ASSERT(reporter, lastCharEnd == run.textRange().end);
        REPORTER_ASSERT(reporter, clusterCount > 0);
    }
}

// A placeholder becomes its own run of its exact width between the text runs, and the next
// run starts right after it, as ParagraphImpl lays out placeholders on the HarfBuzz path
DEF_TEST(CoreTextPipeline_LineShaper_WithPlaceholders, reporter) {
    ParagraphStyle paragraphStyle;
    auto fontCollection = sk_make_sp<FontCollection>();
    fontCollection->setDefaultFontManager(ToolUtils::TestFontMgr());

    ParagraphBuilderImpl builder(paragraphStyle, fontCollection, get_coretext_unicode());
    TextStyle style;
    style.setFontSize(14.0f);
    builder.pushStyle(style);
    builder.addText("Prefix ");
    PlaceholderStyle placeholderStyle(
            30.0f, 20.0f, PlaceholderAlignment::kBaseline, TextBaseline::kAlphabetic, 0.0f);
    builder.addPlaceholder(placeholderStyle);
    builder.addText(" Suffix");
    builder.pop();

    auto paragraph = builder.Build();
    auto impl = static_cast<ParagraphImpl*>(paragraph.get());
    REPORTER_ASSERT(reporter, impl->computeCodeUnitProperties());

    CoreTextLineShaper shaper(impl);
    REPORTER_ASSERT(reporter, shaper.shape());
    REPORTER_ASSERT(reporter, shaper.unresolvedGlyphs() == 0);

    REPORTER_ASSERT(reporter, impl->runs().size() == 3);

    auto& run0 = impl->runs()[0];
    auto& run1 = impl->runs()[1];
    auto& run2 = impl->runs()[2];

    REPORTER_ASSERT(reporter, !run0.isPlaceholder());
    REPORTER_ASSERT(reporter, run0.textRange() == TextRange(0, 7));
    REPORTER_ASSERT(reporter, run0.advance().fX > 0.0f);

    REPORTER_ASSERT(reporter, run1.isPlaceholder());
    REPORTER_ASSERT(reporter, run1.advance().fX == 30.0f);
    REPORTER_ASSERT(reporter, run1.positions()[0].fX == run0.advance().fX);
    REPORTER_ASSERT(reporter, run1.positions()[1].fX == run0.advance().fX + 30.0f);

    REPORTER_ASSERT(reporter, !run2.isPlaceholder());
    REPORTER_ASSERT(reporter, run2.advance().fX > 0.0f);
    REPORTER_ASSERT(reporter, run2.positions()[0].fX == run0.advance().fX + 30.0f);

    impl->buildClusterTable();
    REPORTER_ASSERT(reporter, impl->clusters().size() > 3);
}

// CupertinoSystemText and CupertinoSystemDisplay must resolve in matchSystemFamily so that a
// non-empty fontFamilyFallback list does not skip them and land on the first fallback family
DEF_TEST(CoreTextPipeline_AppleSystemFonts_CupertinoAliases, reporter) {
    if (!has_font_families({"Helvetica"})) {
        return;
    }
    auto fontCollection = sk_make_sp<FontCollection>();
    fontCollection->setDefaultFontManager(ToolUtils::TestFontMgr());
    fontCollection->enableFontFallback();

    CoreTextFontRegistry& registry = CoreTextFontRegistry::Instance();
    for (const char* alias : {"CupertinoSystemText", "CupertinoSystemDisplay"}) {
        std::vector<SkString> families = {SkString(alias), SkString("Helvetica")};
        auto faces = registry.findTypefaces(
                *fontCollection, families, SkFontStyle::Normal(), std::nullopt);
        REPORTER_ASSERT(reporter, faces.size() == 2, "alias=%s size=%zu", alias, faces.size());
        if (faces.size() == 2) {
            SkString firstPs;
            faces[0]->getPostScriptName(&firstPs);
            REPORTER_ASSERT(reporter,
                            firstPs.startsWith(".AppleSystemUIFont") || firstPs.startsWith(".SF"),
                            "alias=%s resolved to %s",
                            alias,
                            firstPs.c_str());
            SkString secondPs;
            faces[1]->getPostScriptName(&secondPs);
            REPORTER_ASSERT(reporter, secondPs.equals("Helvetica"));
        }
    }
}

// .AppleSystemUIFont{Rounded,Serif,Monospaced} select their UI design variants across both
// typeface matching and size-dependent CTFont instantiation (including bold weight traits)
DEF_TEST(CoreTextPipeline_AppleSystemFonts_DesignSuffixes, reporter) {
    if (!has_font_families({"Helvetica"})) {
        return;
    }
    auto fontCollection = sk_make_sp<FontCollection>();
    fontCollection->setDefaultFontManager(ToolUtils::TestFontMgr());
    fontCollection->enableFontFallback();

    CoreTextFontRegistry& registry = CoreTextFontRegistry::Instance();
    const struct {
        const char* fFamily;
        const char* fExpectedRegularPs;
        const char* fExpectedBoldPs;
    } cases[] = {
            {".AppleSystemUIFontRounded", "Rounded-Regular", "Rounded-Bold"},
            {".AppleSystemUIFontSerif", "NewYork-Regular", "NewYork-Bold"},
            {".AppleSystemUIFontMonospaced", "Monospaced-Regular", "Monospaced-Bold"},
    };

    for (const auto& c : cases) {
        std::vector<SkString> families = {SkString(c.fFamily), SkString("Helvetica")};
        auto regFaces = registry.findTypefaces(
                *fontCollection, families, SkFontStyle::Normal(), std::nullopt);
        auto boldFaces = registry.findTypefaces(
                *fontCollection, families, SkFontStyle::Bold(), std::nullopt);
        REPORTER_ASSERT(reporter, regFaces.size() == 2 && boldFaces.size() == 2);
        if (regFaces.size() != 2 || boldFaces.size() != 2) {
            continue;
        }
        SkString regPs;
        regFaces[0]->getPostScriptName(&regPs);
        REPORTER_ASSERT(reporter,
                        regPs.contains(c.fExpectedRegularPs),
                        "family=%s got=%s",
                        c.fFamily,
                        regPs.c_str());

        SkUniqueCFRef<CTFontRef> ctBold =
                registry.createFont(boldFaces[0].get(), SkFontStyle::Bold(), 24.0f);
        REPORTER_ASSERT(reporter, ctBold != nullptr);
        if (ctBold) {
            SkUniqueCFRef<CFStringRef> cfPs(CTFontCopyPostScriptName(ctBold.get()));
            char buf[256] = {};
            if (cfPs) {
                CFStringGetCString(cfPs.get(), buf, sizeof(buf), kCFStringEncodingUTF8);
            }
            REPORTER_ASSERT(reporter,
                            strstr(buf, c.fExpectedBoldPs) != nullptr,
                            "family=%s ctBold=%s",
                            c.fFamily,
                            buf);
        }
    }
}

// With no requested or default family found, a user face stands in before the system font, as
// in FontCollection::getFontManagerOrder, whether font fallback is enabled or not
DEF_TEST(CoreTextPipeline_MissingFamily_UserFaceStandsIn, reporter) {
    sk_sp<SkTypeface> roboto = ToolUtils::CreateTypefaceFromResource("fonts/Roboto-Regular.ttf");
    REPORTER_ASSERT(reporter, roboto);
    auto provider = sk_make_sp<TypefaceFontProvider>();
    provider->registerTypeface(roboto, SkString("FaceA"));
    auto fc = sk_make_sp<FontCollection>();
    fc->setDefaultFontManager(ToolUtils::TestFontMgr(), "NoSuchDefaultFamily");
    fc->setDynamicFontManager(provider);
    std::vector<SkString> families = {SkString("NoSuchFamily")};

    for (bool fallback : {true, false}) {
        if (fallback) {
            fc->enableFontFallback();
        } else {
            fc->disableFontFallback();
        }
        auto expected = fc->findTypefaces(families, SkFontStyle());
        auto actual = CoreTextFontRegistry::Instance().findTypefaces(
                *fc, families, SkFontStyle(), std::nullopt);
        REPORTER_ASSERT(reporter,
                        expected.size() == 1 && expected[0]->uniqueID() == roboto->uniqueID());
        REPORTER_ASSERT(reporter,
                        actual.size() == 1 && actual[0]->uniqueID() == roboto->uniqueID(),
                        "fallback %d",
                        fallback);
    }
}

namespace {
// Font data copies handed out by StubCloneTypeface::onOpenStream and not freed yet
std::atomic<int> gLiveStubFontDataCopies{0};

class StubCloneTypeface final : public SkTypeface {
public:
    // A face named family whose clone fails when failClone; with fontData, each openStream
    // returns a fresh copy that only the stream owns, counted in gLiveStubFontDataCopies
    static sk_sp<SkTypeface> Make(const char* family, bool failClone,
                                  sk_sp<SkData> fontData = nullptr) {
        return sk_sp<SkTypeface>(new StubCloneTypeface(family, failClone, std::move(fontData)));
    }

protected:
    StubCloneTypeface(const char* family, bool failClone, sk_sp<SkData> fontData)
            : SkTypeface(SkFontStyle(), false)
            , fFamily(family)
            , fFailClone(failClone)
            , fFontData(std::move(fontData)) {}

    sk_sp<SkTypeface> onMakeClone(const SkFontArguments&) const override {
        return fFailClone ? nullptr : sk_ref_sp(this);
    }
    // A copy the stream alone owns, so a reader that outlives the stream sees it freed
    std::unique_ptr<SkStreamAsset> onOpenStream(int* ttcIndex) const override {
        if (!fFontData) {
            return nullptr;
        }
        *ttcIndex = 0;
        void* copy = sk_malloc_throw(fFontData->size());
        memcpy(copy, fFontData->data(), fFontData->size());
        ++gLiveStubFontDataCopies;
        return SkMemoryStream::Make(SkData::MakeWithProc(
                copy, fFontData->size(), [](const void* ptr, void*) {
                    --gLiveStubFontDataCopies;
                    sk_free(const_cast<void*>(ptr));
                }, nullptr));
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
    void onCharsToGlyphs(SkSpan<const SkUnichar>, SkSpan<SkGlyphID> glyphs) const override {
        sk_bzero(glyphs.data(), glyphs.size_bytes());
    }
    int onCountGlyphs() const override { return 0; }
    void getPostScriptGlyphNames(SkString*) const override {}
    void getGlyphToUnicodeMap(SkSpan<SkUnichar>) const override {}
    int onGetUPEM() const override { return 0; }
    class EmptyLocalizedStrings final : public SkTypeface::LocalizedStrings {
    public:
        bool next(SkTypeface::LocalizedString*) override { return false; }
    };
    void onGetFamilyName(SkString* familyName) const override { *familyName = fFamily; }
    bool onGetPostScriptName(SkString*) const override { return false; }
    SkTypeface::LocalizedStrings* onCreateFamilyNameIterator() const override {
        return new EmptyLocalizedStrings;
    }
    bool onGlyphMaskNeedsCurrentColor() const override { return false; }
    int onGetVariationDesignPosition(
            SkSpan<SkFontArguments::VariationPosition::Coordinate>) const override {
        return 0;
    }
    int onGetVariationDesignParameters(SkSpan<SkFontParameters::Variation::Axis>) const override {
        return 0;
    }
    int onGetTableTags(SkSpan<SkFontTableTag>) const override { return 0; }
    size_t onGetTableData(SkFontTableTag, size_t, size_t, void*) const override { return 0; }

private:
    SkString fFamily;
    bool fFailClone;
    sk_sp<SkData> fFontData;
};
}  // namespace

// A user face whose variation clone fails is dropped, as FontCollection drops it, so the
// next family stands in instead of a null entry reaching the attributed string
DEF_TEST(CoreTextPipeline_FontRegistry_NullCloneTypefaceSkipped, reporter) {
    sk_sp<SkTypeface> valid = StubCloneTypeface::Make("ValidClone", false);
    auto provider = sk_make_sp<TypefaceFontProvider>();
    provider->registerTypeface(StubCloneTypeface::Make("NullClone", true), SkString("NullClone"));
    provider->registerTypeface(valid, SkString("ValidClone"));

    auto fc = sk_make_sp<FontCollection>();
    fc->setDefaultFontManager(ToolUtils::TestFontMgr());
    fc->setDynamicFontManager(provider);
    fc->disableFontFallback();

    SkFontArguments::VariationPosition::Coordinate coord = {
            SkSetFourByteTag('w', 'g', 'h', 't'), 500.0f};
    SkFontArguments skArgs;
    skArgs.setVariationDesignPosition({&coord, 1});
    FontArguments fontArgs(skArgs);
    std::vector<SkString> families = {SkString("NullClone"), SkString("ValidClone")};

    auto expected = fc->findTypefaces(families, SkFontStyle(), fontArgs);
    auto actual = CoreTextFontRegistry::Instance().findTypefaces(
            *fc, families, SkFontStyle(), fontArgs);
    REPORTER_ASSERT(reporter, expected.size() == 1 && expected[0] != nullptr);
    REPORTER_ASSERT(reporter,
                    actual.size() == 1 && actual[0] != nullptr &&
                            actual[0]->uniqueID() == valid->uniqueID(),
                    "actual size=%zu front=%p",
                    actual.size(),
                    actual.empty() ? nullptr : actual[0].get());
}

// A descriptor parsed from a non-CoreText face's font data stays in the registry cache after
// the face and its stream are gone, so it must own those bytes, or CTFonts read freed memory
DEF_TEST(CoreTextPipeline_FontRegistry_CachedDescriptorOwnsFontData, reporter) {
    sk_sp<SkData> roboto = GetResourceAsData("fonts/Roboto-Regular.ttf");
    if (!roboto) {
        return;
    }
    sk_sp<SkTypeface> face = StubCloneTypeface::Make("StreamOnlyFace", false, roboto);
    const int copiesBefore = gLiveStubFontDataCopies;
    SkUniqueCFRef<CTFontDescriptorRef> desc =
            CoreTextFontRegistry::Instance().getDescriptor(face.get());
    face.reset();
    REPORTER_ASSERT(reporter, desc != nullptr);
    REPORTER_ASSERT(reporter,
                    gLiveStubFontDataCopies == copiesBefore + 1,
                    "font data copies alive: %d, expected %d",
                    gLiveStubFontDataCopies.load(),
                    copiesBefore + 1);
    if (!desc) {
        return;
    }
    SkUniqueCFRef<CTFontRef> font(CTFontCreateWithFontDescriptor(desc.get(), 12, nullptr));
    REPORTER_ASSERT(reporter, font && CTFontGetGlyphCount(font.get()) > 0);
}
