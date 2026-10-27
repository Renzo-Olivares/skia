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
// Each engine gets the SkUnicode its clients ship with: Apple's for CoreText, ICU for HarfBuzz.
// A build lacking it yields null, which ParagraphBuilder::Build rejects instead of silently
// laying out with another implementation
sk_sp<SkUnicode> get_unicode(const ParagraphStyle& style) {
#if defined(SK_UNICODE_APPLE_IMPLEMENTATION)
    if (style.getTextShapingEngine() == TextShapingEngine::kCoreText) {
        return SkUnicodes::Apple::Make();
    }
#endif
#if defined(SK_UNICODE_ICU_IMPLEMENTATION)
    return SkUnicodes::ICU::Make();
#else
    return nullptr;
#endif
}

// Run::offsets() is empty when every glyph offset is zero, and has no entry for the end
// sentinel, whose offset is zero
SkPoint glyph_offset(const Run& run, size_t k) {
    return k < run.offsets().size() ? run.offsets()[k] : SkPoint{0, 0};
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

#if defined(SK_UNICODE_APPLE_IMPLEMENTATION)
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
#endif  // defined(SK_UNICODE_APPLE_IMPLEMENTATION)

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
    REPORTER_ASSERT(reporter, CoreTextLineShaper::IsZeroWidthCodepoint(0x0001));
    REPORTER_ASSERT(reporter, CoreTextLineShaper::IsZeroWidthCodepoint(0x001B));
    REPORTER_ASSERT(reporter, CoreTextLineShaper::IsZeroWidthCodepoint(0x007F));
    REPORTER_ASSERT(reporter, CoreTextLineShaper::IsZeroWidthCodepoint(0x0084));
    REPORTER_ASSERT(reporter, CoreTextLineShaper::IsZeroWidthCodepoint(0x0086));
    REPORTER_ASSERT(reporter, CoreTextLineShaper::IsZeroWidthCodepoint(0x009F));
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
    REPORTER_ASSERT(reporter, !CoreTextLineShaper::IsZeroWidthCodepoint('\t'));
    REPORTER_ASSERT(reporter, !CoreTextLineShaper::IsZeroWidthCodepoint('\n'));
    REPORTER_ASSERT(reporter, !CoreTextLineShaper::IsZeroWidthCodepoint(0x001C));
    REPORTER_ASSERT(reporter, !CoreTextLineShaper::IsZeroWidthCodepoint(0x001F));
    REPORTER_ASSERT(reporter, !CoreTextLineShaper::IsZeroWidthCodepoint(0x0085));
    REPORTER_ASSERT(reporter, !CoreTextLineShaper::IsZeroWidthCodepoint(0x206A));
    REPORTER_ASSERT(reporter, !CoreTextLineShaper::IsZeroWidthCodepoint(0x3164));
}

