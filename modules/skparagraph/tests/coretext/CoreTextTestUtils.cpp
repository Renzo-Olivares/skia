/*
 * Copyright 2026 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
#include "modules/skparagraph/tests/coretext/CoreTextTestUtils.h"

#include "include/core/SkFontMgr.h"
#include "include/core/SkFontStyle.h"
#include "modules/skparagraph/include/TypefaceFontProvider.h"
#include "modules/skparagraph/src/CoreTextFontRegistry.h"
#include "modules/skparagraph/src/ParagraphImpl.h"
#if defined(SK_UNICODE_APPLE_IMPLEMENTATION)
#include "modules/skunicode/include/SkUnicode_apple.h"
#endif
#if defined(SK_UNICODE_ICU_IMPLEMENTATION)
#include "modules/skunicode/include/SkUnicode_icu.h"
#endif
#include "tools/fonts/FontToolUtils.h"

#include <CoreFoundation/CoreFoundation.h>

#include <algorithm>

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

sk_sp<SkUnicode> contract_unicode() {
#if defined(SK_UNICODE_APPLE_IMPLEMENTATION)
    return SkUnicodes::Apple::Make();
#elif defined(SK_UNICODE_ICU_IMPLEMENTATION)
    return SkUnicodes::ICU::Make();
#else
    return nullptr;
#endif
}

sk_sp<FontCollection> system_collection() {
    auto fontCollection = sk_make_sp<FontCollection>();
    fontCollection->setDefaultFontManager(ToolUtils::TestFontMgr());
    return fontCollection;
}

sk_sp<FontCollection> asset_collection(
        std::initializer_list<std::pair<const char*, const char*>> resourceAndFamily) {
    auto provider = sk_make_sp<TypefaceFontProvider>();
    for (const auto& [resource, family] : resourceAndFamily) {
        sk_sp<SkTypeface> typeface = ToolUtils::CreateTypefaceFromResource(resource);
        if (!typeface) {
            return nullptr;
        }
        provider->registerTypeface(std::move(typeface), SkString(family));
    }
    auto fontCollection = sk_make_sp<FontCollection>();
    fontCollection->setAssetFontManager(std::move(provider));
    fontCollection->disableFontFallback();
    return fontCollection;
}

ParagraphStyle contract_paragraph_style() {
    ParagraphStyle style;
    style.setTextShapingEngine(TextShapingEngine::kCoreText);
    style.setApplyRoundingHack(false);
    return style;
}

std::unique_ptr<ParagraphBuilderImpl> contract_builder(const sk_sp<FontCollection>& fontCollection,
                                                       const ParagraphStyle& style) {
    SkASSERT(style.getTextShapingEngine() == TextShapingEngine::kCoreText);
    return std::make_unique<ParagraphBuilderImpl>(style, fontCollection, contract_unicode());
}

std::unique_ptr<Paragraph> layout_built(skiatest::Reporter* reporter,
                                        ParagraphBuilderImpl* builder,
                                        SkScalar width) {
    std::unique_ptr<Paragraph> paragraph = builder->Build();
    paragraph->layout(width);
    const auto* impl = static_cast<ParagraphImpl*>(paragraph.get());
    REPORTER_ASSERT(reporter, impl->text().empty() || impl->isCoreTextShaped());
    return paragraph;
}

std::unique_ptr<Paragraph> layout_text(skiatest::Reporter* reporter,
                                       const sk_sp<FontCollection>& fontCollection,
                                       const ParagraphStyle& paragraphStyle,
                                       const TextStyle& style,
                                       std::string_view text,
                                       SkScalar width) {
    std::unique_ptr<ParagraphBuilderImpl> builder =
            contract_builder(fontCollection, paragraphStyle);
    builder->pushStyle(style);
    builder->addText(text.data(), text.size());
    return layout_built(reporter, builder.get(), width);
}

TextStyle contract_text_style(const char* family, SkScalar size) {
    TextStyle style;
    style.setFontFamilies({SkString(family)});
    style.setFontSize(size);
    return style;
}

std::vector<DrawnGlyph> drawn_glyphs(Paragraph* paragraph) {
    std::vector<DrawnGlyph> glyphs;
    paragraph->visit([&glyphs](int lineNumber, const Paragraph::VisitorInfo* info) {
        if (!info) {
            return;
        }
        for (int i = 0; i < info->count; ++i) {
            glyphs.push_back({info->glyphs[i],
                              info->origin + info->positions[i],
                              info->font,
                              lineNumber,
                              info->utf8Starts[i]});
        }
    });
    return glyphs;
}

std::vector<SkGlyphID> drawn_glyph_ids(Paragraph* paragraph) {
    std::vector<SkGlyphID> ids;
    for (const DrawnGlyph& glyph : drawn_glyphs(paragraph)) {
        ids.push_back(glyph.glyph);
    }
    return ids;
}

std::vector<LineMetrics> line_metrics(Paragraph* paragraph) {
    std::vector<LineMetrics> metrics;
    paragraph->getLineMetrics(metrics);
    return metrics;
}

std::vector<TextBox> tight_boxes(Paragraph* paragraph, size_t start, size_t end) {
    return paragraph->getRectsForRange(start, end, RectHeightStyle::kTight, RectWidthStyle::kTight);
}

std::optional<TextBox> single_tight_box(skiatest::Reporter* reporter,
                                        Paragraph* paragraph,
                                        size_t start,
                                        size_t end) {
    std::vector<TextBox> boxes = tight_boxes(paragraph, start, end);
    REPORTER_ASSERT(
            reporter, boxes.size() == 1, "[%zu, %zu): got %zu boxes", start, end, boxes.size());
    if (boxes.size() != 1) {
        return std::nullopt;
    }
    return boxes[0];
}

SkString family_name(const SkFont& font) {
    SkString name;
    font.getTypeface()->getFamilyName(&name);
    return name;
}

std::optional<SkScalar> axis_coordinate(const SkTypeface* typeface, SkFourByteTag axis) {
    const int count = typeface->getVariationDesignPosition({});
    std::vector<SkFontArguments::VariationPosition::Coordinate> coordinates(std::max(count, 0));
    typeface->getVariationDesignPosition(coordinates);
    for (const auto& coordinate : coordinates) {
        if (coordinate.axis == axis) {
            return coordinate.value;
        }
    }
    return std::nullopt;
}

SkUniqueCFRef<CTFontRef> native_font(const char* family, SkScalar size) {
    SkUniqueCFRef<CFStringRef> name(
            CFStringCreateWithCString(nullptr, family, kCFStringEncodingUTF8));
    return SkUniqueCFRef<CTFontRef>(CTFontCreateWithName(name.get(), size, nullptr));
}

SkUniqueCFRef<CTLineRef> native_line(CTFontRef font,
                                     std::string_view text,
                                     SkScalar tracking,
                                     std::optional<TextDirection> baseDirection) {
    SkUniqueCFRef<CFStringRef> string(
            CFStringCreateWithBytes(nullptr,
                                    reinterpret_cast<const UInt8*>(text.data()),
                                    static_cast<CFIndex>(text.size()),
                                    kCFStringEncodingUTF8,
                                    false));
    SkUniqueCFRef<CFMutableAttributedStringRef> attributed(
            CFAttributedStringCreateMutable(nullptr, 0));
    CFAttributedStringReplaceString(attributed.get(), CFRangeMake(0, 0), string.get());
    const CFRange all = CFRangeMake(0, CFStringGetLength(string.get()));
    CFAttributedStringSetAttribute(attributed.get(), all, kCTFontAttributeName, font);
    if (tracking != 0) {
        const double value = tracking;
        SkUniqueCFRef<CFNumberRef> number(CFNumberCreate(nullptr, kCFNumberDoubleType, &value));
        CFAttributedStringSetAttribute(
                attributed.get(), all, kCTTrackingAttributeName, number.get());
    }
    if (baseDirection) {
        CTWritingDirection writing = *baseDirection == TextDirection::kRtl
                                             ? kCTWritingDirectionRightToLeft
                                             : kCTWritingDirectionLeftToRight;
        CTParagraphStyleSetting setting = {
                kCTParagraphStyleSpecifierBaseWritingDirection, sizeof(writing), &writing};
        SkUniqueCFRef<CTParagraphStyleRef> paragraphStyle(CTParagraphStyleCreate(&setting, 1));
        CFAttributedStringSetAttribute(
                attributed.get(), all, kCTParagraphStyleAttributeName, paragraphStyle.get());
    }
    return SkUniqueCFRef<CTLineRef>(CTLineCreateWithAttributedString(attributed.get()));
}

SkScalar native_line_width(CTLineRef line) {
    return static_cast<SkScalar>(CTLineGetTypographicBounds(line, nullptr, nullptr, nullptr));
}

TextStyle spaced_text_style(const char* family, SkScalar size, SkScalar letterSpacing) {
    TextStyle style = contract_text_style(family, size);
    style.setLetterSpacing(letterSpacing);
    return style;
}

SkScalar first_line_width(Paragraph* paragraph) {
    const std::vector<LineMetrics> metrics = line_metrics(paragraph);
    return metrics.empty() ? -1 : static_cast<SkScalar>(metrics[0].fWidth);
}

SkScalar font_advance(const DrawnGlyph& glyph) {
    SkScalar advance = 0;
    glyph.font.getWidths({&glyph.glyph, 1}, {&advance, 1});
    return advance;
}

}  // namespace skia::textlayout::coretext_test
