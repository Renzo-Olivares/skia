/*
 * Copyright 2026 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
#include "modules/skparagraph/tests/coretext/CoreTextTestUtils.h"

#include "include/core/SkFontMgr.h"
#include "include/core/SkFontStyle.h"
#include "include/core/SkString.h"
#include "modules/skparagraph/include/TextStyle.h"
#include "modules/skparagraph/include/TypefaceFontProvider.h"
#include "modules/skparagraph/src/CoreTextFontRegistry.h"
#include "modules/skparagraph/src/ParagraphBuilderImpl.h"
#if defined(SK_UNICODE_APPLE_IMPLEMENTATION)
#include "modules/skunicode/include/SkUnicode_apple.h"
#endif
#if defined(SK_UNICODE_ICU_IMPLEMENTATION)
#include "modules/skunicode/include/SkUnicode_icu.h"
#endif
#include "tools/fonts/FontToolUtils.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>

namespace skia::textlayout::coretext_test {

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

sk_sp<FontCollection> test_font_collection() {
    auto fontCollection = sk_make_sp<FontCollection>();
    fontCollection->setDefaultFontManager(ToolUtils::TestFontMgr());
    return fontCollection;
}

SkUnicode* test_unicode() {
#if defined(SK_UNICODE_APPLE_IMPLEMENTATION)
    static SkUnicode* unicode = SkUnicodes::Apple::Make().release();
#else
    static SkUnicode* unicode = SkUnicodes::ICU::Make().release();
#endif
    SkASSERT(unicode);
    return unicode;
}

bool has_font_families(std::initializer_list<const char*> families) {
    for (const char* family : families) {
        if (!ToolUtils::TestFontMgr()->matchFamilyStyle(family, SkFontStyle())) {
            return false;
        }
    }
    return true;
}

SkString postscript_name(const SkTypeface& typeface) {
    SkString name;
    typeface.getPostScriptName(&name);
    return name;
}

sk_sp<FontCollection> make_roboto_collection(const sk_sp<SkTypeface>& regular) {
    auto provider = sk_make_sp<TypefaceFontProvider>();
    provider->registerTypeface(regular, SkString("Roboto"));
    auto fontCollection = sk_make_sp<FontCollection>();
    fontCollection->setAssetFontManager(provider);
    fontCollection->setDefaultFontManager(ToolUtils::TestFontMgr());
    return fontCollection;
}

std::vector<SkScalar> last_blob_x(Paragraph* paragraph) {
    std::vector<SkScalar> xs;
    paragraph->visit([&](int, const Paragraph::VisitorInfo* info) {
        if (!info) {
            return;
        }
        xs.clear();
        for (int i = 0; i < info->count; ++i) {
            xs.push_back(info->origin.fX + info->positions[i].fX);
        }
    });
    return xs;
}

PlainLine coretext_line(CTFontRef font, const char* text, std::optional<TextDirection> direction) {
    SkUniqueCFRef<CFStringRef> string(
            CFStringCreateWithCString(nullptr, text, kCFStringEncodingUTF8));
    SkUniqueCFRef<CFMutableAttributedStringRef> attrString(
            CFAttributedStringCreateMutable(nullptr, 0));
    CFAttributedStringReplaceString(attrString.get(), CFRangeMake(0, 0), string.get());
    const CFRange all = CFRangeMake(0, CFStringGetLength(string.get()));
    CFAttributedStringSetAttribute(attrString.get(), all, kCTFontAttributeName, font);
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
    result.line = std::move(line);
    return result;
}

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
                                       const char* text,
                                       TextDirection direction,
                                       SkScalar letterSpacing,
                                       SkScalar width) {
    ParagraphStyle paragraphStyle;
    paragraphStyle.setTextShapingEngine(TextShapingEngine::kCoreText);
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

std::unique_ptr<Paragraph> layout_single_style(const sk_sp<FontCollection>& fontCollection,
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

std::vector<PaintedGlyph> painted_glyphs(Paragraph* paragraph, SkFont* font) {
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

}  // namespace skia::textlayout::coretext_test