// One-style Latin text becomes one LTR run over the whole text, with glyph, position and
// cluster arrays sized as the HarfBuzz path sizes them, so TextLine can read either
DEF_TEST(CoreTextPipeline_LineShaper_SinglePassShaping, reporter) {
    ParagraphStyle paragraphStyle;
    auto fontCollection = sk_make_sp<FontCollection>();
    fontCollection->setDefaultFontManager(ToolUtils::TestFontMgr());

    paragraphStyle.setTextShapingEngine(TextShapingEngine::kCoreText);
    ParagraphBuilderImpl builder(paragraphStyle, fontCollection, get_unicode(paragraphStyle));
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
        ltrStyle.setTextShapingEngine(TextShapingEngine::kCoreText);
        ParagraphBuilderImpl builder(ltrStyle, fontCollection, get_unicode(ltrStyle));
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
        rtlStyle.setTextShapingEngine(TextShapingEngine::kCoreText);
        ParagraphBuilderImpl builder(rtlStyle, fontCollection, get_unicode(rtlStyle));
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
    style.setTextShapingEngine(TextShapingEngine::kCoreText);
    ParagraphBuilderImpl builder(style, fontCollection, get_unicode(style));
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

    paragraphStyle.setTextShapingEngine(TextShapingEngine::kCoreText);
    ParagraphBuilderImpl builder(paragraphStyle, fontCollection, get_unicode(paragraphStyle));
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

// letterSpacing adds its value once per code point and wordSpacing once per space, alone and
// together, on CoreText runs; SkParagraph applies both after shaping, as on the HarfBuzz path
DEF_TEST(CoreTextPipeline_Layout_LetterSpacingAndWordSpacing, reporter) {
    ParagraphStyle paragraphStyle;
    paragraphStyle.setTextShapingEngine(TextShapingEngine::kCoreText);
    auto fontCollection = sk_make_sp<FontCollection>();
    fontCollection->setDefaultFontManager(ToolUtils::TestFontMgr());

    auto createParagraph = [&](const char* text, SkScalar letterSpacing, SkScalar wordSpacing) {
        ParagraphBuilderImpl builder(paragraphStyle, fontCollection, get_unicode(paragraphStyle));
        TextStyle style;
        // Pin Helvetica so baseline at 0.0f has no .SFNS optical trak table
        style.setFontFamilies({SkString("Helvetica")});
        style.setFontSize(20.0f);
        style.setLetterSpacing(letterSpacing);
        style.setWordSpacing(wordSpacing);
        builder.pushStyle(style);
        builder.addText(text);
        builder.pop();
        auto paragraph = builder.Build();
        paragraph->layout(1000.0f);
        return paragraph;
    };

    auto pNormal = createParagraph("Hello World", 0.0f, 0.0f);
    auto pLetterSpaced = createParagraph("Hello World", 5.0f, 0.0f);
    auto pWordSpaced = createParagraph("Hello World", 0.0f, 10.0f);
    auto pCombined = createParagraph("Hello World", 5.0f, 10.0f);

    const SkScalar normalWidth = pNormal->getMaxIntrinsicWidth();
    const SkScalar letterSpacedWidth = pLetterSpaced->getMaxIntrinsicWidth();
    const SkScalar wordSpacedWidth = pWordSpaced->getMaxIntrinsicWidth();
    const SkScalar combinedWidth = pCombined->getMaxIntrinsicWidth();

    // "Hello World" is 11 codepoints; letter spacing 5.0 adds exactly 55.0 points
    REPORTER_ASSERT(reporter, SkScalarNearlyEqual(letterSpacedWidth - normalWidth, 55.0f, 0.05f));

    // Exactly 1 space cluster; word spacing 10.0 adds exactly 10.0 points
    REPORTER_ASSERT(reporter, SkScalarNearlyEqual(wordSpacedWidth - normalWidth, 10.0f, 0.05f));

    // Combined spacing adds 55.0 (letter) + 10.0 (word) = 65.0 points
    REPORTER_ASSERT(reporter, SkScalarNearlyEqual(combinedWidth - normalWidth, 65.0f, 0.05f));

    // Verify cluster halfLetterSpacing contract for Flutter caret geometry
    auto implLS = static_cast<ParagraphImpl*>(pLetterSpaced.get());
    for (const auto& cluster : implLS->clusters()) {
        if (cluster.runOrNull() && !cluster.run().isPlaceholder()) {
            REPORTER_ASSERT(reporter,
                            SkScalarNearlyEqual(cluster.getHalfLetterSpacing(), 2.5f, 0.01f));
        }
    }

    // Multi-style span test with distinct letter spacings
    {
        ParagraphBuilderImpl multiBuilder(
                paragraphStyle, fontCollection, get_unicode(paragraphStyle));
        TextStyle s1;
        s1.setFontFamilies({SkString("Helvetica")});
        s1.setFontSize(20.0f);
        s1.setLetterSpacing(4.0f);
        multiBuilder.pushStyle(s1);
        multiBuilder.addText("Hello ");
        multiBuilder.pop();

        TextStyle s2;
        s2.setFontFamilies({SkString("Helvetica")});
        s2.setFontSize(20.0f);
        s2.setLetterSpacing(2.0f);
        multiBuilder.pushStyle(s2);
        multiBuilder.addText("World");
        multiBuilder.pop();

        auto pMulti = multiBuilder.Build();
        pMulti->layout(1000.0f);

        // "Hello " (6 chars * 4 = 24) + "World" (5 chars * 2 = 10) = 34.0 points extra
        REPORTER_ASSERT(
                reporter,
                SkScalarNearlyEqual(pMulti->getMaxIntrinsicWidth() - normalWidth, 34.0f, 0.05f));
    }

    // Arabic cursive script gets no letterSpacing, as on the HarfBuzz path
    {
        const char* arabic = "سلام";
        auto pArabNormal = createParagraph(arabic, 0.0f, 0.0f);
        auto pArabSpaced = createParagraph(arabic, 50.0f, 0.0f);

        REPORTER_ASSERT(reporter,
                        SkScalarNearlyEqual(pArabSpaced->getMaxIntrinsicWidth(),
                                            pArabNormal->getMaxIntrinsicWidth(),
                                            0.05f));
    }

    // Hit-testing and caret geometry contracts with letter spacing
    {
        auto boxes = pLetterSpaced->getRectsForRange(
                0, 5, RectHeightStyle::kMax, RectWidthStyle::kTight);
        REPORTER_ASSERT(reporter, !boxes.empty());
        REPORTER_ASSERT(reporter, boxes[0].rect.width() > 0.0f);
    }
}

// An empty family list binds the system UI font with no unresolved glyphs, and bold or italic
// styles reach it as traits; this is the default font of every Flutter app on Apple platforms
DEF_TEST(CoreTextPipeline_AppleSystemFonts_OpticalAndTraits, reporter) {
    auto fontCollection = sk_make_sp<FontCollection>();

    // Default empty family resolves to system font
    {
        ParagraphStyle paragraphStyle;
        paragraphStyle.setTextShapingEngine(TextShapingEngine::kCoreText);
        ParagraphBuilderImpl builder(paragraphStyle, fontCollection, get_unicode(paragraphStyle));
        TextStyle style;
        style.setFontSize(16.0f);
        builder.pushStyle(style);
        builder.addText("System Font Regular");
        builder.pop();

        auto paragraph = builder.Build();
        paragraph->layout(500.0f);

        REPORTER_ASSERT(reporter, paragraph->unresolvedGlyphs() == 0);
        REPORTER_ASSERT(reporter, paragraph->getMaxIntrinsicWidth() > 0.0f);

        auto impl = static_cast<ParagraphImpl*>(paragraph.get());
        REPORTER_ASSERT(reporter, impl->runs().size() == 1);
        auto& run = impl->runs()[0];
        REPORTER_ASSERT(reporter,
                        CoreTextFontRegistry::isAppleSystemFont(run.font().getTypeface()));
    }

    // Bold and italic traits on system font
    {
        ParagraphStyle paragraphStyle;
        paragraphStyle.setTextShapingEngine(TextShapingEngine::kCoreText);
        ParagraphBuilderImpl builder(paragraphStyle, fontCollection, get_unicode(paragraphStyle));
        TextStyle boldStyle;
        boldStyle.setFontSize(16.0f);
        boldStyle.setFontStyle(SkFontStyle::Bold());
        builder.pushStyle(boldStyle);
        builder.addText("Bold");
        builder.pop();

        TextStyle italicStyle;
        italicStyle.setFontSize(16.0f);
        italicStyle.setFontStyle(SkFontStyle::Italic());
        builder.pushStyle(italicStyle);
        builder.addText(" Italic");
        builder.pop();

        auto paragraph = builder.Build();
        paragraph->layout(500.0f);

        REPORTER_ASSERT(reporter, paragraph->unresolvedGlyphs() == 0);
        REPORTER_ASSERT(reporter, paragraph->getMaxIntrinsicWidth() > 0.0f);
    }

    // Named system font aliases
    for (const char* alias : {".AppleSystemUIFont", "SF Pro", "system-ui", "-apple-system"}) {
        ParagraphStyle paragraphStyle;
        paragraphStyle.setTextShapingEngine(TextShapingEngine::kCoreText);
        ParagraphBuilderImpl builder(paragraphStyle, fontCollection, get_unicode(paragraphStyle));
        TextStyle style;
        style.setFontFamilies({SkString(alias)});
        style.setFontSize(14.0f);
        builder.pushStyle(style);
        builder.addText("Apple Alias Test");
        builder.pop();

        auto paragraph = builder.Build();
        paragraph->layout(500.0f);

        REPORTER_ASSERT(reporter, paragraph->unresolvedGlyphs() == 0);
        REPORTER_ASSERT(reporter, paragraph->getMaxIntrinsicWidth() > 0.0f);
    }
}

// Latin, CJK, emoji and Arabic in one paragraph resolve through the CoreText cascade, in
// logical order and never with LastResort, without the default font manager's fallback
DEF_TEST(CoreTextPipeline_AppleSystemFonts_MultiScriptFallbackChain, reporter) {
    if (!has_font_families({"Courier", "Apple Color Emoji", "PingFang SC"})) {
        return;
    }
    auto fontCollection = sk_make_sp<FontCollection>();
    fontCollection->setDefaultFontManager(ToolUtils::TestFontMgr());

    // Single pass shapes mixed Latin + CJK + Emoji + Arabic via CoreText platform fallback
    {
        ParagraphStyle paragraphStyle;
        paragraphStyle.setTextShapingEngine(TextShapingEngine::kCoreText);
        ParagraphBuilderImpl builder(paragraphStyle, fontCollection, get_unicode(paragraphStyle));
        TextStyle style;
        style.setFontSize(16.0f);
        builder.pushStyle(style);
        const char* mixedText = "Hello 世界 🎉 مرحبا";
        builder.addText(mixedText);
        builder.pop();

        auto paragraph = builder.Build();
        paragraph->layout(1000.0f);

        REPORTER_ASSERT(reporter, paragraph->unresolvedGlyphs() == 0);
        REPORTER_ASSERT(reporter, paragraph->getMaxIntrinsicWidth() > 0.0f);

        auto impl = static_cast<ParagraphImpl*>(paragraph.get());
        REPORTER_ASSERT(reporter, impl->runs().size() >= 4);

        // Logical text order invariant across script runs
        for (size_t i = 1; i < impl->runs().size(); ++i) {
            REPORTER_ASSERT(
                    reporter,
                    impl->runs()[i].textRange().start >= impl->runs()[i - 1].textRange().end);
        }

        bool foundLatin = false;
        bool foundCJK = false;
        bool foundEmoji = false;
        bool foundArabic = false;

        for (const auto& run : impl->runs()) {
            REPORTER_ASSERT(reporter, run.size() > 0);
            REPORTER_ASSERT(reporter, run.advance().fX > 0.0f);
            REPORTER_ASSERT(reporter, run.font().getTypeface() != nullptr);

            SkString family;
            run.font().getTypeface()->getFamilyName(&family);
            SkString psName;
            run.font().getTypeface()->getPostScriptName(&psName);

            REPORTER_ASSERT(reporter, !family.contains("LastResort"));
            REPORTER_ASSERT(reporter, !psName.contains("LastResort"));

            if (family.contains("Emoji") || psName.contains("Emoji")) {
                foundEmoji = true;
                REPORTER_ASSERT(reporter, run.leftToRight());
            } else if (run.script() == SkSetFourByteTag('L', 'a', 't', 'n')) {
                foundLatin = true;
                REPORTER_ASSERT(reporter, run.leftToRight());
            } else if (run.script() == SkSetFourByteTag('H', 'a', 'n', 'i')) {
                // The space after the ideographs inherits their script but stays in the
                // primary font
                const char* first = mixedText + run.textRange().start;
                if (SkUTF::NextUTF8(&first, mixedText + run.textRange().end) == ' ') {
                    continue;
                }
                foundCJK = true;
                REPORTER_ASSERT(reporter, run.leftToRight());
                REPORTER_ASSERT(reporter,
                                family.contains("PingFang") || family.contains("Hiragino") ||
                                        family.contains("Songti") || family.contains("Heiti") ||
                                        psName.contains("PingFang") || psName.contains("Hiragino"));
            } else if (run.script() == SkSetFourByteTag('A', 'r', 'a', 'b')) {
                foundArabic = true;
                REPORTER_ASSERT(reporter, !run.leftToRight());
                REPORTER_ASSERT(reporter, run.isCursiveScript());
            }
        }

        REPORTER_ASSERT(reporter, foundLatin);
        REPORTER_ASSERT(reporter, foundCJK);
        REPORTER_ASSERT(reporter, foundEmoji);
        REPORTER_ASSERT(reporter, foundArabic);

        // Caret and selection hit-testing geometry across script boundaries in UTF-16 code units
        auto boxesLatin =
                paragraph->getRectsForRange(0, 5, RectHeightStyle::kMax, RectWidthStyle::kTight);
        REPORTER_ASSERT(reporter, !boxesLatin.empty());
        if (!boxesLatin.empty()) {
            REPORTER_ASSERT(reporter, boxesLatin[0].rect.width() > 0.0f);
        }

        auto boxesCJK =
                paragraph->getRectsForRange(6, 8, RectHeightStyle::kMax, RectWidthStyle::kTight);
        REPORTER_ASSERT(reporter, !boxesCJK.empty());
        if (!boxesCJK.empty()) {
            REPORTER_ASSERT(reporter, boxesCJK[0].rect.width() > 0.0f);
        }

        auto boxesEmoji =
                paragraph->getRectsForRange(9, 11, RectHeightStyle::kMax, RectWidthStyle::kTight);
        REPORTER_ASSERT(reporter, !boxesEmoji.empty());
        if (!boxesEmoji.empty()) {
            REPORTER_ASSERT(reporter, boxesEmoji[0].rect.width() > 0.0f);
        }

        auto boxesArabic =
                paragraph->getRectsForRange(12, 17, RectHeightStyle::kMax, RectWidthStyle::kTight);
        REPORTER_ASSERT(reporter, !boxesArabic.empty());
        if (!boxesArabic.empty()) {
            REPORTER_ASSERT(reporter, boxesArabic[0].rect.width() > 0.0f);
        }

        // Verification of canvas text blob paint execution across fallback fonts
        SkBitmap bitmap;
        bitmap.allocN32Pixels(500, 100);
        SkCanvas canvas(bitmap);
        canvas.clear(SK_ColorWHITE);
        paragraph->paint(&canvas, 0, 0);
    }

    // Explicit cascade list ordering via kCTFontCascadeListAttribute
    {
        ParagraphStyle paragraphStyle;
        paragraphStyle.setTextShapingEngine(TextShapingEngine::kCoreText);
        ParagraphBuilderImpl builder(paragraphStyle, fontCollection, get_unicode(paragraphStyle));
        TextStyle style;
        style.setFontSize(16.0f);
        style.setFontFamilies(
                {SkString("Courier"), SkString("Apple Color Emoji"), SkString("PingFang SC")});
        builder.pushStyle(style);
        builder.addText("Courier: 世界 🎉");
        builder.pop();

        auto paragraph = builder.Build();
        paragraph->layout(1000.0f);

        REPORTER_ASSERT(reporter, paragraph->unresolvedGlyphs() == 0);
        REPORTER_ASSERT(reporter, paragraph->getMaxIntrinsicWidth() > 0.0f);

        auto impl = static_cast<ParagraphImpl*>(paragraph.get());
        REPORTER_ASSERT(reporter, impl->runs().size() >= 3);

        auto& runCourier = impl->runs()[0];
        SkString family0;
        runCourier.font().getTypeface()->getFamilyName(&family0);
        REPORTER_ASSERT(reporter, family0.contains("Courier"));
    }
}

// With fallback disabled installed families stay hidden, as in FontCollection, so even Latin
// is unresolved and drawn as glyph 0; Flutter's disableFontFallback has to mean the same here
DEF_TEST(CoreTextPipeline_DisableFontFallback, reporter) {
    auto fontCollection = sk_make_sp<FontCollection>();
    fontCollection->setDefaultFontManager(ToolUtils::TestFontMgr());
    fontCollection->disableFontFallback();
    REPORTER_ASSERT(reporter, !fontCollection->fontFallbackEnabled());

    ParagraphStyle paragraphStyle;
    paragraphStyle.setTextShapingEngine(TextShapingEngine::kCoreText);
    TextStyle style;
    style.setFontFamilies({SkString("Courier")});
    style.setFontSize(16.0f);

    {
        ParagraphBuilderImpl builder(paragraphStyle, fontCollection, get_unicode(paragraphStyle));
        builder.pushStyle(style);
        const char* mixedText = "Hello 世界 🎉 مرحبا";
        builder.addText(mixedText);
        builder.pop();

        auto paragraph = builder.Build();
        paragraph->layout(1000.0f);

        REPORTER_ASSERT(reporter, paragraph->unresolvedGlyphs() > 0);
        auto unresolvedSet = paragraph->unresolvedCodepoints();
        REPORTER_ASSERT(reporter, !unresolvedSet.empty());
        // Installed Courier stays hidden as in FontCollection, so even Latin is unresolved
        REPORTER_ASSERT(reporter, unresolvedSet.count('H') == 1);
        REPORTER_ASSERT(reporter,
                        CoreTextFontRegistry::Instance()
                                .findTypefaces(*fontCollection,
                                               style.getFontFamilies(),
                                               style.getFontStyle(),
                                               std::nullopt)
                                .empty());
        REPORTER_ASSERT(reporter,
                        fontCollection->findTypefaces(style.getFontFamilies(),
                                                      style.getFontStyle())
                                .empty());

        auto impl = static_cast<ParagraphImpl*>(paragraph.get());
        for (const auto& run : impl->runs()) {
            if (run.glyphs().empty()) {
                continue;
            }
            if (run.glyphs()[0] == 0) {
                for (size_t i = 0; i < run.size(); ++i) {
                    REPORTER_ASSERT(reporter, run.glyphs()[i] == 0);
                }
            }
        }
    }

    {
        ParagraphBuilderImpl builder1(paragraphStyle, fontCollection, get_unicode(paragraphStyle));
        builder1.pushStyle(style);
        builder1.addText("世界");
        builder1.pop();
        auto p1 = builder1.Build();
        p1->layout(1000.0f);

        ParagraphBuilderImpl builder2(paragraphStyle, fontCollection, get_unicode(paragraphStyle));
        builder2.pushStyle(style);
        builder2.addText("世\u200D界\u200B");
        builder2.pop();
        auto p2 = builder2.Build();
        p2->layout(1000.0f);

        REPORTER_ASSERT(
                reporter,
                SkScalarNearlyEqual(p1->getMaxIntrinsicWidth(), p2->getMaxIntrinsicWidth()));
        REPORTER_ASSERT(reporter, p2->unresolvedGlyphs() == 2);
        auto unresolvedP2 = p2->unresolvedCodepoints();
        REPORTER_ASSERT(reporter, unresolvedP2.size() == 2);
        REPORTER_ASSERT(reporter, unresolvedP2.count(0x4E16) == 1);
        REPORTER_ASSERT(reporter, unresolvedP2.count(0x754C) == 1);
        REPORTER_ASSERT(reporter, unresolvedP2.count(0x200D) == 0);
        REPORTER_ASSERT(reporter, unresolvedP2.count(0x200B) == 0);
    }

    {
        // A user face registered under the requested name is the only allowed source of glyphs
        sk_sp<SkTypeface> roboto =
                ToolUtils::CreateTypefaceFromResource("fonts/Roboto-Regular.ttf");
        REPORTER_ASSERT(reporter, roboto);
        auto provider = sk_make_sp<TypefaceFontProvider>();
        provider->registerTypeface(roboto, SkString("Courier"));
        auto userFc = sk_make_sp<FontCollection>();
        userFc->setDefaultFontManager(ToolUtils::TestFontMgr());
        userFc->setDynamicFontManager(provider);
        userFc->disableFontFallback();

        auto faces = CoreTextFontRegistry::Instance().findTypefaces(
                *userFc, style.getFontFamilies(), style.getFontStyle(), std::nullopt);
        REPORTER_ASSERT(reporter, faces.size() == 1 && faces[0]->uniqueID() == roboto->uniqueID());

        ParagraphBuilderImpl builder(paragraphStyle, userFc, get_unicode(paragraphStyle));
        builder.pushStyle(style);
        builder.addText("Hello \xE4\xB8\x96");
        builder.pop();
        auto paragraph = builder.Build();
        paragraph->layout(1000.0f);
        auto unresolved = paragraph->unresolvedCodepoints();
        REPORTER_ASSERT(reporter, unresolved.size() == 1 && unresolved.count(0x4E16) == 1);
        SkString robotoPs;
        roboto->getPostScriptName(&robotoPs);
        auto impl = static_cast<ParagraphImpl*>(paragraph.get());
        for (const auto& run : impl->runs()) {
            // Resolved runs carry a new SkTypeface wrapping the bound CTFont, so compare faces
            // by PostScript name rather than by uniqueID
            if (!run.glyphs().empty() && run.glyphs()[0] != 0) {
                SkString runPs;
                run.font().getTypeface()->getPostScriptName(&runPs);
                REPORTER_ASSERT(reporter, runPs == robotoPs, "%s", runPs.c_str());
            }
        }
    }

    {
        fontCollection->enableFontFallback();
        fontCollection->getParagraphCache()->reset();
        REPORTER_ASSERT(reporter, fontCollection->fontFallbackEnabled());

        ParagraphBuilderImpl builder(paragraphStyle, fontCollection, get_unicode(paragraphStyle));
        builder.pushStyle(style);
        const char* mixedText = "Hello 世界 🎉 مرحبا";
        builder.addText(mixedText);
        builder.pop();

        auto paragraph = builder.Build();
        paragraph->layout(1000.0f);

        REPORTER_ASSERT(reporter, paragraph->unresolvedGlyphs() == 0);
        REPORTER_ASSERT(reporter, paragraph->unresolvedCodepoints().empty());
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

// letterSpacing widens the line on either engine, and the kCoreText request is honored only
// where CoreText is built, so switching engines never drops the spacing
DEF_TEST(CoreTextPipeline_EngineSwitchLetterSpacing, reporter) {
    auto fontCollection = sk_make_sp<FontCollection>();
    fontCollection->setDefaultFontManager(ToolUtils::TestFontMgr());

    auto testEngine = [&](TextShapingEngine engine, bool expectedCoreText) {
        ParagraphStyle styleNoSpacing;
        styleNoSpacing.setTextShapingEngine(engine);
        TextStyle textStyleNoSpacing;
        textStyleNoSpacing.setFontFamilies({SkString("Roboto")});
        textStyleNoSpacing.setFontSize(16.0f);

        ParagraphBuilderImpl b1(styleNoSpacing, fontCollection, get_unicode(styleNoSpacing));
        b1.pushStyle(textStyleNoSpacing);
        b1.addText("Letter Spacing Test");
        b1.pop();
        auto p1 = b1.Build();
        p1->layout(1000.0f);

        auto impl1 = static_cast<ParagraphImpl*>(p1.get());
        REPORTER_ASSERT(reporter, impl1->isCoreTextShaped() == expectedCoreText);

        ParagraphStyle styleSpacing;
        styleSpacing.setTextShapingEngine(engine);
        TextStyle textStyleSpacing;
        textStyleSpacing.setFontFamilies({SkString("Roboto")});
        textStyleSpacing.setFontSize(16.0f);
        textStyleSpacing.setLetterSpacing(4.0f);

        ParagraphBuilderImpl b2(styleSpacing, fontCollection, get_unicode(styleSpacing));
        b2.pushStyle(textStyleSpacing);
        b2.addText("Letter Spacing Test");
        b2.pop();
        auto p2 = b2.Build();
        p2->layout(1000.0f);

        auto impl2 = static_cast<ParagraphImpl*>(p2.get());
        REPORTER_ASSERT(reporter, impl2->isCoreTextShaped() == expectedCoreText);
        REPORTER_ASSERT(reporter, p2->getMaxIntrinsicWidth() > p1->getMaxIntrinsicWidth());
    };

    testEngine(TextShapingEngine::kCoreText, true);
    testEngine(TextShapingEngine::kDefault, false);
}

// ParagraphCache keys include the shaping engine, so a HarfBuzz layout never replays CoreText
// runs of the same text and style, or the other way round
DEF_TEST(CoreTextPipeline_CacheKeyEngineSeparation, reporter) {
    auto fontCollection = sk_make_sp<FontCollection>();
    fontCollection->setDefaultFontManager(ToolUtils::TestFontMgr());

    ParagraphCache cache;
    cache.turnOn(true);

    TextStyle textStyle;
    textStyle.setFontFamilies({SkString("Roboto")});
    textStyle.setFontSize(16.0f);

    auto makeParagraph = [&](TextShapingEngine engine) {
        ParagraphStyle paragraphStyle;
        paragraphStyle.setTextShapingEngine(engine);
        ParagraphBuilderImpl builder(paragraphStyle, fontCollection, get_unicode(paragraphStyle));
        builder.pushStyle(textStyle);
        builder.addText("Paragraph Cache Shaping Engine Test");
        builder.pop();
        auto paragraph = builder.Build();
        paragraph->layout(500.0f);
        return paragraph;
    };

    auto pCT1 = makeParagraph(TextShapingEngine::kCoreText);
    auto implCT1 = static_cast<ParagraphImpl*>(pCT1.get());
    REPORTER_ASSERT(reporter, implCT1->isCoreTextShaped());
    REPORTER_ASSERT(reporter, cache.updateParagraph(implCT1));
    REPORTER_ASSERT(reporter, cache.count() == 1);

    auto pHB1 = makeParagraph(TextShapingEngine::kDefault);
    auto implHB1 = static_cast<ParagraphImpl*>(pHB1.get());
    REPORTER_ASSERT(reporter, !implHB1->isCoreTextShaped());
    REPORTER_ASSERT(reporter, !cache.findParagraph(implHB1));

    REPORTER_ASSERT(reporter, cache.updateParagraph(implHB1));
    REPORTER_ASSERT(reporter, cache.count() == 2);

    ParagraphStyle ctQueryStyle;
    ctQueryStyle.setTextShapingEngine(TextShapingEngine::kCoreText);
    ParagraphBuilderImpl bCT(ctQueryStyle, fontCollection, get_unicode(ctQueryStyle));
    bCT.pushStyle(textStyle);
    bCT.addText("Paragraph Cache Shaping Engine Test");
    bCT.pop();
    auto pCTQuery = bCT.Build();
    auto implCTQuery = static_cast<ParagraphImpl*>(pCTQuery.get());
    REPORTER_ASSERT(reporter, !implCTQuery->isCoreTextShaped());
    REPORTER_ASSERT(reporter, cache.findParagraph(implCTQuery));
    REPORTER_ASSERT(reporter, implCTQuery->isCoreTextShaped());

    ParagraphStyle hbQueryStyle;
    hbQueryStyle.setTextShapingEngine(TextShapingEngine::kDefault);
    ParagraphBuilderImpl bHB(hbQueryStyle, fontCollection, get_unicode(hbQueryStyle));
    bHB.pushStyle(textStyle);
    bHB.addText("Paragraph Cache Shaping Engine Test");
    bHB.pop();
    auto pHBQuery = bHB.Build();
    auto implHBQuery = static_cast<ParagraphImpl*>(pHBQuery.get());
    REPORTER_ASSERT(reporter, !implHBQuery->isCoreTextShaped());
    REPORTER_ASSERT(reporter, cache.findParagraph(implHBQuery));
    REPORTER_ASSERT(reporter, !implHBQuery->isCoreTextShaped());
}

namespace {
using Coordinate = SkFontArguments::VariationPosition::Coordinate;

// Lays out "abcabcabc" at 48pt in one family, optionally pinning a single variation axis.
// The sample is restricted to glyphs that fonts/Variable.ttf actually maps
std::unique_ptr<Paragraph> layout_axis_sample(const sk_sp<FontCollection>& fontCollection,
                                              TextShapingEngine engine,
                                              const char* family,
                                              std::optional<Coordinate> coord) {
    TextStyle style;
    style.setFontFamilies({SkString(family)});
    style.setFontSize(48.0f);
    if (coord) {
        SkFontArguments args;
        args.setVariationDesignPosition({&*coord, 1});
        style.setFontArguments(args);
    }
    ParagraphStyle paragraphStyle;
    paragraphStyle.setTextShapingEngine(engine);
    ParagraphBuilderImpl builder(paragraphStyle, fontCollection, get_unicode(paragraphStyle));
    builder.pushStyle(style);
    builder.addText("abcabcabc");
    builder.pop();
    auto paragraph = builder.Build();
    paragraph->layout(1000.0f);
    return paragraph;
}

const SkTypeface* first_run_typeface(Paragraph* paragraph) {
    SkSpan<Run> runs = static_cast<ParagraphImpl*>(paragraph)->runs();
    return runs.empty() ? nullptr : runs[0].font().getTypeface();
}

// SkTypeface_Mac surfaces CTFontCopyVariation here, so the run typeface reports the exact
// position CoreText shaped with; axes left unset report the font's default value
std::optional<float> axis_value(const SkTypeface* typeface, SkFourByteTag axis) {
    constexpr int kMaxAxes = 16;
    Coordinate coords[kMaxAxes];
    const int count = typeface ? typeface->getVariationDesignPosition(coords) : -1;
    if (count < 0 || count > kMaxAxes) {
        return std::nullopt;
    }
    for (int i = 0; i < count; ++i) {
        if (coords[i].axis == axis) {
            return coords[i].value;
        }
    }
    return std::nullopt;
}

bool axis_near(const SkTypeface* typeface, SkFourByteTag axis, float expected) {
    std::optional<float> value = axis_value(typeface, axis);
    return value && SkScalarNearlyEqual(*value, expected, 0.5f);
}
}  // namespace

// Variation axes from TextStyle font arguments reach the CTFont and move advances, and both
// engines agree on the varied face; a dropped axis would leave the three widths equal
DEF_TEST(CoreTextPipeline_FontArguments_VariableFontAxes, reporter) {
    sk_sp<SkTypeface> variable = ToolUtils::CreateTypefaceFromResource("fonts/Variable.ttf");
    if (!variable) {
        // Only CoreText-backed font managers load resource data; the portable one cannot
        return;
    }
    auto provider = sk_make_sp<TypefaceFontProvider>();
    provider->registerTypeface(variable, SkString("Variable"));
    auto fontCollection = sk_make_sp<FontCollection>();
    fontCollection->setAssetFontManager(provider);
    fontCollection->setDefaultFontManager(ToolUtils::TestFontMgr());

    auto layout = [&](TextShapingEngine engine, std::optional<Coordinate> coord) {
        return layout_axis_sample(fontCollection, engine, "Variable", coord);
    };

    // TextStyle font arguments must reach the CTFont: wdth moves advances in this font
    // (wght does not), so the three layouts can only agree when the axis is dropped
    auto regular = layout(TextShapingEngine::kCoreText, std::nullopt);
    auto narrow = layout(TextShapingEngine::kCoreText, Coordinate{Coordinate::wdth, 50.0f});
    auto wide = layout(TextShapingEngine::kCoreText, Coordinate{Coordinate::wdth, 200.0f});
    for (Paragraph* paragraph : {regular.get(), narrow.get(), wide.get()}) {
        REPORTER_ASSERT(reporter, static_cast<ParagraphImpl*>(paragraph)->isCoreTextShaped());
        REPORTER_ASSERT(reporter, paragraph->unresolvedGlyphs() == 0);
    }
    const SkScalar regularWidth = regular->getMaxIntrinsicWidth();
    const SkScalar narrowWidth = narrow->getMaxIntrinsicWidth();
    const SkScalar wideWidth = wide->getMaxIntrinsicWidth();
    REPORTER_ASSERT(reporter, narrowWidth < regularWidth - 10.0f,
                    "wdth=50 width %g vs default %g", narrowWidth, regularWidth);
    REPORTER_ASSERT(reporter, wideWidth > regularWidth + 10.0f,
                    "wdth=200 width %g vs default %g", wideWidth, regularWidth);
    const SkTypeface* narrowTypeface = first_run_typeface(narrow.get());
    REPORTER_ASSERT(reporter, axis_near(narrowTypeface, Coordinate::wdth, 50.0f),
                    "run typeface reports wdth %g",
                    axis_value(narrowTypeface, Coordinate::wdth).value_or(-1.0f));

    // Both engines take advances from CoreText for the same varied face, so they must agree
    auto hbNarrow = layout(TextShapingEngine::kDefault, Coordinate{Coordinate::wdth, 50.0f});
    const SkScalar hbNarrowWidth = hbNarrow->getMaxIntrinsicWidth();
    REPORTER_ASSERT(reporter, !static_cast<ParagraphImpl*>(hbNarrow.get())->isCoreTextShaped());
    REPORTER_ASSERT(reporter, SkScalarNearlyEqual(hbNarrowWidth, narrowWidth, 1.0f),
                    "HarfBuzz wdth=50 width %g vs CoreText %g", hbNarrowWidth, narrowWidth);
}

// A wght axis on the system font changes its weight while opsz keeps following the point size,
// since pinning opsz would freeze the optical size the system font picks per size
DEF_TEST(CoreTextPipeline_FontArguments_SystemFontWeightAxis, reporter) {
    if (!has_font_families({".AppleSystemUIFont"})) {
        return;
    }
    // Flutter's default family on Apple platforms. wght must come from the font arguments,
    // while opsz keeps tracking the point size: pinning it (as SkCTFontCreateExactCopy does on
    // the HarfBuzz path) would freeze the optical size the system font picks per size
    auto check = [&](const sk_sp<FontCollection>& fontCollection) {
        auto layout = [&](std::optional<Coordinate> coord) {
            return layout_axis_sample(fontCollection, TextShapingEngine::kCoreText,
                                      ".AppleSystemUIFont", coord);
        };
        auto regular = layout(std::nullopt);
        auto black = layout(Coordinate{Coordinate::wght, 900.0f});
        auto thin = layout(Coordinate{Coordinate::wght, 100.0f});
        for (Paragraph* paragraph : {regular.get(), black.get(), thin.get()}) {
            REPORTER_ASSERT(reporter, static_cast<ParagraphImpl*>(paragraph)->isCoreTextShaped());
            REPORTER_ASSERT(reporter, paragraph->unresolvedGlyphs() == 0);
        }
        const SkScalar regularWidth = regular->getMaxIntrinsicWidth();
        const SkScalar blackWidth = black->getMaxIntrinsicWidth();
        const SkScalar thinWidth = thin->getMaxIntrinsicWidth();
        REPORTER_ASSERT(reporter, blackWidth > regularWidth + 20.0f,
                        "wght=900 width %g vs regular %g", blackWidth, regularWidth);
        REPORTER_ASSERT(reporter, thinWidth < regularWidth - 5.0f,
                        "wght=100 width %g vs regular %g", thinWidth, regularWidth);

        const SkTypeface* regularTypeface = first_run_typeface(regular.get());
        const SkTypeface* blackTypeface = first_run_typeface(black.get());
        REPORTER_ASSERT(reporter, axis_near(blackTypeface, Coordinate::wght, 900.0f),
                        "wght=900 run typeface reports wght %g",
                        axis_value(blackTypeface, Coordinate::wght).value_or(-1.0f));
        REPORTER_ASSERT(reporter, axis_near(regularTypeface, Coordinate::opsz, 48.0f),
                        "regular run typeface reports opsz %g",
                        axis_value(regularTypeface, Coordinate::opsz).value_or(-1.0f));
        REPORTER_ASSERT(reporter, axis_near(blackTypeface, Coordinate::opsz, 48.0f),
                        "wght=900 run typeface reports opsz %g",
                        axis_value(blackTypeface, Coordinate::opsz).value_or(-1.0f));
    };

    // An empty collection binds the system font through CoreText alone; a CoreText-backed
    // default manager resolves the same family to an SkTypeface_Mac first
    check(sk_make_sp<FontCollection>());
    auto withDefaultManager = sk_make_sp<FontCollection>();
    withDefaultManager->setDefaultFontManager(ToolUtils::TestFontMgr());
    check(withDefaultManager);
}

namespace {
// Flutter's default family on Apple platforms carrying the BCP-47 tag Flutter copies from
// Locale.toLanguageTag() into TextStyle::setLocale
TextStyle system_font_style(const char* locale) {
    TextStyle style;
    style.setFontFamilies({SkString(".AppleSystemUIFont")});
    style.setFontSize(16.0f);
    style.setLocale(SkString(locale));
    return style;
}

// Lays out U+9AA8 U+76F4 U+5203 in a single span through CoreText
std::unique_ptr<Paragraph> layout_locale_sample(const sk_sp<FontCollection>& fontCollection,
                                                const char* locale) {
    ParagraphStyle paragraphStyle;
    paragraphStyle.setTextShapingEngine(TextShapingEngine::kCoreText);
    ParagraphBuilderImpl builder(paragraphStyle, fontCollection, get_unicode(paragraphStyle));
    builder.pushStyle(system_font_style(locale));
    builder.addText("\xE9\xAA\xA8\xE7\x9B\xB4\xE5\x88\x83");
    builder.pop();
    auto paragraph = builder.Build();
    paragraph->layout(1000.0f);
    return paragraph;
}

// Family of the typeface CoreText bound to the run; empty when the run does not exist so a
// failed contrast reports what was shaped instead of indexing past the run array
SkString run_family(Paragraph* paragraph, size_t index) {
    SkSpan<Run> runs = static_cast<ParagraphImpl*>(paragraph)->runs();
    SkString family;
    if (index < runs.size() && runs[index].font().getTypeface()) {
        runs[index].font().getTypeface()->getFamilyName(&family);
    }
    return family;
}
}  // namespace

// The span locale reaches CoreText as kCTLanguageAttributeName, so unified Han code points fall
// back to the Japanese, Chinese or Korean font the locale asks for
DEF_TEST(CoreTextPipeline_Locale_LanguageAttributeDrivesCJKFallback, reporter) {
    // An empty collection leaves every fallback decision to CoreText, which is where the
    // span locale has to arrive as kCTLanguageAttributeName
    auto fontCollection = sk_make_sp<FontCollection>();
    auto ja = layout_locale_sample(fontCollection, "ja");
    auto jaJP = layout_locale_sample(fontCollection, "ja-JP");
    auto ko = layout_locale_sample(fontCollection, "ko");
    auto zhHant = layout_locale_sample(fontCollection, "zh-Hant");
    auto zhHans = layout_locale_sample(fontCollection, "zh-Hans");

    ParagraphStyle paragraphStyle;
    paragraphStyle.setTextShapingEngine(TextShapingEngine::kCoreText);
    ParagraphBuilderImpl builder(paragraphStyle, fontCollection, get_unicode(paragraphStyle));
    builder.pushStyle(system_font_style("ja"));
    builder.addText("\xE9\xAA\xA8");
    builder.pop();
    builder.pushStyle(system_font_style("zh-Hant"));
    builder.addText("\xE9\xAA\xA8");
    builder.pop();
    auto mixed = builder.Build();
    mixed->layout(1000.0f);

    for (Paragraph* paragraph :
         {ja.get(), jaJP.get(), ko.get(), zhHant.get(), zhHans.get(), mixed.get()}) {
        auto impl = static_cast<ParagraphImpl*>(paragraph);
        REPORTER_ASSERT(reporter, impl->isCoreTextShaped());
        REPORTER_ASSERT(reporter, paragraph->unresolvedGlyphs() == 0);
        REPORTER_ASSERT(reporter, !impl->runs().empty());
    }

    // Han unification: U+9AA8 U+76F4 U+5203 are the same code points in ja, zh-Hans, zh-Hant
    // and ko, so only the span locale can tell CoreText which regional font to fall back to.
    // Contrasts stay between explicit tags: the no-locale pick follows the machine's languages
    const SkString jaFamily = run_family(ja.get(), 0);
    const SkString jaJPFamily = run_family(jaJP.get(), 0);
    const SkString koFamily = run_family(ko.get(), 0);
    const SkString zhHantFamily = run_family(zhHant.get(), 0);
    const SkString zhHansFamily = run_family(zhHans.get(), 0);
    REPORTER_ASSERT(reporter, jaFamily.contains("Hiragino"),
                    "locale ja shaped with '%s'", jaFamily.c_str());
    REPORTER_ASSERT(reporter, jaJPFamily.contains("Hiragino"),
                    "locale ja-JP shaped with '%s'", jaJPFamily.c_str());
    REPORTER_ASSERT(reporter, koFamily.contains("Apple SD Gothic"),
                    "locale ko shaped with '%s'", koFamily.c_str());
    REPORTER_ASSERT(reporter, zhHantFamily.contains("PingFang") && zhHantFamily.contains("TC"),
                    "locale zh-Hant shaped with '%s'", zhHantFamily.c_str());
    REPORTER_ASSERT(reporter, !zhHansFamily.contains("Hiragino"),
                    "locale zh-Hans shaped with '%s'", zhHansFamily.c_str());

    // The language attribute must cover the span range only: a paragraph-wide tag or one
    // leaking out of the first span would shape both ideographs with the Japanese font
    const size_t mixedRuns = static_cast<ParagraphImpl*>(mixed.get())->runs().size();
    const SkString mixedJaFamily = run_family(mixed.get(), 0);
    const SkString mixedZhHantFamily = run_family(mixed.get(), 1);
    REPORTER_ASSERT(reporter, mixedRuns >= 2,
                    "ja + zh-Hant spans produced %zu run(s)", mixedRuns);
    REPORTER_ASSERT(reporter, mixedJaFamily.contains("Hiragino"),
                    "ja span shaped with '%s'", mixedJaFamily.c_str());
    REPORTER_ASSERT(reporter, mixedZhHantFamily.contains("TC"),
                    "zh-Hant span shaped with '%s'", mixedZhHantFamily.c_str());
}

namespace {
sk_sp<FontCollection> make_roboto_collection(const sk_sp<SkTypeface>& regular) {
    auto provider = sk_make_sp<TypefaceFontProvider>();
    provider->registerTypeface(regular, SkString("Roboto"));
    auto fontCollection = sk_make_sp<FontCollection>();
    fontCollection->setAssetFontManager(provider);
    fontCollection->setDefaultFontManager(ToolUtils::TestFontMgr());
    return fontCollection;
}

// Font bound to the first run; nullptr when nothing was shaped so the caller reports the
// empty paragraph instead of indexing past the run array
const SkFont* first_run_font(Paragraph* paragraph) {
    SkSpan<Run> runs = static_cast<ParagraphImpl*>(paragraph)->runs();
    return runs.empty() ? nullptr : &runs[0].font();
}

// Lays out "Synthetic" in one span so the first run carries the whole style request
std::unique_ptr<Paragraph> layout_styled_sample(const sk_sp<FontCollection>& fontCollection,
                                                TextShapingEngine engine,
                                                const char* family,
                                                SkFontStyle fontStyle,
                                                bool fakeMissingFontStyles) {
    TextStyle style;
    style.setFontFamilies({SkString(family)});
    style.setFontSize(24.0f);
    style.setFontStyle(fontStyle);
    ParagraphStyle paragraphStyle;
    paragraphStyle.setTextShapingEngine(engine);
    paragraphStyle.setFakeMissingFontStyles(fakeMissingFontStyles);
    ParagraphBuilderImpl builder(paragraphStyle, fontCollection, get_unicode(paragraphStyle));
    builder.pushStyle(style);
    builder.addText("Synthetic");
    builder.pop();
    auto paragraph = builder.Build();
    paragraph->layout(1000.0f);
    return paragraph;
}
}  // namespace

// A bold italic request on a Regular-only face gets the same fake bold and skew as on the
// HarfBuzz path (fakeMissingFontStyles), so synthetic styles look the same on both engines
DEF_TEST(CoreTextPipeline_SyntheticStyles_ParityWithHarfBuzz, reporter) {
    sk_sp<SkTypeface> regular = ToolUtils::CreateTypefaceFromResource("fonts/Roboto-Regular.ttf");
    if (!regular) {
        // Only CoreText-backed font managers load resource data; the portable one cannot
        return;
    }
    const SkFontStyle boldItalic(SkFontStyle::kBold_Weight, SkFontStyle::kNormal_Width,
                                 SkFontStyle::kItalic_Slant);
    REPORTER_ASSERT(reporter, ParagraphStyle().fakeMissingFontStyles());

    auto fontCollection = make_roboto_collection(regular);
    auto ct = layout_styled_sample(fontCollection, TextShapingEngine::kCoreText, "Roboto",
                                   boldItalic, true);
    auto hb = layout_styled_sample(
            fontCollection, TextShapingEngine::kDefault, "Roboto", boldItalic, true);
    REPORTER_ASSERT(reporter, static_cast<ParagraphImpl*>(ct.get())->isCoreTextShaped());
    REPORTER_ASSERT(reporter, !static_cast<ParagraphImpl*>(hb.get())->isCoreTextShaped());
    for (Paragraph* paragraph : {ct.get(), hb.get()}) {
        REPORTER_ASSERT(reporter, paragraph->unresolvedGlyphs() == 0);
        REPORTER_ASSERT(reporter, first_run_font(paragraph) != nullptr);
    }
    const SkFont* ctFont = first_run_font(ct.get());
    const SkFont* hbFont = first_run_font(hb.get());
    if (!ctFont || !hbFont) {
        return;
    }

    // Only a Regular face is registered, so both engines bind an upright sub-semibold typeface
    // and the bold italic request can only be met synthetically; a failure here means the
    // resource changed, not the pipeline
    for (const SkFont* font : {ctFont, hbFont}) {
        const SkFontStyle bound = font->getTypeface()->fontStyle();
        REPORTER_ASSERT(reporter, bound.weight() < SkFontStyle::kSemiBold_Weight,
                        "Roboto run typeface weight %d", bound.weight());
        REPORTER_ASSERT(reporter, bound.slant() != SkFontStyle::kItalic_Slant,
                        "Roboto run typeface slant %d", static_cast<int>(bound.slant()));
    }

    // OneLineShaper is the oracle for fakeMissingFontStyles; CoreText must leave the run SkFont
    // in the same state, and both sides are pinned absolutely so a regression in one engine
    // cannot hide behind the other
    REPORTER_ASSERT(reporter, hbFont->isEmbolden(), "HarfBuzz run not emboldened");
    REPORTER_ASSERT(reporter, SkScalarNearlyEqual(hbFont->getSkewX(), -0.25f),
                    "HarfBuzz run skewX %g", hbFont->getSkewX());
    REPORTER_ASSERT(reporter, ctFont->isEmbolden(), "CoreText run not emboldened");
    REPORTER_ASSERT(reporter, SkScalarNearlyEqual(ctFont->getSkewX(), -0.25f),
                    "CoreText run skewX %g", ctFont->getSkewX());
    REPORTER_ASSERT(reporter, ctFont->isEmbolden() == hbFont->isEmbolden(),
                    "embolden CoreText %d vs HarfBuzz %d",
                    ctFont->isEmbolden(), hbFont->isEmbolden());
    REPORTER_ASSERT(reporter, SkScalarNearlyEqual(ctFont->getSkewX(), hbFont->getSkewX()),
                    "skewX CoreText %g vs HarfBuzz %g", ctFont->getSkewX(), hbFont->getSkewX());

    // Sharing fontCollection also verifies ParagraphCache keys on fakeMissingFontStyles instead
    // of replaying the emboldened/skewed runs from the flag-on entries above
    for (TextShapingEngine engine : {TextShapingEngine::kCoreText, TextShapingEngine::kDefault}) {
        const bool coreText = engine == TextShapingEngine::kCoreText;
        auto plain = layout_styled_sample(fontCollection, engine, "Roboto", boldItalic, false);
        const SkFont* plainFont = first_run_font(plain.get());
        REPORTER_ASSERT(reporter,
                        static_cast<ParagraphImpl*>(plain.get())->isCoreTextShaped() == coreText);
        REPORTER_ASSERT(reporter, plainFont != nullptr);
        if (plainFont) {
            REPORTER_ASSERT(reporter, !plainFont->isEmbolden(),
                            "engine %d: fakeMissingFontStyles=false still emboldened the run",
                            static_cast<int>(engine));
            REPORTER_ASSERT(reporter, plainFont->getSkewX() == 0,
                            "engine %d: fakeMissingFontStyles=false still skewed the run by %g",
                            static_cast<int>(engine), plainFont->getSkewX());
        }
    }
    REPORTER_ASSERT(reporter, fontCollection->getParagraphCache()->count() == 4,
                    "Expected 4 cache entries (2 engines x 2 fakeMissingFontStyles); got %d",
                    fontCollection->getParagraphCache()->count());

    // CoreText binds a real bold system face for a bold request, so synthesis must compare
    // against the bound face and stay off; keying on the requested weight alone fails here
    auto systemBold = layout_styled_sample(sk_make_sp<FontCollection>(),
                                           TextShapingEngine::kCoreText, ".AppleSystemUIFont",
                                           SkFontStyle::Bold(), true);
    const SkFont* systemFont = first_run_font(systemBold.get());
    REPORTER_ASSERT(reporter, static_cast<ParagraphImpl*>(systemBold.get())->isCoreTextShaped());
    REPORTER_ASSERT(reporter, systemBold->unresolvedGlyphs() == 0);
    REPORTER_ASSERT(reporter, systemFont != nullptr);
    if (systemFont) {
        const int boundWeight = systemFont->getTypeface()->fontStyle().weight();
        REPORTER_ASSERT(reporter, boundWeight >= SkFontStyle::kSemiBold_Weight,
                        "system font bound weight %d for a bold request", boundWeight);
        REPORTER_ASSERT(reporter, !systemFont->isEmbolden(),
                        "real bold face (weight %d) was emboldened", boundWeight);
        REPORTER_ASSERT(reporter, systemFont->getSkewX() == 0,
                        "upright request skewed by %g", systemFont->getSkewX());
    }
}

namespace {
// Records where TextLine hands each glyph blob to the canvas. The canvas places glyph k at
// (x, y) + the blob's own point k, so both halves are kept: their sum is the painted baseline
class BaselineShiftBlobRecorder : public ParagraphPainter {
public:
    static constexpr int kMaxBlobs = 4;
    struct Blob {
        SkScalar fX;
        SkScalar fY;
        uint32_t fGlyphCount;
        SkPoint fFirstGlyph;
    };

    void drawTextBlob(const sk_sp<SkTextBlob>& blob, SkScalar x, SkScalar y,
                      const SkPaintOrID&) override {
        // Every call is counted so an unexpected blob split is reported instead of overrunning
        // the fixed storage
        ++fTotal;
        if (fTotal > kMaxBlobs) {
            return;
        }
        Blob& record = fBlobs[fTotal - 1];
        record = {x, y, 0, {0, 0}};
        if (!blob) {
            return;
        }
        SkTextBlobRunIterator it(blob.get());
        if (!it.done() && it.positioning() == SkTextBlobRunIterator::kFull_Positioning) {
            record.fGlyphCount = it.glyphCount();
            if (record.fGlyphCount > 0) {
                record.fFirstGlyph = it.points()[0];
            }
        }
    }
    void drawTextShadow(const sk_sp<SkTextBlob>&, SkScalar, SkScalar, SkColor,
                        SkScalar) override {}
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
                                               TextShapingEngine engine,
                                               SkScalar shift) {
    TextStyle plain;
    plain.setFontFamilies({SkString("Roboto")});
    plain.setFontSize(20.0f);
    TextStyle shifted = plain;
    shifted.setFontSize(12.0f);
    shifted.setBaselineShift(shift);

    ParagraphStyle paragraphStyle;
    paragraphStyle.setTextShapingEngine(engine);
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
                             TextShapingEngine engine,
                             SkScalar shift) {
    const bool coreText = engine == TextShapingEngine::kCoreText;
    const char* name = coreText ? "CoreText" : "HarfBuzz";
    auto paragraph = layout_shifted_pair(fontCollection, engine, shift);
    auto impl = static_cast<ParagraphImpl*>(paragraph.get());
    REPORTER_ASSERT(reporter, impl->isCoreTextShaped() == coreText);
    REPORTER_ASSERT(reporter, paragraph->unresolvedGlyphs() == 0);
    REPORTER_ASSERT(reporter, paragraph->lineNumber() == 1);

    // Both shapers must hand the style shift to the run model: paint applies it from there
    SkSpan<Run> runs = impl->runs();
    REPORTER_ASSERT(reporter, runs.size() == 2, "%s shift %g: %zu run(s)", name, shift,
                    runs.size());
    for (const Run& run : runs) {
        const SkScalar expected = run.textRange().start == 0 ? 0.0f : shift;
        REPORTER_ASSERT(reporter, run.baselineShift() == expected,
                        "%s shift %g: run [%zu, %zu) carries baselineShift %g", name, shift,
                        run.textRange().start, run.textRange().end, run.baselineShift());
    }

    BaselineShiftBlobRecorder recorder;
    paragraph->paint(&recorder, 0.0f, 0.0f);
    REPORTER_ASSERT(reporter, recorder.fTotal == 2, "%s shift %g: %d blob(s) painted", name,
                    shift, recorder.fTotal);
    if (recorder.fTotal != 2) {
        return SK_ScalarNaN;
    }
    const auto& a = recorder.fBlobs[0];
    const auto& b = recorder.fBlobs[1];
    // LTR line: whichever blob lands further left is "X"
    const bool aIsLeft = a.fX + a.fFirstGlyph.fX < b.fX + b.fFirstGlyph.fX;
    const auto& plain = aIsLeft ? a : b;
    const auto& shifted = aIsLeft ? b : a;
    REPORTER_ASSERT(reporter, plain.fGlyphCount == 1 && shifted.fGlyphCount == 1,
                    "%s shift %g: blobs carry %u and %u glyph(s)", name, shift,
                    plain.fGlyphCount, shifted.fGlyphCount);
    return (shifted.fY + shifted.fFirstGlyph.fY) - (plain.fY + plain.fFirstGlyph.fY);
}
}  // namespace

// A baselineShift span paints shifted by the same amount on both engines, which holds only if
// the CoreText shaper leaves glyphs on the unshifted baseline for TextLine to move
DEF_TEST(CoreTextPipeline_BaselineShift_PaintParityWithHarfBuzz, reporter) {
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
    // positions coming back from either shaper must sit on the unshifted baseline; a shaper
    // that pre-shifts them paints the span back onto the baseline it was meant to leave.
    // The floor(+0.5) rounding of the painted baseline is what the 0.51 tolerance absorbs
    for (SkScalar shift : {6.0f, -6.0f}) {
        const SkScalar ctDelta =
                painted_shift_delta(reporter, fontCollection, TextShapingEngine::kCoreText, shift);
        REPORTER_ASSERT(reporter, SkScalarNearlyEqual(ctDelta, shift, 0.51f),
                        "CoreText paints a %g baseline shift as %g", shift, ctDelta);
        const SkScalar hbDelta =
                painted_shift_delta(reporter, fontCollection, TextShapingEngine::kDefault, shift);
        REPORTER_ASSERT(reporter,
                        SkScalarNearlyEqual(hbDelta, shift, 0.51f),
                        "HarfBuzz paints a %g baseline shift as %g",
                        shift,
                        hbDelta);
        REPORTER_ASSERT(reporter, SkScalarNearlyEqual(ctDelta, hbDelta, 0.51f),
                        "shift %g: CoreText delta %g vs HarfBuzz %g", shift, ctDelta, hbDelta);
    }
}

namespace {
// Lays out text in one span of family through CoreText; callers turn the paragraph cache
// off so every call is an independent typesetting pass
std::unique_ptr<Paragraph> layout_identity_sample(const sk_sp<FontCollection>& fontCollection,
                                                  const char* family,
                                                  const char* text) {
    TextStyle style;
    style.setFontFamilies({SkString(family)});
    style.setFontSize(20.0f);
    ParagraphStyle paragraphStyle;
    paragraphStyle.setTextShapingEngine(TextShapingEngine::kCoreText);
    ParagraphBuilderImpl builder(paragraphStyle, fontCollection, get_unicode(paragraphStyle));
    builder.pushStyle(style);
    builder.addText(text);
    builder.pop();
    auto paragraph = builder.Build();
    paragraph->layout(1000.0f);
    return paragraph;
}

// Identity of the typeface bound to the first run; 0 when nothing was shaped
SkTypefaceID first_run_typeface_id(Paragraph* paragraph) {
    const SkFont* font = first_run_font(paragraph);
    return font && font->getTypeface() ? font->getTypeface()->uniqueID() : 0;
}
}  // namespace

// Relayout binds the same SkTypefaceID for an asset face and for the system font, so glyph
// strikes keyed by typeface stay warm instead of missing on every typesetting pass
DEF_TEST(CoreTextPipeline_TypefaceIdentity_StableAcrossLayouts, reporter) {
    sk_sp<SkTypeface> roboto = ToolUtils::CreateTypefaceFromResource("fonts/Roboto-Regular.ttf");
    if (!roboto) {
        // Only CoreText-backed font managers load resource data; the portable one cannot
        return;
    }
    auto fontCollection = make_roboto_collection(roboto);
    // A cache hit replays the first layout's SkFonts, which would fake the stability under test
    fontCollection->getParagraphCache()->turnOn(false);

    // Glyph strikes downstream are keyed by SkTypefaceID, so a typeface minted per typesetting
    // pass would miss them on every relayout; the data-backed face goes through the registry's
    // descriptor path and the system font through its per-size re-creation
    for (const char* family : {"Roboto", ".AppleSystemUIFont"}) {
        auto first = layout_identity_sample(fontCollection, family, "Identity one");
        auto second = layout_identity_sample(fontCollection, family, "Identity two");
        REPORTER_ASSERT(reporter, static_cast<ParagraphImpl*>(first.get())->isCoreTextShaped());
        REPORTER_ASSERT(reporter, first->unresolvedGlyphs() == 0);
        const SkTypefaceID firstId = first_run_typeface_id(first.get());
        const SkTypefaceID secondId = first_run_typeface_id(second.get());
        REPORTER_ASSERT(reporter, firstId != 0 && firstId == secondId,
                        "%s: typeface %u in the first layout, %u in the second", family,
                        firstId, secondId);

        first->markDirty();
        first->layout(500.0f);
        const SkTypefaceID relaidId = first_run_typeface_id(first.get());
        REPORTER_ASSERT(reporter, relaidId == firstId,
                        "%s: typeface %u before relayout, %u after", family, firstId, relaidId);
    }

    // The asset font must actually be what CoreText bound, or the Roboto pass above only
    // measured the system fallback twice
    auto asset = layout_identity_sample(fontCollection, "Roboto", "Identity");
    REPORTER_ASSERT(reporter, run_family(asset.get(), 0).equals("Roboto"),
                    "Roboto resolved to '%s'", run_family(asset.get(), 0).c_str());
}

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
    REPORTER_ASSERT(reporter,
                    impl->isCoreTextShaped() == (engine == TextShapingEngine::kCoreText));
    REPORTER_ASSERT(reporter, paragraph->unresolvedGlyphs() == 0);
    *clusters = impl->clusters().size();
    return paragraph->getLongestLine();
}
}  // namespace

// letterSpacing tracks Latin by the full amount but leaves Arabic alone, with the same clusters
// as without spacing, on both engines (css-text-4: no tracking inside cursive scripts)
DEF_TEST(CoreTextPipeline_LetterSpacing_SkipsCursiveScriptsLikeHarfBuzz, reporter) {
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
    for (TextShapingEngine engine : {
                 TextShapingEngine::kDefault,
                 TextShapingEngine::kCoreText}) {
        const bool coreText = engine == TextShapingEngine::kCoreText;
        const char* name = coreText ? "CoreText" : "HarfBuzz";
        size_t plainClusters = 0;
        size_t trackedClusters = 0;
        const SkScalar latinDelta =
                tracked_width(reporter, fontCollection, engine, kLatin, kSpacing,
                              &trackedClusters) -
                tracked_width(reporter, fontCollection, engine, kLatin, 0.0f, &plainClusters);
        REPORTER_ASSERT(reporter, SkScalarNearlyEqual(latinDelta, kFullTracking, 0.01f),
                        "%s: Latin grew by %g for letterSpacing %g", name, latinDelta, kSpacing);
        REPORTER_ASSERT(reporter, trackedClusters == plainClusters);

        // Both engines leave joining scripts alone (css-text-4 cursive tracking)
        const SkScalar expectedArabic = 0.0f;
        const SkScalar arabicDelta =
                tracked_width(reporter, fontCollection, engine, kArabic, kSpacing,
                              &trackedClusters) -
                tracked_width(reporter, fontCollection, engine, kArabic, 0.0f, &plainClusters);
        REPORTER_ASSERT(reporter, SkScalarNearlyEqual(arabicDelta, expectedArabic, 0.01f),
                        "%s: Arabic grew by %g for letterSpacing %g, expected %g", name,
                        arabicDelta, kSpacing, expectedArabic);
        REPORTER_ASSERT(reporter, trackedClusters == plainClusters,
                        "%s: %zu clusters tracked vs %zu plain", name, trackedClusters,
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
        return tracked_width(reporter, fontCollection, TextShapingEngine::kCoreText,
                             text.c_str(), kSpacing, &clusters) -
               tracked_width(reporter, fontCollection, TextShapingEngine::kCoreText,
                             text.c_str(), 0.0f, &clusters);
    };
    const SkScalar arabicDelta = delta(arabic);
    REPORTER_ASSERT(reporter, SkScalarNearlyEqual(arabicDelta, 0.0f, 0.01f),
                    "Arabic grew by %g", arabicDelta);
    for (const char* unlisted : {"\u09AC\u09BE\u0982\u09B2\u09BE",
                                 "\u1781\u17D2\u1798\u17C2\u179A",
                                 "\u12A0\u121B\u122D\u129B"}) {
        const SkScalar mixedDelta = delta(arabic + unlisted);
        REPORTER_ASSERT(reporter, mixedDelta >= kSpacing - 0.01f,
                        "%s after Arabic grew by %g for letterSpacing %g",
                        unlisted, mixedDelta, kSpacing);
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
        REPORTER_ASSERT(reporter, run.isCursiveScript() == hasArabic || (!hasArabic && !hasLatin),
                        "%s: run [%zu, %zu) cursive=%d, Arabic=%d, Latin=%d", family,
                        run.textRange().start, run.textRange().end, run.isCursiveScript(),
                        hasArabic, hasLatin);
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
            CursiveSplit plain = split_cursive_runs(reporter, fontCollection, family, text, 0);
            CursiveSplit spaced =
                    split_cursive_runs(reporter, fontCollection, family, text, kSpacing);
            REPORTER_ASSERT(reporter, !plain.fCursiveGlyphs.empty());
            REPORTER_ASSERT(reporter, spaced.fCursiveGlyphs == plain.fCursiveGlyphs,
                            "%s '%s': %zu Arabic glyphs plain, %zu spaced", family, text,
                            plain.fCursiveGlyphs.size(), spaced.fCursiveGlyphs.size());
            REPORTER_ASSERT(reporter,
                            SkScalarNearlyEqual(spaced.fCursiveWidth, plain.fCursiveWidth, 0.01f),
                            "%s '%s': Arabic width %g plain, %g spaced", family, text,
                            plain.fCursiveWidth, spaced.fCursiveWidth);
            REPORTER_ASSERT(reporter, spaced.fOtherCodepoints == plain.fOtherCodepoints);
            // Explicit spacing stacks on the .SFNS auto trak, see
            // StacksOnTrakLikeNativeTracking
            const SkScalar otherDelta = spaced.fOtherWidth - plain.fOtherWidth;
            const SkScalar expectedOther = kSpacing * plain.fOtherCodepoints;
            REPORTER_ASSERT(reporter,
                            SkScalarNearlyEqual(otherDelta, expectedOther, 0.01f),
                            "%s '%s': Latin grew by %g, expected %g",
                            family,
                            text,
                            otherDelta,
                            expectedOther);
        }
        CursiveSplit mixed = split_cursive_runs(reporter, fontCollection, family, kMixed, 0);
        REPORTER_ASSERT(reporter, mixed.fOtherCodepoints >= 5);
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
    builder.addText("a\xEF\xBB\xBF" "\xD7\xA9\xD7\x9C");
    auto paragraph = builder.Build();
    paragraph->layout(1000);
    auto runs = static_cast<ParagraphImpl*>(paragraph.get())->runs();
    REPORTER_ASSERT(reporter, runs.size() == 2);
    if (runs.size() != 2) {
        return;
    }
    const Run& latin = runs[0];
    REPORTER_ASSERT(reporter, latin.size() < latin.textRange().width());
    REPORTER_ASSERT(reporter, latin.positions()[latin.size()].fX == runs[1].positions()[0].fX,
                    "first run ends at %g, second starts at %g",
                    latin.positions()[latin.size()].fX, runs[1].positions()[0].fX);
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
    REPORTER_ASSERT(reporter, paragraph->getMaxIntrinsicWidth() == 0,
                    "zero-size text is %g wide", paragraph->getMaxIntrinsicWidth());
    // Zero sizes stay on CoreText so a build without HarfBuzz lays them out as well
    REPORTER_ASSERT(reporter, static_cast<ParagraphImpl*>(paragraph.get())->isCoreTextShaped());
}

// The ellipsis of a truncated CoreText line is shaped and resolved through CoreText, so it
// still appears in a build without HarfBuzz
DEF_TEST(CoreTextPipeline_EllipsisShapedByCoreText, reporter) {
    if (!has_font_families({"Helvetica"})) {
        return;
    }
    auto fontCollection = test_font_collection();
    ParagraphStyle style;
    style.setTextShapingEngine(TextShapingEngine::kCoreText);
    style.setMaxLines(1);
    style.setEllipsis(u"\u2026");
    ParagraphBuilderImpl builder(style, fontCollection, get_unicode(style));
    TextStyle textStyle;
    textStyle.setFontFamilies({SkString("Helvetica")});
    textStyle.setFontSize(20);
    builder.pushStyle(textStyle);
    builder.addText("Ellipsis must survive a build without HarfBuzz");
    builder.pop();
    auto paragraph = builder.Build();
    paragraph->layout(150);
    auto impl = static_cast<ParagraphImpl*>(paragraph.get());
    REPORTER_ASSERT(reporter, impl->isCoreTextShaped());
    REPORTER_ASSERT(reporter, paragraph->didExceedMaxLines());
    REPORTER_ASSERT(reporter, impl->lines().size() == 1);
    if (impl->lines().size() != 1) {
        return;
    }
    const Run* ellipsis = impl->lines()[0].ellipsis();
    REPORTER_ASSERT(reporter, ellipsis && ellipsis->size() == 1 && ellipsis->isResolved());
    REPORTER_ASSERT(reporter,
                    paragraph->getLongestLine() <= 150,
                    "ellipsized line is %g wide",
                    paragraph->getLongestLine());
}

// Truncated UTF-8 gives a CFString exactly as long as CoreTextIndexMap counts, one U+FFFD per
// invalid byte, so every CFRange still lands on its text; a mismatch would shift all runs
DEF_TEST(CoreTextPipeline_InvalidUtf8MatchesIndexMap, reporter) {
    // A truncated three-byte sequence followed by ASCII
    const char text[] = "\xE2\x82" "a";
    const size_t bytes = sizeof(text) - 1;
    auto fontCollection = sk_make_sp<FontCollection>();
    fontCollection->setDefaultFontManager(ToolUtils::TestFontMgr());
    CoreTextAttrStringBuilder attrBuilder(text, bytes, TextDirection::kLtr, fontCollection);
    attrBuilder.addStyle(TextRange(0, bytes), TextStyle());
    auto attr = attrBuilder.build();
    REPORTER_ASSERT(reporter, attr != nullptr);
    REPORTER_ASSERT(reporter, SkToSizeT(CFAttributedStringGetLength(attr.get())) ==
                                      attrBuilder.indexMap().toUtf16Index(bytes));

    ParagraphStyle style;
    style.setTextShapingEngine(TextShapingEngine::kCoreText);
    ParagraphBuilderImpl builder(style, fontCollection, get_unicode(style));
    builder.pushStyle(TextStyle());
    builder.addText(text, bytes);
    auto paragraph = builder.Build();
    paragraph->layout(500);
    REPORTER_ASSERT(reporter, static_cast<ParagraphImpl*>(paragraph.get())->isCoreTextShaped());
}

namespace {
// Forwards to a real manager and counts every family or typeface query, so a test can prove a
// code path never consults it
class CountingFontMgr final : public SkFontMgr {
public:
    explicit CountingFontMgr(sk_sp<SkFontMgr> fontMgr) : fFontMgr(std::move(fontMgr)) {}
    int queries() const { return fQueries; }

protected:
    int onCountFamilies() const override {
        ++fQueries;
        return fFontMgr->countFamilies();
    }
    void onGetFamilyName(int index, SkString* familyName) const override {
        ++fQueries;
        fFontMgr->getFamilyName(index, familyName);
    }
    sk_sp<SkFontStyleSet> onCreateStyleSet(int index) const override {
        ++fQueries;
        return fFontMgr->createStyleSet(index);
    }
    sk_sp<SkFontStyleSet> onMatchFamily(const char familyName[]) const override {
        ++fQueries;
        return fFontMgr->matchFamily(familyName);
    }
    sk_sp<SkTypeface> onMatchFamilyStyle(const char familyName[],
                                         const SkFontStyle& style) const override {
        ++fQueries;
        return fFontMgr->matchFamilyStyle(familyName, style);
    }
    sk_sp<SkTypeface> onMatchFamilyStyleCharacter(const char familyName[],
                                                  const SkFontStyle& style,
                                                  const char* bcp47[],
                                                  int bcp47Count,
                                                  SkUnichar character) const override {
        ++fQueries;
        return fFontMgr->matchFamilyStyleCharacter(
                familyName, style, bcp47, bcp47Count, character);
    }
    sk_sp<SkTypeface> onMakeFromData(sk_sp<SkData> data, int ttcIndex) const override {
        ++fQueries;
        return fFontMgr->makeFromData(std::move(data), ttcIndex);
    }
    sk_sp<SkTypeface> onMakeFromStreamIndex(std::unique_ptr<SkStreamAsset> stream,
                                            int ttcIndex) const override {
        ++fQueries;
        return fFontMgr->makeFromStream(std::move(stream), ttcIndex);
    }
    sk_sp<SkTypeface> onMakeFromStreamArgs(std::unique_ptr<SkStreamAsset> stream,
                                           const SkFontArguments& args) const override {
        ++fQueries;
        return fFontMgr->makeFromStream(std::move(stream), args);
    }
    sk_sp<SkTypeface> onMakeFromFile(const char path[], int ttcIndex) const override {
        ++fQueries;
        return fFontMgr->makeFromFile(path, ttcIndex);
    }
    sk_sp<SkTypeface> onLegacyMakeTypeface(const char familyName[],
                                           SkFontStyle style) const override {
        ++fQueries;
        return fFontMgr->legacyMakeTypeface(familyName, style);
    }

private:
    sk_sp<SkFontMgr> fFontMgr;
    mutable int fQueries = 0;
};
}  // namespace

// System families, the cascade for CJK, Arabic and emoji, the strut, empty-line metrics and the
// ellipsis all resolve through CoreText, so the default manager is never asked anything
DEF_TEST(CoreTextPipeline_DefaultFontManagerIgnored, reporter) {
    if (!has_font_families({"Helvetica", "PingFang SC", "Apple Color Emoji"})) {
        return;
    }
    auto counting = sk_make_sp<CountingFontMgr>(ToolUtils::TestFontMgr());
    auto fontCollection = sk_make_sp<FontCollection>();
    fontCollection->setDefaultFontManager(counting);
    fontCollection->enableFontFallback();

    ParagraphStyle paragraphStyle;
    paragraphStyle.setTextShapingEngine(TextShapingEngine::kCoreText);
    paragraphStyle.setMaxLines(1);
    paragraphStyle.setEllipsis(u"\u2026");
    StrutStyle strut;
    strut.setStrutEnabled(true);
    strut.setFontFamilies({SkString("Helvetica")});
    strut.setFontSize(16.0f);
    paragraphStyle.setStrutStyle(strut);

    TextStyle style;
    style.setFontFamilies({SkString("Helvetica")});
    style.setFontSize(16.0f);
    ParagraphBuilderImpl builder(paragraphStyle, fontCollection, get_unicode(paragraphStyle));
    builder.pushStyle(style);
    builder.addText("Latin \xE4\xB8\xAD\xE6\x96\x87 \xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A "
                    "\xF0\x9F\x98\x80 and enough trailing text to need an ellipsis\n\n");
    builder.pop();
    auto paragraph = builder.Build();
    paragraph->layout(200.0f);

    auto impl = static_cast<ParagraphImpl*>(paragraph.get());
    REPORTER_ASSERT(reporter, impl->isCoreTextShaped());
    REPORTER_ASSERT(reporter, paragraph->didExceedMaxLines());
    REPORTER_ASSERT(reporter, impl->unresolvedGlyphs() == 0);
    REPORTER_ASSERT(reporter, counting->queries() == 0, "queries=%d", counting->queries());
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

// Empty fontFamilies with an unresolved glyph (e.g. U+10FFFF hitting LastResort) must bind the
// default system typeface instead of dereferencing a null primary SkTypeface, and keep
// unresolvedGlyphs() and unresolvedCodepoints() synchronized across control/format characters
DEF_TEST(CoreTextPipeline_EmptyFontFamilies_LastResortAndUnresolvedSync, reporter) {
    auto fc = test_font_collection();
    ParagraphStyle ps;
    ps.setTextShapingEngine(TextShapingEngine::kCoreText);
    ps.setFakeMissingFontStyles(true);

    TextStyle ts;
    ts.setFontFamilies({});
    ts.setFontSize(16.0f);
    ts.setFontStyle(SkFontStyle::Bold());

    ParagraphBuilderImpl builder(ps, fc, get_unicode(ps));
    builder.pushStyle(ts);
    // U+10FFFF is a noncharacter guaranteed to fall through to LastResort, surrounded by ZWJ/ZWNJ
    // and newline controls that must not be recorded in unresolvedCodepoints
    builder.addText("A\n\xE2\x80\x8D\xF4\x8F\xBF\xBF\xE2\x80\x8B");
    builder.pop();

    auto p = builder.Build();
    p->layout(500.0f);

    REPORTER_ASSERT(reporter, p->unresolvedGlyphs() == 1, "unresolved=%d", p->unresolvedGlyphs());
    auto unresolved = p->unresolvedCodepoints();
    REPORTER_ASSERT(reporter, unresolved.size() == 1, "size=%zu", unresolved.size());
    REPORTER_ASSERT(reporter, unresolved.count(0x10FFFF) == 1);
    REPORTER_ASSERT(reporter, unresolved.count('\n') == 0);
    REPORTER_ASSERT(reporter, unresolved.count(0x200D) == 0);
    REPORTER_ASSERT(reporter, unresolved.count(0x200B) == 0);
}

// Multi-run ellipsis (e.g. Latin + Emoji or fallback characters) must coalesce into a single
// RunHandler buffer without triggering SkASSERT(!fRun) in TextLine::ShapeString
DEF_TEST(CoreTextPipeline_MultiRunEllipsisAndAdjacentStyleRuns, reporter) {
    if (!has_font_families({"Helvetica", "Apple Color Emoji"})) {
        return;
    }
    auto fc = test_font_collection();
    ParagraphStyle ps;
    ps.setTextShapingEngine(TextShapingEngine::kCoreText);
    ps.setMaxLines(1);
    ps.setEllipsis(SkString("...\xF0\x9F\x98\x80"));

    ParagraphBuilderImpl builder(ps, fc, get_unicode(ps));
    TextStyle regular;
    regular.setFontFamilies({SkString("Helvetica")});
    regular.setFontSize(18.0f);
    regular.setFontEdging(SkFont::Edging::kAlias);
    builder.pushStyle(regular);
    builder.addText("First span ");
    builder.pop();

    TextStyle subpixelBold = regular;
    subpixelBold.setFontEdging(SkFont::Edging::kSubpixelAntiAlias);
    builder.pushStyle(subpixelBold);
    builder.addText("Second span that is long enough to trigger ellipsis truncation");
    builder.pop();

    auto p = builder.Build();
    p->layout(140.0f);

    auto impl = static_cast<ParagraphImpl*>(p.get());
    REPORTER_ASSERT(reporter, impl->isCoreTextShaped());
    REPORTER_ASSERT(reporter, p->didExceedMaxLines());
    REPORTER_ASSERT(reporter, impl->runs().size() >= 2);
    if (impl->runs().size() >= 2) {
        REPORTER_ASSERT(reporter, impl->runs()[0].font().getEdging() == SkFont::Edging::kAlias);
        REPORTER_ASSERT(reporter,
                        impl->runs()[1].font().getEdging() ==
                                SkFont::Edging::kSubpixelAntiAlias);
    }
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
    for (const auto& run : impl->runs()) {
        for (size_t k = 1; k <= run.size(); ++k) {
            if (run.leftToRight()) {
                REPORTER_ASSERT(reporter, run.clusterIndexes()[k] >= run.clusterIndexes()[k - 1]);
            }
        }
    }
    auto boxes = p->getRectsForRange(0, 3, RectHeightStyle::kTight, RectWidthStyle::kTight);
    REPORTER_ASSERT(reporter, !boxes.empty());
}

// CoreText places trailing whitespace in an RTL CTLine at negative X coordinates
// [-CTLineGetTrailingWhitespaceWidth, 0), so visualOriginX must start there to keep the last
// glyph's fPositions[k] consistent with the fPositions[glyphCount] advance sentinel
DEF_TEST(CoreTextPipeline_RtlTrailingWhitespaceClusterWidthAndUserOnlyFontCollection, reporter) {
    if (!has_font_families({"Helvetica", "Apple Color Emoji"})) {
        return;
    }
    auto fc = test_font_collection();
    ParagraphStyle ps;
    ps.setTextShapingEngine(TextShapingEngine::kCoreText);
    ps.setTextDirection(TextDirection::kRtl);

    // "אבג" vs "אבג    " (3 Hebrew letters + 4 trailing spaces)
    const char* rtlNoSpaces = "\xD7\x90\xD7\x91\xD7\x92";
    const char* rtlTrailingSpaces = "\xD7\x90\xD7\x91\xD7\x92    ";
    TextStyle ts;
    ts.setFontFamilies({SkString("Helvetica")});
    ts.setFontSize(14.0f);

    ParagraphBuilderImpl baseBuilder(ps, fc, get_unicode(ps));
    baseBuilder.pushStyle(ts);
    baseBuilder.addText(rtlNoSpaces);
    baseBuilder.pop();
    auto baseP = baseBuilder.Build();
    baseP->layout(500.0f);

    ParagraphBuilderImpl builder(ps, fc, get_unicode(ps));
    builder.pushStyle(ts);
    builder.addText(rtlTrailingSpaces);
    builder.pop();
    auto p = builder.Build();
    p->layout(500.0f);

    auto impl = static_cast<ParagraphImpl*>(p.get());
    REPORTER_ASSERT(reporter, impl->isCoreTextShaped());
    Paragraph::GlyphClusterInfo baseFirstCluster;
    Paragraph::GlyphClusterInfo firstCluster;
    REPORTER_ASSERT(reporter, baseP->getGlyphClusterAt(0, &baseFirstCluster));
    REPORTER_ASSERT(reporter, p->getGlyphClusterAt(0, &firstCluster));
    REPORTER_ASSERT(reporter,
                    SkScalarNearlyEqual(firstCluster.fBounds.width(),
                                        baseFirstCluster.fBounds.width(),
                                        0.01f),
                    "RTL first cluster width %f != no-trailing-spaces width %f",
                    firstCluster.fBounds.width(),
                    baseFirstCluster.fBounds.width());

    // When a client registers user fonts (AssetFontManager) without setting a defaultFontManager,
    // matchFamily must not resolve system families ahead of registered user families
    sk_sp<SkTypeface> emojiFace =
            ToolUtils::TestFontMgr()->matchFamilyStyle("Apple Color Emoji", SkFontStyle());
    REPORTER_ASSERT(reporter, emojiFace != nullptr);
    if (emojiFace) {
        auto assetProvider = sk_make_sp<TypefaceFontProvider>();
        assetProvider->registerTypeface(emojiFace, SkString("Apple Color Emoji"));
        auto userOnlyFc = sk_make_sp<FontCollection>();
        userOnlyFc->setAssetFontManager(assetProvider);

        auto matched = CoreTextFontRegistry::Instance().findTypefaces(
                *userOnlyFc,
                {SkString(".AppleSystemUIFont"), SkString("Apple Color Emoji")},
                SkFontStyle::Normal(),
                std::nullopt);
        REPORTER_ASSERT(reporter, matched.size() == 1, "matched=%zu", matched.size());
        if (matched.size() == 1) {
            SkString family;
            matched[0]->getFamilyName(&family);
            REPORTER_ASSERT(reporter,
                            family.equals("Apple Color Emoji"),
                            "expected Apple Color Emoji, got '%s'",
                            family.c_str());
        }
    }
}

// Flutter 130492: system UI italic faces (.SFNS-RegularItalic, .NewYork-RegularItalic,
// .SFNSMono-RegularItalic) set kCTFontTraitItalic on the CTFont but omit kCTFontSlantTrait from
// CTFontCopyFontDescriptor, so SkTypeface::fontStyle() reports upright; CoreTextLineShaper must
// query the bound CTFont directly instead of stacking synthetic -0.25 skewX on top of real italic
DEF_TEST(CoreTextPipeline_SystemFontItalic_SingleSlant_130492, reporter) {
    if (!has_font_families({".AppleSystemUIFont"})) {
        return;
    }
    auto fc = test_font_collection();
    const char* families[] = {
            ".AppleSystemUIFont",
            "CupertinoSystemText",
            ".AppleSystemUIFontSerif",
            ".AppleSystemUIFontMonospaced",
    };

    for (const char* family : families) {
        for (SkFontStyle reqStyle : {SkFontStyle::Italic(), SkFontStyle::BoldItalic()}) {
            ParagraphStyle ps;
            ps.setTextShapingEngine(TextShapingEngine::kCoreText);
            ps.setFakeMissingFontStyles(true);
            ParagraphBuilderImpl builder(ps, fc, get_unicode(ps));
            TextStyle ts;
            ts.setFontFamilies({SkString(family)});
            ts.setFontSize(24.0f);
            ts.setFontStyle(reqStyle);
            builder.pushStyle(ts);
            builder.addText("System Italic Slant Check");
            builder.pop();

            auto p = builder.Build();
            p->layout(1000.0f);
            auto impl = static_cast<ParagraphImpl*>(p.get());
            REPORTER_ASSERT(reporter, impl->isCoreTextShaped());
            REPORTER_ASSERT(reporter, impl->runs().size() == 1);
            if (impl->runs().size() != 1) {
                continue;
            }
            const SkFont& font = impl->runs()[0].font();
            REPORTER_ASSERT(reporter,
                            font.getSkewX() == 0.0f,
                            "%s (weight=%d) double-slanted with synthetic skewX=%g",
                            family,
                            reqStyle.weight(),
                            font.getSkewX());
            REPORTER_ASSERT(reporter,
                            !font.isEmbolden(),
                            "%s (weight=%d) unexpectedly emboldened",
                            family,
                            reqStyle.weight());
        }
    }

    // Ellipsis shaped via CoreTextLineShaper::ShapeAffix with .AppleSystemUIFont Italic must also
    // keep skewX == 0 instead of double-slanting the ellipsis run
    ParagraphStyle ellPs;
    ellPs.setTextShapingEngine(TextShapingEngine::kCoreText);
    ellPs.setFakeMissingFontStyles(true);
    ellPs.setMaxLines(1);
    ellPs.setEllipsis(SkString("..."));
    TextStyle ellTs;
    ellTs.setFontFamilies({SkString(".AppleSystemUIFont")});
    ellTs.setFontSize(20.0f);
    ellTs.setFontStyle(SkFontStyle::Italic());
    ellPs.setTextStyle(ellTs);
    ParagraphBuilderImpl ellBuilder(ellPs, fc, get_unicode(ellPs));
    ellBuilder.pushStyle(ellTs);
    ellBuilder.addText("System italic line that is long enough to truncate with an ellipsis");
    ellBuilder.pop();
    auto ellP = ellBuilder.Build();
    ellP->layout(120.0f);
    auto ellImpl = static_cast<ParagraphImpl*>(ellP.get());
    REPORTER_ASSERT(reporter, ellP->didExceedMaxLines());
    REPORTER_ASSERT(reporter, !ellImpl->lines().empty());
    if (!ellImpl->lines().empty() && ellImpl->lines()[0].ellipsis()) {
        REPORTER_ASSERT(reporter, ellImpl->lines()[0].ellipsis()->font().getSkewX() == 0.0f);
    }

    // When .AppleSystemUIFont Italic is a secondary fallback family in kCTFontCascadeListAttribute,
    // CoreTextFontRegistry::getDescriptor must preserve its italic trait rather than reading the
    // upright-reported SkTypeface::fontStyle()
    if (has_font_families({"Apple Color Emoji"})) {
        ParagraphStyle fbPs;
        fbPs.setTextShapingEngine(TextShapingEngine::kCoreText);
        fbPs.setFakeMissingFontStyles(true);
        ParagraphBuilderImpl fbBuilder(fbPs, fc, get_unicode(fbPs));
        TextStyle fbTs;
        fbTs.setFontFamilies({SkString("Apple Color Emoji"), SkString(".AppleSystemUIFont")});
        fbTs.setFontSize(24.0f);
        fbTs.setFontStyle(SkFontStyle::Italic());
        fbBuilder.pushStyle(fbTs);
        fbBuilder.addText("FallbackItalic");
        fbBuilder.pop();
        auto fbP = fbBuilder.Build();
        fbP->layout(1000.0f);
        auto fbImpl = static_cast<ParagraphImpl*>(fbP.get());
        REPORTER_ASSERT(reporter, fbImpl->runs().size() == 1);
        if (fbImpl->runs().size() == 1) {
            REPORTER_ASSERT(reporter,
                            fbImpl->runs()[0].font().getSkewX() == 0.0f,
                            "fallback .AppleSystemUIFont Italic got synthetic skewX=%g",
                            fbImpl->runs()[0].font().getSkewX());
        }
    }
}

// Adjacent Blocks must shape within a single CTLine so contextual Arabic joining and RTL
// mid-line spaces work across style boundaries, while CoreText splits CTRuns whenever Run-level
// font or metric properties differ (including when both blocks fall back to LastResort)
DEF_TEST(CoreTextPipeline_AdjacentBlocks_SingleCTLineShapingAndRunIsolation, reporter) {
    if (!has_font_families({"Geeza Pro", "Helvetica", "Courier"})) {
        return;
    }

    // Contextual Arabic joining across two Blocks with different baselineShift and fontEdging
    {
        auto fc = test_font_collection();
        ParagraphStyle ps;
        ps.setTextShapingEngine(TextShapingEngine::kCoreText);
        ps.setTextDirection(TextDirection::kRtl);

        TextStyle baseStyle;
        baseStyle.setFontFamilies({SkString("Geeza Pro")});
        baseStyle.setFontSize(24.0f);

        // "مرحبا" = 5 Arabic letters (10 UTF-8 bytes), split after "مرح" (6 bytes)
        const char* fullWord = "\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7";
        const char* part0 = "\xD9\x85\xD8\xB1\xD8\xAD";
        const char* part1 = "\xD8\xA8\xD8\xA7";

        ParagraphBuilderImpl singleBuilder(ps, fc, get_unicode(ps));
        singleBuilder.pushStyle(baseStyle);
        singleBuilder.addText(fullWord);
        singleBuilder.pop();
        auto pSingle = singleBuilder.Build();
        pSingle->layout(1000.0f);

        TextStyle style0 = baseStyle;
        style0.setFontEdging(SkFont::Edging::kAntiAlias);
        style0.setBaselineShift(0.0f);
        TextStyle style1 = baseStyle;
        style1.setFontEdging(SkFont::Edging::kSubpixelAntiAlias);
        style1.setBaselineShift(-4.0f);

        ParagraphBuilderImpl splitBuilder(ps, fc, get_unicode(ps));
        splitBuilder.pushStyle(style0);
        splitBuilder.addText(part0);
        splitBuilder.pop();
        splitBuilder.pushStyle(style1);
        splitBuilder.addText(part1);
        splitBuilder.pop();
        auto pSplit = splitBuilder.Build();
        pSplit->layout(1000.0f);

        auto implSingle = static_cast<ParagraphImpl*>(pSingle.get());
        auto implSplit = static_cast<ParagraphImpl*>(pSplit.get());
        REPORTER_ASSERT(reporter, implSingle->runs().size() == 1);
        REPORTER_ASSERT(reporter,
                        implSplit->runs().size() == 2,
                        "expected 2 isolated runs, got %zu",
                        implSplit->runs().size());
        REPORTER_ASSERT(reporter,
                        SkScalarNearlyEqual(pSplit->getMaxIntrinsicWidth(),
                                            pSingle->getMaxIntrinsicWidth(),
                                            0.05f),
                        "split Arabic width %f != joined single-run width %f",
                        pSplit->getMaxIntrinsicWidth(),
                        pSingle->getMaxIntrinsicWidth());
        if (implSingle->runs().size() == 1 && implSplit->runs().size() == 2) {
            REPORTER_ASSERT(reporter,
                            implSplit->runs()[0].font().getEdging() == SkFont::Edging::kAntiAlias);
            REPORTER_ASSERT(reporter,
                            implSplit->runs()[1].font().getEdging() ==
                                    SkFont::Edging::kSubpixelAntiAlias);
            REPORTER_ASSERT(reporter, implSplit->runs()[0].baselineShift() == 0.0f);
            REPORTER_ASSERT(reporter, implSplit->runs()[1].baselineShift() == -4.0f);
            // Boundary letters Hah (end of run 0, visual glyph 0) and Beh (start of run 1, visual
            // glyph 1) must match the joined medial glyph IDs from the unsplit word
            REPORTER_ASSERT(reporter,
                            implSplit->runs()[1].glyphs()[1] == implSingle->runs()[0].glyphs()[1],
                            "Beh glyph %u != medial Beh %u",
                            implSplit->runs()[1].glyphs()[1],
                            implSingle->runs()[0].glyphs()[1]);
            REPORTER_ASSERT(reporter,
                            implSplit->runs()[0].glyphs()[0] == implSingle->runs()[0].glyphs()[2],
                            "Hah glyph %u != medial Hah %u",
                            implSplit->runs()[0].glyphs()[0],
                            implSingle->runs()[0].glyphs()[2]);
        }
    }

    // RTL mid-line space at the end of Block 0 must not be treated as line-trailing whitespace
    // that downgrades the space to LTR (bidiLevel 0) and inverts the visual word order
    {
        auto fc = test_font_collection();
        ParagraphStyle ps;
        ps.setTextShapingEngine(TextShapingEngine::kCoreText);
        ps.setTextDirection(TextDirection::kRtl);

        TextStyle style0;
        style0.setFontFamilies({SkString(".AppleSystemUIFont")});
        style0.setFontSize(20.0f);
        style0.setFontEdging(SkFont::Edging::kAlias);
        TextStyle style1 = style0;
        style1.setFontEdging(SkFont::Edging::kAntiAlias);

        // "שלום עולם" split into "שלום " (first word, visually right) +
        // "עולם" (second word, visually left)
        const char* rtlPart0 = "\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D ";
        const char* rtlPart1 = "\xD7\xA2\xD7\x95\xD7\x9C\xD7\x9D";

        ParagraphBuilderImpl splitBuilder(ps, fc, get_unicode(ps));
        splitBuilder.pushStyle(style0);
        splitBuilder.addText(rtlPart0);
        splitBuilder.pop();
        splitBuilder.pushStyle(style1);
        splitBuilder.addText(rtlPart1);
        splitBuilder.pop();
        auto pSplit = splitBuilder.Build();
        pSplit->layout(1000.0f);

        auto boxesWord0 =
                pSplit->getRectsForRange(0, 4, RectHeightStyle::kTight, RectWidthStyle::kTight);
        auto boxesWord1 =
                pSplit->getRectsForRange(5, 9, RectHeightStyle::kTight, RectWidthStyle::kTight);
        REPORTER_ASSERT(reporter, !boxesWord0.empty() && !boxesWord1.empty());
        if (!boxesWord0.empty() && !boxesWord1.empty()) {
            REPORTER_ASSERT(reporter,
                            boxesWord0[0].rect.fLeft > boxesWord1[0].rect.fLeft,
                            "RTL word 0 ('שלום', left=%f) should be visually right of word 1 "
                            "('עולם', left=%f)",
                            boxesWord0[0].rect.fLeft,
                            boxesWord1[0].rect.fLeft);
        }
    }

    // Adjacent Blocks differing only in paint attributes (color) coalesce into a single Run,
    // while Blocks with disableFontFallback() and different fontFamilies stay isolated
    {
        auto fc = test_font_collection();
        ParagraphStyle ps;
        ps.setTextShapingEngine(TextShapingEngine::kCoreText);

        TextStyle redStyle;
        redStyle.setFontFamilies({SkString("Helvetica")});
        redStyle.setFontSize(20.0f);
        redStyle.setColor(SK_ColorRED);
        TextStyle blueStyle = redStyle;
        blueStyle.setColor(SK_ColorBLUE);

        ParagraphBuilderImpl colorBuilder(ps, fc, get_unicode(ps));
        colorBuilder.pushStyle(redStyle);
        colorBuilder.addText("Hello ");
        colorBuilder.pop();
        colorBuilder.pushStyle(blueStyle);
        colorBuilder.addText("World");
        colorBuilder.pop();
        auto pColor = colorBuilder.Build();
        pColor->layout(1000.0f);
        auto implColor = static_cast<ParagraphImpl*>(pColor.get());
        REPORTER_ASSERT(reporter,
                        implColor->runs().size() == 1,
                        "paint-only adjacent blocks should coalesce into 1 Run, got %zu",
                        implColor->runs().size());

        // With fallback disabled installed families are hidden, so both faces are user faces
        sk_sp<SkTypeface> roboto =
                ToolUtils::CreateTypefaceFromResource("fonts/Roboto-Regular.ttf");
        sk_sp<SkTypeface> dejaVu =
                ToolUtils::CreateTypefaceFromResource("fonts/DejaVuSans.subset.ttf");
        REPORTER_ASSERT(reporter, roboto && dejaVu);
        auto provider = sk_make_sp<TypefaceFontProvider>();
        provider->registerTypeface(roboto, SkString("FaceA"));
        provider->registerTypeface(dejaVu, SkString("FaceB"));
        fc->setDynamicFontManager(provider);
        fc->disableFontFallback();
        SkString famA;
        SkString famB;
        roboto->getFamilyName(&famA);
        dejaVu->getFamilyName(&famB);
        TextStyle styleA;
        styleA.setFontFamilies({SkString("FaceA")});
        styleA.setFontSize(20.0f);
        TextStyle styleB;
        styleB.setFontFamilies({SkString("FaceB")});
        styleB.setFontSize(20.0f);

        ParagraphBuilderImpl builder(ps, fc, get_unicode(ps));
        builder.pushStyle(styleA);
        builder.addText("\xF4\x8F\xBF\xBF");
        builder.pop();
        builder.pushStyle(styleB);
        builder.addText("\xF4\x8F\xBF\xBF");
        builder.pop();

        auto p = builder.Build();
        p->layout(1000.0f);
        auto impl = static_cast<ParagraphImpl*>(p.get());
        REPORTER_ASSERT(reporter, p->unresolvedGlyphs() == 2);
        REPORTER_ASSERT(reporter,
                        impl->runs().size() == 2,
                        "expected 2 unresolved runs for FaceA and FaceB, got %zu",
                        impl->runs().size());
        if (impl->runs().size() == 2) {
            SkString fam0;
            SkString fam1;
            impl->runs()[0].font().getTypeface()->getFamilyName(&fam0);
            impl->runs()[1].font().getTypeface()->getFamilyName(&fam1);
            REPORTER_ASSERT(reporter,
                            fam0 == famA,
                            "run 0 expected %s, got '%s'",
                            famA.c_str(),
                            fam0.c_str());
            REPORTER_ASSERT(reporter,
                            fam1 == famB,
                            "run 1 expected %s, got '%s'",
                            famB.c_str(),
                            fam1.c_str());
        }
    }
}

namespace {
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

struct PlainLine {
    std::vector<SkScalar> x;
    std::vector<SkScalar> advances;
    SkScalar width = 0;
};

// Tracking for a UTF-16 range of a coretext_line, on top of the tracking of the whole line
struct TrackingSpan {
    CFRange range;
    SkScalar tracking;
};

// Glyph x and width CoreText itself draws for text on one CTLine in font. A nonzero tracking goes
// in as spacingAttribute: kCTTrackingAttributeName as NSTrackingAttributeName (fonts keep their
// trak, tracking goes after each grapheme), or kCTKernAttributeName as NSKernAttributeName.
// Without a direction the line takes the paragraph direction of its first strong character
PlainLine coretext_line(CTFontRef font,
                        const char* text,
                        SkScalar tracking,
                        std::optional<TextDirection> direction = std::nullopt,
                        std::initializer_list<TrackingSpan> spans = {},
                        CFStringRef spacingAttribute = kCTTrackingAttributeName) {
    SkUniqueCFRef<CFStringRef> string(
            CFStringCreateWithCString(nullptr, text, kCFStringEncodingUTF8));
    SkUniqueCFRef<CFMutableAttributedStringRef> attrString(
            CFAttributedStringCreateMutable(nullptr, 0));
    CFAttributedStringReplaceString(attrString.get(), CFRangeMake(0, 0), string.get());
    const CFRange all = CFRangeMake(0, CFStringGetLength(string.get()));
    CFAttributedStringSetAttribute(attrString.get(), all, kCTFontAttributeName, font);
    auto setTracking = [&](CFRange range, SkScalar value) {
        const double trackingValue = value;
        SkUniqueCFRef<CFNumberRef> number(
                CFNumberCreate(nullptr, kCFNumberDoubleType, &trackingValue));
        CFAttributedStringSetAttribute(attrString.get(), range, spacingAttribute, number.get());
    };
    if (tracking != 0) {
        setTracking(all, tracking);
    }
    for (const TrackingSpan& span : spans) {
        setTracking(span.range, span.tracking);
    }
    if (direction) {
        CTWritingDirection writing = *direction == TextDirection::kRtl
                                             ? kCTWritingDirectionRightToLeft
                                             : kCTWritingDirectionLeftToRight;
        CTParagraphStyleSetting setting = {
                kCTParagraphStyleSpecifierBaseWritingDirection, sizeof(writing), &writing};
        SkUniqueCFRef<CTParagraphStyleRef> paragraphStyle(CTParagraphStyleCreate(&setting, 1));
        CFAttributedStringSetAttribute(
                attrString.get(), all, kCTParagraphStyleAttributeName, paragraphStyle.get());
    }
    SkUniqueCFRef<CTLineRef> line(CTLineCreateWithAttributedString(attrString.get()));
    PlainLine result;
    result.width = static_cast<SkScalar>(
            CTLineGetTypographicBounds(line.get(), nullptr, nullptr, nullptr));
    CFArrayRef runs = CTLineGetGlyphRuns(line.get());
    for (CFIndex i = 0; i < CFArrayGetCount(runs); ++i) {
        CTRunRef run = (CTRunRef)CFArrayGetValueAtIndex(runs, i);
        const CFIndex count = CTRunGetGlyphCount(run);
        std::vector<CGPoint> positions(count);
        std::vector<CGSize> advances(count);
        CTRunGetPositions(run, CFRangeMake(0, count), positions.data());
        CTRunGetAdvances(run, CFRangeMake(0, count), advances.data());
        for (CFIndex k = 0; k < count; ++k) {
            result.x.push_back(static_cast<SkScalar>(positions[k].x));
            result.advances.push_back(static_cast<SkScalar>(advances[k].width));
        }
    }
    return result;
}

// Glyph x and width CoreText itself draws for text on one CTLine at 20pt
PlainLine plain_coretext_line(const char* family, const char* text) {
    SkUniqueCFRef<CFStringRef> name(
            CFStringCreateWithCString(nullptr, family, kCFStringEncodingUTF8));
    SkUniqueCFRef<CTFontRef> font(CTFontCreateWithName(name.get(), 20, nullptr));
    return coretext_line(font.get(), text, 0);
}

struct CaretStep {
    SkScalar x;
    int position;
    int affinity;
};

// Hit-test results along the line, one entry where the result changes
std::vector<CaretStep> caret_steps(Paragraph* paragraph) {
    std::vector<CaretStep> steps;
    const int count = SkScalarCeilToInt(paragraph->getMaxIntrinsicWidth() * 4) + 8;
    for (int i = 0; i < count; ++i) {
        const SkScalar x = (i + 0.5f) / 4;
        PositionWithAffinity hit = paragraph->getGlyphPositionAtCoordinate(x, 10);
        const int affinity = static_cast<int>(hit.affinity);
        if (steps.empty() || steps.back().position != hit.position ||
            steps.back().affinity != affinity) {
            steps.push_back({x, hit.position, affinity});
        }
    }
    return steps;
}

struct DrawnGlyph {
    SkScalar drawn;
    bool mark;
    size_t cluster;
};

// Glyphs in the order and at the x they are painted; Run positions follow the logical run order,
// CTLine glyphs the visual one. Marks are glyphs off a grapheme start, which only holds for fonts
// that draw each mark from its own code point: dots a Nastaliq font splits off its base, a reph
// or a pre-base matra start a grapheme and count as spacing here
std::vector<DrawnGlyph> drawn_glyphs(ParagraphImpl* impl) {
    std::vector<DrawnGlyph> glyphs;
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
    const std::vector<DrawnGlyph> glyphs = drawn_glyphs(impl);
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

// SkParagraph walks the pen in 32-bit float while CoreText sums CGFloat advances; 1/4096pt absorbs
// float accumulation across the glyphs of these test lines
constexpr SkScalar kTrackedLineTolerance = 1.0f / 4096;

// Checks a one-line kCoreText paragraph against the CTLine native tracking gives for the same
// text: the same width and each glyph drawn at the same x. TextLine draws a letter-spaced line
// half the letterSpacing of its first cluster to the right (Cluster::getHalfLetterSpacing), a
// Flutter convention both engines keep, so the glyphs are compared from there
void expect_drawn_as_tracked_line(skiatest::Reporter* reporter,
                                  const char* label,
                                  ParagraphImpl* impl,
                                  SkScalar firstLetterSpacing,
                                  const PlainLine& native) {
    REPORTER_ASSERT(reporter, impl->isCoreTextShaped() && impl->lines().size() == 1, "%s", label);
    if (impl->lines().size() != 1) {
        return;
    }
    // Intrinsic widths are rounded to 0.01, the line width is not
    const SkScalar width = impl->lines()[0].width();
    REPORTER_ASSERT(reporter,
                    SkScalarNearlyEqual(width, native.width, kTrackedLineTolerance),
                    "%s: width %.5f, native tracking %.5f",
                    label,
                    width,
                    native.width);
    const std::vector<DrawnGlyph> glyphs = drawn_glyphs(impl);
    REPORTER_ASSERT(reporter,
                    glyphs.size() == native.x.size(),
                    "%s: %zu glyphs, native tracking %zu",
                    label,
                    glyphs.size(),
                    native.x.size());
    for (size_t k = 0; k < std::min(glyphs.size(), native.x.size()); ++k) {
        const SkScalar x = glyphs[k].drawn - firstLetterSpacing / 2;
        REPORTER_ASSERT(reporter,
                        SkScalarNearlyEqual(x, native.x[k], kTrackedLineTolerance),
                        "%s glyph %zu: drawn at %.5f, native tracking %.5f",
                        label,
                        k,
                        x,
                        native.x[k]);
    }
}

// A copy of font without its trak. The private attribute only builds the trak-free references
// that prove a test can tell a line that keeps the trak from one that drops it
SkUniqueCFRef<CTFontRef> untracked_copy(CTFontRef font) {
    int zero = 0;
    SkUniqueCFRef<CFNumberRef> zeroNumber(CFNumberCreate(nullptr, kCFNumberIntType, &zero));
    const void* keys[] = {CFSTR("NSCTFontUnscaledTrackingAttribute")};
    const void* values[] = {zeroNumber.get()};
    SkUniqueCFRef<CFDictionaryRef> attributes(CFDictionaryCreate(nullptr,
                                                                 keys,
                                                                 values,
                                                                 1,
                                                                 &kCFTypeDictionaryKeyCallBacks,
                                                                 &kCFTypeDictionaryValueCallBacks));
    SkUniqueCFRef<CTFontDescriptorRef> descriptor(
            CTFontDescriptorCreateWithAttributes(attributes.get()));
    return SkUniqueCFRef<CTFontRef>(
            CTFontCreateCopyWithAttributes(font, 0, nullptr, descriptor.get()));
}

// The fonts CoreText draws text with on one CTLine in primary, and the line widths once the
// trak is taken out of the primary font only or out of the fallback fonts only. A trak-free
// primary does not reach the cascade, so every run gets its own trak-free copy
struct UntrackedLine {
    std::vector<SkString> fonts;
    bool hasPrimaryRun = false;
    SkScalar primaryUntrackedWidth = 0;
    SkScalar fallbackUntrackedWidth = 0;
};

UntrackedLine untracked_line(CTFontRef primary, const char* text) {
    SkUniqueCFRef<CFStringRef> string(
            CFStringCreateWithCString(nullptr, text, kCFStringEncodingUTF8));
    SkUniqueCFRef<CFMutableAttributedStringRef> attrString(
            CFAttributedStringCreateMutable(nullptr, 0));
    CFAttributedStringReplaceString(attrString.get(), CFRangeMake(0, 0), string.get());
    CFAttributedStringSetAttribute(attrString.get(),
                                   CFRangeMake(0, CFStringGetLength(string.get())),
                                   kCTFontAttributeName,
                                   primary);
    SkUniqueCFRef<CFMutableAttributedStringRef> primaryUntracked(
            CFAttributedStringCreateMutableCopy(nullptr, 0, attrString.get()));
    SkUniqueCFRef<CFMutableAttributedStringRef> fallbackUntracked(
            CFAttributedStringCreateMutableCopy(nullptr, 0, attrString.get()));
    SkUniqueCFRef<CTLineRef> line(CTLineCreateWithAttributedString(attrString.get()));
    UntrackedLine result;
    CFArrayRef runs = CTLineGetGlyphRuns(line.get());
    for (CFIndex i = 0; i < CFArrayGetCount(runs); ++i) {
        CTRunRef run = (CTRunRef)CFArrayGetValueAtIndex(runs, i);
        CTFontRef font =
                (CTFontRef)CFDictionaryGetValue(CTRunGetAttributes(run), kCTFontAttributeName);
        SkUniqueCFRef<CFStringRef> psName(CTFontCopyPostScriptName(font));
        char name[128] = {};
        CFStringGetCString(psName.get(), name, sizeof(name), kCFStringEncodingUTF8);
        result.fonts.emplace_back(name);
        const bool isPrimary = CFEqual(font, primary);
        result.hasPrimaryRun |= isPrimary;
        SkUniqueCFRef<CTFontRef> untracked = untracked_copy(font);
        CFAttributedStringSetAttribute(isPrimary ? primaryUntracked.get() : fallbackUntracked.get(),
                                       CTRunGetStringRange(run),
                                       kCTFontAttributeName,
                                       untracked.get());
    }
    auto width = [](CFAttributedStringRef attributed) {
        SkUniqueCFRef<CTLineRef> untrackedLine(CTLineCreateWithAttributedString(attributed));
        return static_cast<SkScalar>(
                CTLineGetTypographicBounds(untrackedLine.get(), nullptr, nullptr, nullptr));
    };
    result.primaryUntrackedWidth = width(primaryUntracked.get());
    result.fallbackUntrackedWidth = width(fallbackUntracked.get());
    return result;
}
}  // namespace

// Explicit letterSpacing stacks on the font's trak as NSTrackingAttributeName and
// NSKernAttributeName do, for the system font (positive and negative trak) and fallback fonts
// (PingFang, SF Hebrew, emoji): the line is the CTLine of native tracking, as wide as that of
// native kerning. SkParagraph does not space cursive scripts, see CursiveArabicSystemFonts
DEF_TEST(CoreTextPipeline_LetterSpacing_StacksOnTrakLikeNativeTracking, reporter) {
    if (!has_font_families({".AppleSystemUIFont"})) {
        return;
    }
    struct Case {
        const char* name;
        const char* text;
        TextDirection direction;
        // CoreText draws part of the text in fonts other than the system font
        bool fallback;
    };
    const Case cases[] = {
            {"Latin", "The quick brown fox jumps over the lazy dog", TextDirection::kLtr, false},
            {"PingFang", "Ab \u6C34\u6C34\u6C34", TextDirection::kLtr, true},
            {"Hebrew", "Ab \u05E9\u05DC\u05D5", TextDirection::kLtr, true},
            {"mixed",
             "Hi \u6C34 \U0001F600 \u05E9\u05DC\u05D5\u05DD ok",
             TextDirection::kLtr,
             true},
            {"combining", "ae\u0301\u0323x o\u0308", TextDirection::kLtr, false},
            {"Hebrew RTL",
             "\u05E9\u05DC\u05D5\u05DD \u05E2\u05D5\u05DC\u05DD",
             TextDirection::kRtl,
             true},
            {"Latin first RTL", "ok \u05E9\u05DC\u05D5\u05DD", TextDirection::kRtl, true},
    };
    for (const Case& c : cases) {
        for (SkScalar size : {11.0f, 13.0f, 17.0f, 28.0f, 48.0f}) {
            SkUniqueCFRef<CTFontRef> system(
                    CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, size, nullptr));
            auto unspaced = layout_spaced(".AppleSystemUIFont", size, 0, c.text, c.direction);
            auto* unspacedImpl = static_cast<ParagraphImpl*>(unspaced.get());

            // Without trak in the system font, or in the fallback fonts where the text falls
            // back, a line that drops the trak would match native tracking as well
            const UntrackedLine untracked = untracked_line(system.get(), c.text);
            const SkScalar tracked = coretext_line(system.get(), c.text, 0).width;
            REPORTER_ASSERT(reporter,
                            !untracked.hasPrimaryRun ||
                                    !SkScalarNearlyEqual(untracked.primaryUntrackedWidth,
                                                         tracked,
                                                         kTrackedLineTolerance),
                            "%s %gpt: system font has no trak, %.5f with and without",
                            c.name,
                            size,
                            tracked);
            REPORTER_ASSERT(reporter,
                            !c.fallback || !SkScalarNearlyEqual(untracked.fallbackUntrackedWidth,
                                                                tracked,
                                                                kTrackedLineTolerance),
                            "%s %gpt: fallback fonts have no trak, %.5f with and without",
                            c.name,
                            size,
                            tracked);
            SkUniqueCFRef<CFStringRef> systemPsName(CTFontCopyPostScriptName(system.get()));
            char systemName[128] = {};
            CFStringGetCString(
                    systemPsName.get(), systemName, sizeof(systemName), kCFStringEncodingUTF8);
            int fallbackRuns = 0;
            for (const Run& run : unspacedImpl->runs()) {
                SkString runName;
                run.font().getTypeface()->getPostScriptName(&runName);
                REPORTER_ASSERT(reporter,
                                std::find(untracked.fonts.begin(),
                                          untracked.fonts.end(),
                                          runName) != untracked.fonts.end(),
                                "%s %gpt: run in %s, which CoreText does not pick",
                                c.name,
                                size,
                                runName.c_str());
                fallbackRuns += runName.equals(systemName) ? 0 : 1;
            }
            REPORTER_ASSERT(reporter,
                            (fallbackRuns > 0) == c.fallback,
                            "%s %gpt: %d fallback runs",
                            c.name,
                            size,
                            fallbackRuns);

            for (SkScalar spacing : {-0.41f, 0.0f, 1.0f}) {
                SkString label = SkStringPrintf("%s %gpt spacing %g", c.name, size, spacing);
                auto paragraph =
                        layout_spaced(".AppleSystemUIFont", size, spacing, c.text, c.direction);
                auto* impl = static_cast<ParagraphImpl*>(paragraph.get());
                expect_drawn_as_tracked_line(
                        reporter,
                        label.c_str(),
                        impl,
                        spacing,
                        coretext_line(system.get(), c.text, spacing, c.direction));
                if (impl->lines().size() != 1) {
                    continue;
                }
                // NSKernAttributeName is a second native oracle, apart from tracking
                const SkScalar kerned = coretext_line(system.get(),
                                                      c.text,
                                                      spacing,
                                                      c.direction,
                                                      {},
                                                      kCTKernAttributeName)
                                                .width;
                REPORTER_ASSERT(reporter,
                                SkScalarNearlyEqual(
                                        impl->lines()[0].width(), kerned, kTrackedLineTolerance),
                                "%s: width %.5f, native kerning %.5f",
                                label.c_str(),
                                impl->lines()[0].width(),
                                kerned);
            }
        }
    }
}

// Native tracking turns ligatures off for any nonzero value, negative included, while kerning
// keeps them; Helvetica ligates fi and ffl
DEF_TEST(CoreTextPipeline_LetterSpacing_DisablesLigaturesLikeNativeTracking, reporter) {
    if (!has_font_families({"Helvetica"})) {
        return;
    }
    const char* text = "office fluffy";
    SkUniqueCFRef<CTFontRef> helvetica(CTFontCreateWithName(CFSTR("Helvetica"), 24, nullptr));
    const PlainLine ligated = coretext_line(helvetica.get(), text, 0);
    REPORTER_ASSERT(reporter, ligated.x.size() < strlen(text), "Helvetica forms no ligature");
    for (SkScalar spacing : {-0.5f, 0.0f, 0.5f}) {
        SkString label = SkStringPrintf("Helvetica spacing %g", spacing);
        auto paragraph = layout_spaced("Helvetica", 24, spacing, text, TextDirection::kLtr);
        expect_drawn_as_tracked_line(reporter,
                                     label.c_str(),
                                     static_cast<ParagraphImpl*>(paragraph.get()),
                                     spacing,
                                     coretext_line(helvetica.get(), text, spacing));
    }
}

// Styles with different letterSpacing on one line track like CTLine ranges with different
// kCTTrackingAttributeName values, including a style boundary inside fallback text
DEF_TEST(CoreTextPipeline_LetterSpacing_MixedStylesLikeNativeTrackingRanges, reporter) {
    if (!has_font_families({".AppleSystemUIFont"})) {
        return;
    }
    struct Case {
        const char* first;
        SkScalar firstSpacing;
        const char* second;
        SkScalar secondSpacing;
    };
    const Case cases[] = {
            {"Hello ", -0.41f, "World", 1.0f},
            {"Ab \u6C34\u6C34", 1.0f, "\u6C34 \u05E9\u05DC\u05D5\u05DD", -0.41f},
    };
    for (const Case& c : cases) {
        for (SkScalar size : {13.0f, 17.0f, 28.0f}) {
            ParagraphStyle paragraphStyle;
            paragraphStyle.setTextShapingEngine(TextShapingEngine::kCoreText);
            paragraphStyle.setTextAlign(TextAlign::kLeft);
            ParagraphBuilderImpl builder(
                    paragraphStyle, test_font_collection(), get_unicode(paragraphStyle));
            TextStyle style;
            style.setFontFamilies({SkString(".AppleSystemUIFont")});
            style.setFontSize(size);
            style.setLetterSpacing(c.firstSpacing);
            builder.pushStyle(style);
            builder.addText(c.first);
            style.setLetterSpacing(c.secondSpacing);
            builder.pushStyle(style);
            builder.addText(c.second);
            auto paragraph = builder.Build();
            paragraph->layout(10000);

            SkString text(c.first);
            text.append(c.second);
            SkUniqueCFRef<CFStringRef> first(
                    CFStringCreateWithCString(nullptr, c.first, kCFStringEncodingUTF8));
            SkUniqueCFRef<CFStringRef> second(
                    CFStringCreateWithCString(nullptr, c.second, kCFStringEncodingUTF8));
            const CFIndex firstLength = CFStringGetLength(first.get());
            SkUniqueCFRef<CTFontRef> system(
                    CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, size, nullptr));
            const PlainLine native = coretext_line(
                    system.get(),
                    text.c_str(),
                    0,
                    std::nullopt,
                    {{CFRangeMake(0, firstLength), c.firstSpacing},
                     {CFRangeMake(firstLength, CFStringGetLength(second.get())), c.secondSpacing}});
            SkString label = SkStringPrintf("'%s' %gpt", text.c_str(), size);
            expect_drawn_as_tracked_line(reporter,
                                         label.c_str(),
                                         static_cast<ParagraphImpl*>(paragraph.get()),
                                         c.firstSpacing,
                                         native);
        }
    }
}

// Trailing spaces take the spacing too, as native tracking gives them: the line width with
// spaces is the CTLine width, and without them the CTLine width of the text without the spaces.
// CTLineGetTrailingWhitespaceWidth does not give the latter: it also counts positive tracking
// after the last glyph, which kCTKernAttributeName does not
DEF_TEST(CoreTextPipeline_LetterSpacing_TrailingSpaceLikeNativeTracking, reporter) {
    if (!has_font_families({".AppleSystemUIFont"})) {
        return;
    }
    for (SkScalar spacing : {-0.41f, 1.0f}) {
        SkUniqueCFRef<CTFontRef> system(
                CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, 17, nullptr));
        const SkScalar nativeWithSpaces = coretext_line(system.get(), "ok  ", spacing).width;
        const SkScalar nativeWithout = coretext_line(system.get(), "ok", spacing).width;
        auto paragraph =
                layout_spaced(".AppleSystemUIFont", 17, spacing, "ok  ", TextDirection::kLtr);
        auto* impl = static_cast<ParagraphImpl*>(paragraph.get());
        REPORTER_ASSERT(reporter, impl->isCoreTextShaped() && impl->lines().size() == 1);
        if (impl->lines().size() != 1) {
            continue;
        }
        const TextLine& line = impl->lines()[0];
        REPORTER_ASSERT(
                reporter,
                SkScalarNearlyEqual(
                        line.width() + line.spacesWidth(), nativeWithSpaces, kTrackedLineTolerance),
                "spacing %g: width with spaces %.5f, native %.5f",
                spacing,
                line.width() + line.spacesWidth(),
                nativeWithSpaces);
        REPORTER_ASSERT(reporter,
                        SkScalarNearlyEqual(line.width(), nativeWithout, kTrackedLineTolerance),
                        "spacing %g: width %.5f, native without the spaces %.5f",
                        spacing,
                        line.width(),
                        nativeWithout);
    }
}

// A letter-spaced ellipsis that falls back keeps the fallback font's trak, as the line text
// does. By Flutter convention SkParagraph adds no letterSpacing to the ellipsis (a native
// truncation token would take the tracking), so it is exactly as wide as the CTLine of its font
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
            REPORTER_ASSERT(reporter, impl->isCoreTextShaped() && impl->lines().size() == 1);
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
            const SkScalar expected = coretext_line(fallback.get(), ellipsis, 0).width;
            // The fallback face must track at this size, or the check below proves nothing
            SkUniqueCFRef<CTFontRef> untracked = untracked_copy(fallback.get());
            const SkScalar untrackedWidth = coretext_line(untracked.get(), ellipsis, 0).width;
            REPORTER_ASSERT(reporter,
                            !SkScalarNearlyEqual(expected, untrackedWidth, 0.01f),
                            "size %g %s: no trak, %.5f with and without",
                            size,
                            runName.c_str(),
                            expected);
            REPORTER_ASSERT(
                    reporter,
                    SkScalarNearlyEqual(ellipsisRun->advance().fX, expected, kTrackedLineTolerance),
                    "size %g %s: ellipsis width %.5f, native %.5f",
                    size,
                    runName.c_str(),
                    ellipsisRun->advance().fX,
                    expected);
        }
    }
}

// CoreText derives advances from drawn x, so a mark drawn off the pen gets an advance and the
// glyph before it in its run loses as much. Each glyph must still draw where CoreText draws it,
// mark clusters must stay empty and carets must fall as on HarfBuzz
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
    const Case cases[] = {
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
    for (const Case& c : cases) {
        const PlainLine plain = plain_coretext_line(c.family, c.text);
        auto paragraph = expect_drawn_as_coretext(reporter, c.family, c.text, c.direction, plain);
        auto* impl = static_cast<ParagraphImpl*>(paragraph.get());
        // The case only tests something if CoreText gives some mark an advance
        bool markAdvanced = false;
        const std::vector<DrawnGlyph> glyphs = drawn_glyphs(impl);
        for (size_t k = 0; k < std::min(glyphs.size(), plain.advances.size()); ++k) {
            markAdvanced |= glyphs[k].mark && plain.advances[k] != 0;
        }
        REPORTER_ASSERT(reporter, markAdvanced, "%s: CoreText advances no mark", c.family);

        // SF's optical size makes HarfBuzz advances uniformly wider, so caret x are compared
        // relative to the line width
        auto harfBuzz = layout_marked(TextShapingEngine::kDefault, c.family, c.text, c.direction);
        const SkScalar scale = paragraph->getMaxIntrinsicWidth() / harfBuzz->getMaxIntrinsicWidth();
        const std::vector<CaretStep> ctSteps = caret_steps(paragraph.get());
        const std::vector<CaretStep> hbSteps = caret_steps(harfBuzz.get());
        REPORTER_ASSERT(reporter,
                        ctSteps.size() == hbSteps.size(),
                        "%s: %zu caret steps on CoreText, %zu on HarfBuzz",
                        c.family,
                        ctSteps.size(),
                        hbSteps.size());
        for (size_t i = 0; i < std::min(ctSteps.size(), hbSteps.size()); ++i) {
            const CaretStep& ct = ctSteps[i];
            const CaretStep& hb = hbSteps[i];
            REPORTER_ASSERT(reporter,
                            ct.position == hb.position && ct.affinity == hb.affinity &&
                                    SkScalarNearlyEqual(ct.x, hb.x * scale, 1.0f),
                            "%s caret step %zu: CoreText %d/%d from x=%g, HarfBuzz %d/%d from "
                            "x=%g (scaled %g)",
                            c.family,
                            i,
                            ct.position,
                            ct.affinity,
                            ct.x,
                            hb.position,
                            hb.affinity,
                            hb.x,
                            hb.x * scale);
        }
    }
}

// On a line that starts its CTLine, a tab ends at CoreText's tab stop even when its glyph has no
// hmtx advance, and the tab's cluster keeps that advance. This is CoreText's tab model, not
// HarfBuzz parity: HarfBuzz gives the tab its hmtx advance (KEEPER-DEBT in shapeTextRegion)
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
            auto harfBuzz = layout_marked(TextShapingEngine::kDefault, family, text,
                                          TextDirection::kLtr);
            INFOF(reporter,
                  "%s: CoreText width %g, HarfBuzz %g\n",
                  family,
                  paragraph->getMaxIntrinsicWidth(),
                  harfBuzz->getMaxIntrinsicWidth());
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
    REPORTER_ASSERT(reporter, impl->isCoreTextShaped() && paragraph->unresolvedGlyphs() == 0);
    // extendedVisit and getPath read fPositions without fOffsets, so cursive y stays in fOffsets
    REPORTER_ASSERT(reporter, !impl->runs().empty());
    bool movedInY = false;
    for (const Run& run : impl->runs()) {
        REPORTER_ASSERT(reporter, run.size() > 0);
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
    REPORTER_ASSERT(reporter, !lines.empty() && glyphs.size() >= 8);
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
        REPORTER_ASSERT(reporter,
                        base,
                        "mark at %zu: no base glyph at %zu",
                        mark.cluster,
                        baseCluster);
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

// The ellipsis is shaped apart from the text, with the same advances and offsets: a mark keeps
// no advance and draws where CoreText draws it
DEF_TEST(CoreTextPipeline_EllipsisMarksTakeNoAdvance, reporter) {
    if (!has_font_families({"Arial", "Arial Hebrew"})) {
        return;
    }
    for (const char* family : {"Arial", "Arial Hebrew"}) {
        for (const char* ellipsis : {"\u05D1\u05B8", "\u05D1\u05B8\u2026"}) {
            ParagraphStyle paragraphStyle;
            paragraphStyle.setTextShapingEngine(TextShapingEngine::kCoreText);
            paragraphStyle.setTextDirection(TextDirection::kRtl);
            paragraphStyle.setMaxLines(1);
            paragraphStyle.setEllipsis(SkString(ellipsis));
            TextStyle style;
            style.setFontFamilies({SkString(family)});
            style.setFontSize(20);
            ParagraphBuilderImpl builder(
                    paragraphStyle, test_font_collection(), get_unicode(paragraphStyle));
            builder.pushStyle(style);
            builder.addText(
                    "\u05E9\u05DC\u05D5\u05DD \u05E2\u05D5\u05DC\u05DD "
                    "\u05E9\u05DC\u05D5\u05DD \u05E2\u05D5\u05DC\u05DD "
                    "\u05E9\u05DC\u05D5\u05DD \u05E2\u05D5\u05DC\u05DD");
            auto paragraph = builder.Build();
            paragraph->layout(120);
            auto* impl = static_cast<ParagraphImpl*>(paragraph.get());
            const Run* run = impl->lines().empty() ? nullptr : impl->lines()[0].ellipsis();
            const PlainLine plain = plain_coretext_line(family, ellipsis);
            REPORTER_ASSERT(reporter,
                            impl->isCoreTextShaped() && run && run->size() == plain.x.size(),
                            "%s: no ellipsis run with %zu glyphs",
                            family,
                            plain.x.size());
            if (!run || run->size() != plain.x.size()) {
                continue;
            }
            REPORTER_ASSERT(reporter,
                            SkScalarNearlyEqual(run->advance().fX, plain.width, 0.01f),
                            "%s: ellipsis advance %g, CoreText line %g",
                            family,
                            run->advance().fX,
                            plain.width);
            for (size_t k = 0; k < run->size(); ++k) {
                const SkScalar drawn = run->positions()[k].fX + glyph_offset(*run, k).fX;
                REPORTER_ASSERT(reporter,
                                SkScalarNearlyEqual(drawn, plain.x[k], 0.01f),
                                "%s ellipsis glyph %zu: drawn at %g, CoreText at %g",
                                family,
                                k,
                                drawn,
                                plain.x[k]);
                // Arial draws the qamats off the baseline; its y belongs in the offset
                REPORTER_ASSERT(reporter,
                                run->positions()[k].fY == 0,
                                "%s ellipsis glyph %zu: position y %g",
                                family,
                                k,
                                run->positions()[k].fY);
            }
        }
    }
}

namespace {
// Ahem has square glyphs (advance = font size), like the FlutterTest font of the Flutter tests
sk_sp<FontCollection> make_ahem_collection() {
    sk_sp<SkTypeface> ahem = ToolUtils::CreateTypefaceFromResource("fonts/ahem.ttf");
    if (!ahem) {
        return nullptr;
    }
    auto provider = sk_make_sp<TypefaceFontProvider>();
    provider->registerTypeface(ahem, SkString("Ahem"));
    auto fontCollection = sk_make_sp<FontCollection>();
    fontCollection->setAssetFontManager(provider);
    fontCollection->disableFontFallback();
    return fontCollection;
}

std::unique_ptr<Paragraph> layout_ahem(const sk_sp<FontCollection>& fontCollection,
                                       TextShapingEngine engine,
                                       const char* text,
                                       TextDirection direction,
                                       SkScalar letterSpacing,
                                       SkScalar width) {
    ParagraphStyle paragraphStyle;
    paragraphStyle.setTextShapingEngine(engine);
    paragraphStyle.setTextDirection(direction);
    paragraphStyle.setApplyRoundingHack(false);
    TextStyle style;
    style.setFontFamilies({SkString("Ahem")});
    style.setFontSize(14.0f);
    style.setLetterSpacing(letterSpacing);
    ParagraphBuilderImpl builder(paragraphStyle, fontCollection, get_unicode(paragraphStyle));
    builder.pushStyle(style);
    builder.addText(text);
    auto paragraph = builder.Build();
    paragraph->layout(width);
    return paragraph;
}
}  // namespace

// A tap exactly on the right edge of the last glyph before '\n' belongs to that line end
// (upstream); Flutter's caret movement (editable_text_shortcuts) relies on it
DEF_TEST(CoreTextPipeline_HitTest_HardBreakLineEndParityWithHarfBuzz, reporter) {
    auto fontCollection = make_ahem_collection();
    if (!fontCollection) {
        // Only CoreText-backed font managers load resource data; the portable one cannot
        return;
    }
    const char* text = "aa\na\naa";
    for (TextDirection direction : {TextDirection::kLtr, TextDirection::kRtl}) {
        auto ct = layout_ahem(fontCollection, TextShapingEngine::kCoreText, text, direction, 0,
                              1000);
        auto hb = layout_ahem(fontCollection, TextShapingEngine::kDefault, text, direction, 0,
                              1000);
        REPORTER_ASSERT(reporter, static_cast<ParagraphImpl*>(ct.get())->isCoreTextShaped());
        REPORTER_ASSERT(reporter, ct->lineNumber() == 3 && hb->lineNumber() == 3);
        if (direction == TextDirection::kLtr) {
            PositionWithAffinity lineEnd = hb->getGlyphPositionAtCoordinate(28, 7);
            REPORTER_ASSERT(reporter, lineEnd.position == 2 && lineEnd.affinity == kUpstream,
                            "HarfBuzz line end %d/%d", lineEnd.position, lineEnd.affinity);
        }
        for (SkScalar y : {7.0f, 21.0f, 35.0f}) {
            for (SkScalar x : {0.0f, 6.0f, 7.0f, 13.0f, 14.0f, 15.0f, 27.0f, 28.0f, 29.0f,
                               500.0f, 986.0f, 1000.0f}) {
                PositionWithAffinity ctPos = ct->getGlyphPositionAtCoordinate(x, y);
                PositionWithAffinity hbPos = hb->getGlyphPositionAtCoordinate(x, y);
                REPORTER_ASSERT(reporter,
                                ctPos.position == hbPos.position &&
                                        ctPos.affinity == hbPos.affinity,
                                "dir=%d (%g,%g): CoreText %d/%d vs HarfBuzz %d/%d",
                                static_cast<int>(direction), x, y, ctPos.position,
                                ctPos.affinity, hbPos.position, hbPos.affinity);
            }
        }
    }
}

// A cluster wider than the line after a break opportunity starts the next line, as a longer
// word does; CoreText shapes a ZWJ emoji sequence into one such cluster even when unresolved
DEF_TEST(CoreTextPipeline_LineBreak_WideClusterStartsNextLine, reporter) {
    auto fontCollection = make_ahem_collection();
    if (!fontCollection) {
        return;
    }
    const char* kFamilyEmoji = "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D"
                               "\xF0\x9F\x91\xA6";
    const char* kHan = "\xE6\xBC\xA2";
    for (TextShapingEngine engine : {TextShapingEngine::kCoreText, TextShapingEngine::kDefault}) {
        for (const char* wide : {kFamilyEmoji, kHan}) {
            ParagraphStyle paragraphStyle;
            paragraphStyle.setTextShapingEngine(engine);
            paragraphStyle.setApplyRoundingHack(false);
            TextStyle style;
            style.setFontFamilies({SkString("Ahem")});
            style.setFontSize(14.0f);
            ParagraphBuilderImpl builder(paragraphStyle, fontCollection,
                                         get_unicode(paragraphStyle));
            builder.pushStyle(style);
            builder.addText("1");
            style.setFontSize(wide == kHan ? 30.0f : 14.0f);
            builder.pushStyle(style);
            builder.addText(wide);
            auto paragraph = builder.Build();
            paragraph->layout(20);
            std::vector<LineMetrics> lines;
            paragraph->getLineMetrics(lines);
            REPORTER_ASSERT(reporter, lines.size() >= 2 && lines[0].fEndIndex == 1 &&
                                              lines[1].fStartIndex == 1,
                            "engine %d %s: %zu lines, first [%zu,%zu)", static_cast<int>(engine),
                            wide == kHan ? "Han" : "emoji", lines.size(),
                            lines.empty() ? 0 : lines[0].fStartIndex,
                            lines.empty() ? 0 : lines[0].fEndIndex);
        }
    }
}

// Flutter lays text out again at getMaxIntrinsicWidth() and expects the same lines; spaced
// clusters (14.1 wide) make the float sum depend on the order the line breaker adds them
DEF_TEST(CoreTextPipeline_LineBreak_MaxIntrinsicWidthKeepsOneLine, reporter) {
    auto fontCollection = make_ahem_collection();
    if (!fontCollection) {
        return;
    }
    for (TextShapingEngine engine : {TextShapingEngine::kCoreText, TextShapingEngine::kDefault}) {
        for (TextDirection direction : {TextDirection::kLtr, TextDirection::kRtl}) {
            for (SkScalar letterSpacing : {0.1f, 1.1f}) {
                auto paragraph = layout_ahem(fontCollection, engine, "View licenses", direction,
                                             letterSpacing, 1000);
                SkScalar maxIntrinsicWidth = paragraph->getMaxIntrinsicWidth();
                paragraph->layout(maxIntrinsicWidth);
                REPORTER_ASSERT(reporter, paragraph->lineNumber() == 1,
                                "engine %d dir=%d ls=%g: %zu lines at width %.9g",
                                static_cast<int>(engine), static_cast<int>(direction),
                                letterSpacing, paragraph->lineNumber(), maxIntrinsicWidth);
            }
        }
    }
}

// SkParagraph spaces CoreText clusters itself, so letterSpacing lands after every glyph in
// either line direction and on non-cursive RTL scripts, exactly as on the HarfBuzz path
DEF_TEST(CoreTextPipeline_LetterSpacing_ParityWithHarfBuzz, reporter) {
    auto fontCollection = make_ahem_collection();
    if (!fontCollection) {
        return;
    }
    const char* kHebrew = "\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D";
    for (const char* text : {"Cancel", kHebrew}) {
        for (TextDirection direction : {TextDirection::kLtr, TextDirection::kRtl}) {
            for (SkScalar letterSpacing : {-0.41f, 1.5f}) {
                auto ct = layout_ahem(fontCollection, TextShapingEngine::kCoreText, text,
                                      direction, letterSpacing, 300);
                auto hb = layout_ahem(fontCollection, TextShapingEngine::kDefault, text,
                                      direction, letterSpacing, 300);
                REPORTER_ASSERT(reporter,
                                SkScalarNearlyEqual(ct->getMaxIntrinsicWidth(),
                                                    hb->getMaxIntrinsicWidth()),
                                "%s dir=%d ls=%g: width CoreText %g vs HarfBuzz %g", text,
                                static_cast<int>(direction), letterSpacing,
                                ct->getMaxIntrinsicWidth(), hb->getMaxIntrinsicWidth());
                const int length = text == kHebrew ? 4 : 6;
                for (int i = 0; i < length; ++i) {
                    auto ctBoxes = ct->getRectsForRange(i, i + 1, RectHeightStyle::kTight,
                                                        RectWidthStyle::kTight);
                    auto hbBoxes = hb->getRectsForRange(i, i + 1, RectHeightStyle::kTight,
                                                        RectWidthStyle::kTight);
                    REPORTER_ASSERT(reporter, ctBoxes.size() == 1 && hbBoxes.size() == 1);
                    if (ctBoxes.size() != 1 || hbBoxes.size() != 1) {
                        continue;
                    }
                    const SkRect& c = ctBoxes[0].rect;
                    const SkRect& h = hbBoxes[0].rect;
                    REPORTER_ASSERT(reporter,
                                    SkScalarNearlyEqual(c.fLeft, h.fLeft, 0.01f) &&
                                            SkScalarNearlyEqual(c.fRight, h.fRight, 0.01f),
                                    "%s dir=%d ls=%g [%d]: CoreText %g..%g vs HarfBuzz %g..%g",
                                    text, static_cast<int>(direction), letterSpacing, i,
                                    c.fLeft, c.fRight, h.fLeft, h.fRight);
                }
            }
        }
    }

    // Spacing goes between glyphs, so both engines shape positive letterSpacing without the
    // fi/fl ligatures
    sk_sp<SkTypeface> roboto = ToolUtils::CreateTypefaceFromResource("fonts/Roboto-Regular.ttf");
    if (!roboto) {
        return;
    }
    auto robotoCollection = make_roboto_collection(roboto);
    for (TextShapingEngine engine : {TextShapingEngine::kCoreText, TextShapingEngine::kDefault}) {
        ParagraphStyle paragraphStyle;
        paragraphStyle.setTextShapingEngine(engine);
        TextStyle style;
        style.setFontFamilies({SkString("Roboto")});
        style.setFontSize(20.0f);
        style.setLetterSpacing(2.0f);
        ParagraphBuilderImpl builder(paragraphStyle, robotoCollection,
                                     get_unicode(paragraphStyle));
        builder.pushStyle(style);
        builder.addText("fifl");
        auto paragraph = builder.Build();
        paragraph->layout(500);
        auto impl = static_cast<ParagraphImpl*>(paragraph.get());
        REPORTER_ASSERT(reporter, impl->runs().size() == 1 && impl->runs()[0].size() == 4,
                        "engine %d: %zu runs, %zu glyphs", static_cast<int>(engine),
                        impl->runs().size(), impl->runs().empty() ? 0 : impl->runs()[0].size());
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
            auto plain =
                    layout_ahem(fontCollection, TextShapingEngine::kCoreText, text, direction, 0,
                                1000);
            for (SkScalar letterSpacing : {0.1f, -0.41f}) {
                auto spaced = layout_ahem(fontCollection, TextShapingEngine::kCoreText, text,
                                          direction, letterSpacing, 1000);
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
        ParagraphBuilderImpl builder(paragraphStyle, robotoCollection,
                                     get_unicode(paragraphStyle));
        builder.pushStyle(style);
        builder.addText("a\xCC\x81");
        auto paragraph = builder.Build();
        paragraph->layout(500);
        widths[i] = paragraph->getMaxIntrinsicWidth();
    }
    REPORTER_ASSERT(reporter, SkScalarNearlyEqual(widths[1] - widths[0], 4.0f),
                    "spaced %g vs unspaced %g", widths[1], widths[0]);
}

namespace {
// Lays out text in Roboto with letterSpacing 4; a color-only style change at splitAt sends
// ParagraphImpl::applySpacingAndBuildClusterTable through its per-cluster path
std::unique_ptr<Paragraph> layout_spaced_split(const sk_sp<FontCollection>& fontCollection,
                                               TextShapingEngine engine,
                                               const char* text,
                                               size_t splitAt,
                                               TextDirection direction) {
    ParagraphStyle paragraphStyle;
    paragraphStyle.setTextShapingEngine(engine);
    paragraphStyle.setTextDirection(direction);
    TextStyle style;
    style.setFontFamilies({SkString("Roboto")});
    style.setFontSize(20.0f);
    style.setLetterSpacing(4.0f);
    style.setColor(SK_ColorBLACK);
    ParagraphBuilderImpl builder(paragraphStyle, fontCollection, get_unicode(paragraphStyle));
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
// runs in descending text order, and must still keep the point on its base and give the
// line the same half-spacing shift as HarfBuzz
DEF_TEST(CoreTextPipeline_LetterSpacing_StyleSplitKeepsMarkOnBase, reporter) {
    sk_sp<SkTypeface> roboto = ToolUtils::CreateTypefaceFromResource("fonts/Roboto-Regular.ttf");
    if (!roboto) {
        return;
    }
    auto fontCollection = make_roboto_collection(roboto);
    const char* shalom = "\xD7\xA9\xD6\xB8\xD7\x9C\xD7\x95\xD6\xB9\xD7\x9D";
    auto whole = layout_spaced_split(fontCollection, TextShapingEngine::kCoreText, shalom,
                                     strlen(shalom), TextDirection::kRtl);
    auto split = layout_spaced_split(fontCollection, TextShapingEngine::kCoreText, shalom, 4,
                                     TextDirection::kRtl);
    auto wholeRuns = static_cast<ParagraphImpl*>(whole.get())->runs();
    auto splitRuns = static_cast<ParagraphImpl*>(split.get())->runs();
    REPORTER_ASSERT(reporter, wholeRuns.size() == 1 && splitRuns.size() == 1);
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
    REPORTER_ASSERT(reporter, wholeRun.size() == splitRun.size());
    for (size_t i = 0; i <= std::min(wholeRun.size(), splitRun.size()); ++i) {
        REPORTER_ASSERT(reporter,
                        wholeRun.positions()[i].fX == splitRun.positions()[i].fX,
                        "glyph %zu: one style %g, two styles %g", i,
                        wholeRun.positions()[i].fX, splitRun.positions()[i].fX);
    }

    // CoreText puts x and U+0301 in two clusters; the line starts after half a letterSpacing
    const char* accented = "x\xCC\x81" "bc";
    auto ct = layout_spaced_split(fontCollection, TextShapingEngine::kCoreText, accented, 3,
                                  TextDirection::kLtr);
    auto hb = layout_spaced_split(fontCollection, TextShapingEngine::kDefault, accented, 3,
                                  TextDirection::kLtr);
    auto ctBoxes = ct->getRectsForRange(0, 2, RectHeightStyle::kTight, RectWidthStyle::kTight);
    auto hbBoxes = hb->getRectsForRange(0, 2, RectHeightStyle::kTight, RectWidthStyle::kTight);
    REPORTER_ASSERT(reporter, !ctBoxes.empty() && !hbBoxes.empty());
    if (!ctBoxes.empty() && !hbBoxes.empty()) {
        REPORTER_ASSERT(reporter, ctBoxes[0].rect.fLeft == hbBoxes[0].rect.fLeft,
                        "line start: CoreText %g, HarfBuzz %g", ctBoxes[0].rect.fLeft,
                        hbBoxes[0].rect.fLeft);
    }
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
    for (const Case& c : {Case{"\x01", ""}, Case{"\x1F", ""}, Case{"a\x01" "b", "ab"},
                          Case{"a\x1B" "b", "ab"}, Case{"a\x7F" "b", "ab"},
                          Case{"a\xC2\x90" "b", "ab"}, Case{"\x01" "b", "b"}, Case{"a\x01", "a"},
                          Case{"\xD7\x90\x01\xD7\x91", "\xD7\x90\xD7\x91"}}) {
        for (TextDirection direction : {TextDirection::kLtr, TextDirection::kRtl}) {
            auto ct = layout_ahem(fontCollection, TextShapingEngine::kCoreText, c.text,
                                  direction, 0, 1000);
            auto plain = layout_ahem(fontCollection, TextShapingEngine::kCoreText,
                                     c.withoutControl, direction, 0, 1000);
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
            REPORTER_ASSERT(reporter, covered == strlen(c.text), "len=%zu dir=%d: covered %zu",
                            strlen(c.text), static_cast<int>(direction), covered);
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
                            strlen(c.text), static_cast<int>(direction),
                            ct->getMaxIntrinsicWidth(), plain->getMaxIntrinsicWidth(),
                            validGlyphs, ctImpl->unresolvedGlyphs(), expectedUnresolved,
                            ct->getHeight());
        }
    }
}

// One-style kCoreText paragraph of text on one unwrapped line, without the rounding hack
static std::unique_ptr<Paragraph> layout_single_style(const sk_sp<FontCollection>& fontCollection,
                                                      const char* family,
                                                      const char* text,
                                                      SkScalar fontSize,
                                                      bool subpixel) {
    ParagraphStyle paragraphStyle;
    paragraphStyle.setTextShapingEngine(TextShapingEngine::kCoreText);
    paragraphStyle.setApplyRoundingHack(false);
    TextStyle style;
    style.setFontFamilies({SkString(family)});
    style.setFontSize(fontSize);
    style.setSubpixel(subpixel);
    ParagraphBuilderImpl builder(paragraphStyle, fontCollection, get_unicode(paragraphStyle));
    builder.pushStyle(style);
    builder.addText(text);
    auto paragraph = builder.Build();
    paragraph->layout(10000);
    return paragraph;
}

// SH6: a TextStyle with subpixel off lands every glyph advance, line width and ellipsis position
// on whole pixels, while subpixel on preserves fractional advances at fractional font sizes
DEF_TEST(CoreTextPipeline_NonSubpixel_AdvancesWholePixels, reporter) {
    // Run positions are the shaper's accumulated glyph advances, which is the contract under test
    auto countFractional = [&](Paragraph* paragraph) {
        auto* impl = static_cast<ParagraphImpl*>(paragraph);
        REPORTER_ASSERT(reporter, !impl->runs().empty());
        int fractional = 0;
        for (const Run& run : impl->runs()) {
            REPORTER_ASSERT(reporter, run.size() > 0);
            for (size_t i = 0; i <= run.size(); ++i) {
                fractional += run.positions()[i].fX != SkScalarRoundToScalar(run.positions()[i].fX);
            }
        }
        return fractional;
    };
    auto ahem = make_ahem_collection();
    if (ahem) {
        for (const char* text : {"OK", "Cancel", "Delete all", "Actions section overscroll"}) {
            for (SkScalar fontSize : {16.8f, 14.2f, 13.3f}) {
                for (bool subpixel : {false, true}) {
                    auto paragraph = layout_single_style(ahem, "Ahem", text, fontSize, subpixel);
                    REPORTER_ASSERT(
                            reporter,
                            static_cast<ParagraphImpl*>(paragraph.get())->isCoreTextShaped());
                    const int fractional = countFractional(paragraph.get());
                    const SkScalar width = paragraph->getLongestLine();
                    if (subpixel) {
                        REPORTER_ASSERT(reporter,
                                        fractional > 0,
                                        "'%s' size=%g subpixel: all positions whole",
                                        text,
                                        fontSize);
                    } else {
                        REPORTER_ASSERT(reporter,
                                        fractional == 0 && width == SkScalarRoundToScalar(width),
                                        "'%s' size=%g: %d fractional positions, width %.9g",
                                        text,
                                        fontSize,
                                        fractional,
                                        width);
                    }
                }
            }
        }
        // The ellipsis is shaped apart from the line and must land on the same grid
        for (SkScalar fontSize : {16.8f, 14.2f, 13.3f}) {
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
            auto* impl = static_cast<ParagraphImpl*>(paragraph.get());
            // No public API exposes the ellipsis run, which is the unit under test
            const Run* ellipsis = impl->lines().empty() ? nullptr : impl->lines()[0].ellipsis();
            REPORTER_ASSERT(reporter, ellipsis && ellipsis->size() > 0,
                            "size=%g: no ellipsis", fontSize);
            if (!ellipsis) {
                continue;
            }
            for (size_t i = 0; i <= ellipsis->size(); ++i) {
                const SkScalar x = ellipsis->positions()[i].fX;
                REPORTER_ASSERT(reporter,
                                x == SkScalarRoundToScalar(x),
                                "ellipsis size=%g glyph %zu at %.9g",
                                fontSize,
                                i,
                                x);
            }
        }
    }

    // CoreText gives ZWJ and soft hyphen no advance while their nominal one is not zero; they
    // stay zero, which is also a whole pixel, and the glyphs after them stay on the grid
    if (has_font_families({"Helvetica"})) {
        const char* text = "a\xE2\x80\x8D" "b\xC2\xAD" "c";
        for (SkScalar fontSize : {13.3f, 16.8f, 23.7f}) {
            auto paragraph =
                    layout_single_style(test_font_collection(), "Helvetica", text, fontSize, false);
            const int fractional = countFractional(paragraph.get());
            REPORTER_ASSERT(reporter,
                            fractional == 0,
                            "ignorables size=%g: %d fractional positions",
                            fontSize,
                            fractional);
        }
    }
}

namespace {
struct PaintedGlyph {
    SkGlyphID id;
    SkScalar drawnX;
    SkScalar nominal;
};

// Glyphs paragraph draws, in paint order, at their drawn x with their nominal advances from
// CTFontGetAdvancesForGlyphs (font data including optical size and trak, which SkFont::getWidths
// leaves out); when non-null, font receives the last glyph run's SkFont
std::vector<PaintedGlyph> painted_glyphs(Paragraph* paragraph, SkFont* font = nullptr) {
    std::vector<PaintedGlyph> glyphs;
    paragraph->visit([&](int, const Paragraph::VisitorInfo* info) {
        if (!info) {
            return;
        }
        if (font) {
            *font = info->font;
        }
        SkTypeface* tf = info->font.getTypeface();
        SkUniqueCFRef<CTFontRef> ctFont = CoreTextFontRegistry::Instance().createFont(
                tf, tf ? tf->fontStyle() : SkFontStyle(), info->font.getSize());
        if (!ctFont) {
            return;
        }
        for (int i = 0; i < info->count; ++i) {
            const CGGlyph glyph = info->glyphs[i];
            CGSize nominal = CGSizeZero;
            CTFontGetAdvancesForGlyphs(
                    ctFont.get(), kCTFontOrientationHorizontal, &glyph, &nominal, 1);
            glyphs.push_back({info->glyphs[i],
                              info->origin.fX + info->positions[i].fX,
                              static_cast<SkScalar>(nominal.width)});
        }
    });
    return glyphs;
}
}  // namespace

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
                auto paragraph =
                        layout_single_style(fontCollection, family, text, fontSize, true);
                REPORTER_ASSERT(reporter,
                                static_cast<ParagraphImpl*>(paragraph.get())->isCoreTextShaped());
                SkFont font;
                const std::vector<PaintedGlyph> glyphs = painted_glyphs(paragraph.get(), &font);
                SkGlyphID cmap[2] = {};
                font.textToGlyphs(text, 2, SkTextEncoding::kUTF8, cmap);
                REPORTER_ASSERT(reporter,
                                glyphs.size() == 2 && glyphs[0].id == cmap[0] &&
                                        glyphs[1].id == cmap[1],
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

// A primary font without U+000A: HarfBuzz keeps '\n' in it as .notdef, CoreText moves it to a
// fallback font. Unspaced lines must still match; the letter-spaced widths are only logged, as
// the split run spaces them in another order
DEF_TEST(CoreTextPipeline_HardBreak_PrimaryFontWithoutNewline, reporter) {
    sk_sp<SkTypeface> dejaVu = ToolUtils::CreateTypefaceFromResource("fonts/DejaVuSans.subset.ttf");
    if (!dejaVu) {
        return;
    }
    REPORTER_ASSERT(
            reporter,
            SkFont(dejaVu).unicharToGlyph('\n') == 0 && SkFont(dejaVu).unicharToGlyph('a') != 0);
    auto provider = sk_make_sp<TypefaceFontProvider>();
    provider->registerTypeface(dejaVu, SkString("DejaVu Sans"));
    auto fontCollection = sk_make_sp<FontCollection>();
    fontCollection->setAssetFontManager(provider);
    fontCollection->setDefaultFontManager(ToolUtils::TestFontMgr());
    auto layout = [&](TextShapingEngine engine, const char* text, SkScalar letterSpacing) {
        ParagraphStyle paragraphStyle;
        paragraphStyle.setTextShapingEngine(engine);
        paragraphStyle.setApplyRoundingHack(false);
        TextStyle style;
        style.setFontFamilies({SkString("DejaVu Sans")});
        style.setFontSize(20.0f);
        style.setLetterSpacing(letterSpacing);
        ParagraphBuilderImpl builder(paragraphStyle, fontCollection, get_unicode(paragraphStyle));
        builder.pushStyle(style);
        builder.addText(text);
        auto paragraph = builder.Build();
        paragraph->layout(10000);
        return paragraph;
    };
    for (const char* text : {"aa\naa", "aaaaaaaaaaaaa\naaaa", "a\n\na"}) {
        for (SkScalar letterSpacing : {0.0f, 0.6f}) {
            auto ct = layout(TextShapingEngine::kCoreText, text, letterSpacing);
            auto hb = layout(TextShapingEngine::kDefault, text, letterSpacing);
            std::vector<LineMetrics> ctLines, hbLines;
            ct->getLineMetrics(ctLines);
            hb->getLineMetrics(hbLines);
            REPORTER_ASSERT(reporter, ctLines.size() == hbLines.size());
            for (size_t i = 0; i < std::min(ctLines.size(), hbLines.size()); ++i) {
                if (letterSpacing != 0) {
                    INFOF(reporter,
                          "'%s' spaced line %zu: CoreText w=%.9g, HarfBuzz w=%.9g\n",
                          text,
                          i,
                          ctLines[i].fWidth,
                          hbLines[i].fWidth);
                    continue;
                }
                REPORTER_ASSERT(reporter,
                                ctLines[i].fWidth == hbLines[i].fWidth,
                                "'%s' line %zu: CoreText w=%.9g vs HarfBuzz w=%.9g",
                                text,
                                i,
                                ctLines[i].fWidth,
                                hbLines[i].fWidth);
            }
            if (letterSpacing != 0) {
                continue;
            }
            for (size_t i = 0; i < strlen(text); ++i) {
                auto ctBoxes = ct->getRectsForRange(
                        i, i + 1, RectHeightStyle::kTight, RectWidthStyle::kTight);
                auto hbBoxes = hb->getRectsForRange(
                        i, i + 1, RectHeightStyle::kTight, RectWidthStyle::kTight);
                REPORTER_ASSERT(reporter, ctBoxes.size() == hbBoxes.size());
                for (size_t k = 0; k < std::min(ctBoxes.size(), hbBoxes.size()); ++k) {
                    REPORTER_ASSERT(reporter,
                                    ctBoxes[k].rect.fLeft == hbBoxes[k].rect.fLeft &&
                                            ctBoxes[k].rect.fRight == hbBoxes[k].rect.fRight,
                                    "'%s' char %zu: CoreText [%.9g, %.9g] vs HarfBuzz [%.9g, %.9g]",
                                    text,
                                    i,
                                    ctBoxes[k].rect.fLeft,
                                    ctBoxes[k].rect.fRight,
                                    hbBoxes[k].rect.fLeft,
                                    hbBoxes[k].rect.fRight);
                }
            }
        }
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
            auto spaced = layout(c.text, letterSpacing);
            std::vector<LineMetrics> lines;
            spaced->getLineMetrics(lines);
            REPORTER_ASSERT(reporter,
                            lines.size() == c.lines && plainLines.size() == c.lines,
                            "'%s' spacing=%g: %zu lines, %zu unspaced",
                            c.text,
                            letterSpacing,
                            lines.size(),
                            plainLines.size());
            for (size_t i = 0; i < std::min(lines.size(), plainLines.size()); ++i) {
                // ASCII text has one grapheme per byte and trailing spaces are excluded from fWidth
                const size_t graphemes = lines[i].fEndExcludingWhitespaces - lines[i].fStartIndex;
                const double expected = plainLines[i].fWidth + letterSpacing * graphemes;
                REPORTER_ASSERT(reporter,
                                SkScalarNearlyEqual(lines[i].fWidth, expected, 1e-3f),
                                "'%s' spacing=%g line %zu: width %.9g, expected %.9g",
                                c.text,
                                letterSpacing,
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
        const auto& oneLineRuns = static_cast<ParagraphImpl*>(oneLine.get())->runs();
        REPORTER_ASSERT(reporter,
                        oneLineRuns.size() == 1 && oneLineRuns[0].size() == 3,
                        "x, U+0301, b: %zu runs, %zu glyphs",
                        oneLineRuns.size(),
                        oneLineRuns.empty() ? 0 : oneLineRuns[0].size());
        std::vector<LineMetrics> twoLineMetrics, oneLineMetrics;
        twoLines->getLineMetrics(twoLineMetrics);
        oneLine->getLineMetrics(oneLineMetrics);
        REPORTER_ASSERT(reporter, twoLineMetrics.size() == 2 && oneLineMetrics.size() == 1);
        if (twoLineMetrics.size() == 2 && oneLineMetrics.size() == 1) {
            REPORTER_ASSERT(reporter,
                            SkScalarNearlyEqual(twoLineMetrics[1].fWidth, oneLineMetrics[0].fWidth),
                            "accented line: after '\\n' %g, alone %g",
                            twoLineMetrics[1].fWidth,
                            oneLineMetrics[0].fWidth);
        }
    }
}
