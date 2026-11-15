// Copyright 2026 Google LLC
#include "modules/skparagraph/src/CoreTextSpanFonts.h"

#include "include/core/SkSpan.h"
#include "include/ports/SkTypeface_mac.h"
#include "include/private/SkTo.h"
#include "modules/skparagraph/src/CoreTextFontRegistry.h"

#include <array>
#include <vector>

namespace skia {
namespace textlayout {

namespace {

// Returns font restyled by attrs, or font retained when attrs is empty
SkUniqueCFRef<CTFontRef> copy_with_attributes(CTFontRef font,
                                              CGFloat fontSize,
                                              CFDictionaryRef attrs) {
    SkASSERT(font);
    SkASSERT(attrs);
    if (CFDictionaryGetCount(attrs) == 0) {
        return SkUniqueCFRef<CTFontRef>((CTFontRef)CFRetain(font));
    }
    SkUniqueCFRef<CTFontDescriptorRef> desc(CTFontDescriptorCreateWithAttributes(attrs));
    SkASSERT_RELEASE(desc);
    SkUniqueCFRef<CTFontRef> styled(
            CTFontCreateCopyWithAttributes(font, fontSize, nullptr, desc.get()));
    SkASSERT_RELEASE(styled);
    return styled;
}

// TextStyle accepts zero, negative and NaN sizes but CoreText needs a positive point size
// (createFont asserts it), so such a span takes 14, the TextStyle default size
CGFloat span_font_size(const TextStyle& style) {
    return style.getFontSize() > 0.0f ? style.getFontSize() : 14.0f;
}

// Returns a non-null descriptor for Apple's LastResort font at size
SkUniqueCFRef<CTFontDescriptorRef> last_resort_descriptor(CGFloat size) {
    SkUniqueCFRef<CTFontRef> lrFont(CTFontCreateWithName(CFSTR("LastResort"), size, nullptr));
    SkASSERT_RELEASE(lrFont);
    SkUniqueCFRef<CTFontDescriptorRef> lrDesc(CTFontCopyFontDescriptor(lrFont.get()));
    SkASSERT_RELEASE(lrDesc);
    return lrDesc;
}

// Returns style's kCTFontVariationAttribute value, or null without any: a varied system UI
// typeface seeds all its cloned axes (createFont rebuilds system fonts by name) before
// FontArguments override matching ones, while unvaried .AppleSystemUIFont* keeps point-size opsz;
// palette overrides stay HarfBuzz-only
SkUniqueCFRef<CFDictionaryRef> span_variation(const TextStyle& style) {
    SkSpan<const SkFontArguments::VariationPosition::Coordinate> extra;
    if (const auto& fontArgs = style.getFontArguments()) {
        extra = fontArgs->getVariationCoordinates();
    }
    return CoreTextFontRegistry::makeSpanVariation(style.getTypeface(), extra);
}

// Returns the cascade descriptor of system UI face tf varied by variationAttrs, copied from the
// font made at size like the primary one. The cached size-0 descriptor plus wght, with or without
// kCTFontSizeAttribute, loses wght at 24pt on macOS 14 M1 and 15 Intel (200 draws as SF Light)
SkUniqueCFRef<CTFontDescriptorRef> varied_system_cascade_descriptor(
        SkTypeface* tf, CGFloat size, CFDictionaryRef variationAttrs) {
    SkUniqueCFRef<CTFontRef> base = CoreTextFontRegistry::Instance().createFont(
            tf, CoreTextFontRegistry::faceStyle(tf), size);
    SkUniqueCFRef<CTFontRef> varied = copy_with_attributes(base.get(), size, variationAttrs);
    SkUniqueCFRef<CTFontDescriptorRef> desc(CTFontCopyFontDescriptor(varied.get()));
    SkASSERT_RELEASE(desc);
    return desc;
}

// Sets kCTFontCascadeListAttribute in descAttrs to the descriptors of typefaces after the first,
// then LastResort at size when fallback is disabled. A non-null variation from span_variation
// goes onto system UI and non-CTFont entries
void add_cascade_list(CFMutableDictionaryRef descAttrs,
                      SkSpan<const sk_sp<SkTypeface>> typefaces,
                      CGFloat size,
                      bool fallbackDisabled,
                      CFDictionaryRef variation) {
    if (typefaces.size() <= 1 && !fallbackDisabled) {
        return;
    }
    SkUniqueCFRef<CFMutableArrayRef> cascadeList(
            CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks));
    SkASSERT_RELEASE(cascadeList);
    SkUniqueCFRef<CFDictionaryRef> variationAttrs;
    if (variation && typefaces.size() > 1) {
        variationAttrs = CoreTextFontRegistry::makeVariationAttrs(variation);
    }
    for (size_t i = 1; i < typefaces.size(); ++i) {
        SkTypeface* tf = typefaces[i].get();
        if (variation && CoreTextFontRegistry::isSystemUiFace(tf)) {
            SkUniqueCFRef<CTFontDescriptorRef> desc =
                    varied_system_cascade_descriptor(tf, size, variationAttrs.get());
            CFArrayAppendValue(cascadeList.get(), desc.get());
            continue;
        }
        SkUniqueCFRef<CTFontDescriptorRef> desc =
                CoreTextFontRegistry::Instance().copyDescriptor(tf);
        if (variation && !SkTypeface_GetCTFontRef(tf)) {
            desc.reset(CTFontDescriptorCreateCopyWithAttributes(desc.get(), variationAttrs.get()));
            SkASSERT_RELEASE(desc);
        }
        CFArrayAppendValue(cascadeList.get(), desc.get());
    }
    // Omitting or emptying kCTFontCascadeListAttribute still lets CoreText walk global system
    // fallback; LastResort catches unmapped scalars first so CoreTextLineShaper marks them .notdef
    if (fallbackDisabled) {
        SkUniqueCFRef<CTFontDescriptorRef> lrDesc = last_resort_descriptor(size);
        CFArrayAppendValue(cascadeList.get(), lrDesc.get());
    }
    SkASSERT(CFArrayGetCount(cascadeList.get()) > 0);
    CFDictionarySetValue(descAttrs, kCTFontCascadeListAttribute, cascadeList.get());
}

// Appends {tag: value} to featureArray, skipping non-ASCII caller feature tags that
// CFStringCreateWithCString rejects
void append_feature(CFMutableArrayRef featureArray, const char* tag, int value) {
    SkUniqueCFRef<CFStringRef> tagStr(
            CFStringCreateWithCString(kCFAllocatorDefault, tag, kCFStringEncodingASCII));
    if (!tagStr) {
        return;
    }
    SkUniqueCFRef<CFNumberRef> valNum(
            CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &value));
    SkASSERT_RELEASE(valNum);
    std::array<const void*, 2> keys{kCTFontOpenTypeFeatureTag, kCTFontOpenTypeFeatureValue};
    std::array<const void*, 2> vals{tagStr.get(), valNum.get()};
    SkUniqueCFRef<CFDictionaryRef> featDict(CFDictionaryCreate(kCFAllocatorDefault,
                                                               keys.data(),
                                                               vals.data(),
                                                               SkTo<CFIndex>(keys.size()),
                                                               &kCFTypeDictionaryKeyCallBacks,
                                                               &kCFTypeDictionaryValueCallBacks));
    SkASSERT_RELEASE(featDict);
    CFArrayAppendValue(featureArray, featDict.get());
}

struct SpanFeatures {
    // kCTFontFeatureSettingsAttribute value: the user features with a 4-character tag, then
    // liga=0 when the style has letterSpacing; null when that list is empty
    SkUniqueCFRef<CFArrayRef> settings;
    // The non-empty user features without that liga=0 for a letter-spaced span, null otherwise
    SkUniqueCFRef<CFArrayRef> userFeatures;
};

SpanFeatures make_span_features(const TextStyle& style) {
    // CoreText expects 4-character OpenType feature tags packed into descriptor settings
    const auto& features = style.getFontFeatures();
    const SkScalar letterSpacing = style.getLetterSpacing();
    // letterSpacing needs feature settings even without user features; the extra array
    // slot holds its liga=0
    if (features.empty() && letterSpacing == 0) {
        return {};
    }
    SkUniqueCFRef<CFMutableArrayRef> featureArray(CFArrayCreateMutable(
            kCFAllocatorDefault, SkTo<CFIndex>(features.size() + 1), &kCFTypeArrayCallBacks));
    SkASSERT_RELEASE(featureArray);
    for (const auto& feat : features) {
        if (feat.fName.size() == 4) {
            append_feature(featureArray.get(), feat.fName.c_str(), feat.fValue);
        }
    }
    SpanFeatures result;
    // TODO(jlavrova): OneLineShaper keeps liga for negative letterSpacing, while native tracking
    // and CSS Text 3 drop optional ligatures for any nonzero spacing — remove when it uses != 0
    // Spacing goes between clusters, so a ligature would hide it; liga goes off after the
    // user features. Cursive runs get no spacing and keep liga, see setCursiveRunsFont
    if (letterSpacing != 0) {
        if (CFArrayGetCount(featureArray.get()) > 0) {
            result.userFeatures.reset(CFArrayCreateCopy(kCFAllocatorDefault, featureArray.get()));
            SkASSERT_RELEASE(result.userFeatures);
        }
        append_feature(featureArray.get(), "liga", 0);
    }
    if (CFArrayGetCount(featureArray.get()) > 0) {
        result.settings.reset(featureArray.release());
    }
    return result;
}

// Styles whose spans get the same CTFonts: can_coalesce_styles compares sizes only nearly, and
// the CTFont takes the exact stand-in size, which also keeps a NaN size equal to itself
bool same_span_font(const TextStyle& a, const TextStyle& b) {
    return can_coalesce_styles(a, b) && span_font_size(a) == span_font_size(b);
}

}  // namespace

bool can_coalesce_styles(const TextStyle& a, const TextStyle& b) {
    // StyleType::kFont omits explicit fTypeface, features, and whether letterSpacing is nonzero
    // (which sets liga=0 on the span CTFont here), whereas letterSpacing and wordSpacing advances
    // are applied post-shaping per Block
    return a.matchOneAttribute(StyleType::kFont, b) && a.getTypeface() == b.getTypeface() &&
           a.getFontFeatures() == b.getFontFeatures() &&
           (a.getLetterSpacing() == 0) == (b.getLetterSpacing() == 0);
}

CoreTextRequestedFonts::CoreTextRequestedFonts(sk_sp<FontCollection> fontCollection)
        : fFontCollection(std::move(fontCollection)) {
    SkASSERT(fFontCollection);
}

CoreTextRequestedFonts::Entry* CoreTextRequestedFonts::findOrMake(const TextStyle& style) {
    for (Entry& entry : fEntries) {
        if (same_span_font(entry.style(), style)) {
            return &entry;
        }
    }
    const CGFloat size = span_font_size(style);
    // Clones carry the variation in their CTFontDescriptor, so the primary font and every cascade
    // entry shape with the same axes as the HarfBuzz path. Empty when the style names no typeface
    // and no family resolves, so createFont picks the system UI font
    const std::vector<sk_sp<SkTypeface>> typefaces =
            CoreTextFontRegistry::Instance().resolveTypefaces(*fFontCollection, style);
    SkTypeface* primary = typefaces.empty() ? nullptr : typefaces.front().get();
    // An explicit typeface keeps its own weight, width and slant before variation is applied;
    // only a null or family-resolved face takes the TextStyle's
    SkFontStyle fontStyle = style.getTypeface()
                                    ? CoreTextFontRegistry::faceStyle(style.getTypeface())
                                    : style.getFontStyle();
    SkUniqueCFRef<CTFontRef> base =
            CoreTextFontRegistry::Instance().createFont(primary, fontStyle, size);
    SkASSERT(base);
    SkUniqueCFRef<CFMutableDictionaryRef> descAttrs(
            CFDictionaryCreateMutable(kCFAllocatorDefault,
                                      0,
                                      &kCFTypeDictionaryKeyCallBacks,
                                      &kCFTypeDictionaryValueCallBacks));
    SkASSERT_RELEASE(descAttrs);
    SkUniqueCFRef<CFDictionaryRef> variation = span_variation(style);
    add_cascade_list(descAttrs.get(),
                     typefaces,
                     size,
                     !fFontCollection->fontFallbackEnabled(),
                     variation.get());
    SpanFeatures features = make_span_features(style);
    if (features.settings) {
        CFDictionarySetValue(
                descAttrs.get(), kCTFontFeatureSettingsAttribute, features.settings.get());
    }
    if (variation) {
        CFDictionarySetValue(descAttrs.get(), kCTFontVariationAttribute, variation.get());
    }
    SkUniqueCFRef<CTFontRef> font = copy_with_attributes(base.get(), size, descAttrs.get());
    if (style.getLetterSpacing() == 0) {
        return &fEntries.emplace_back(style, std::move(font), nullptr, nullptr);
    }
    // Cascade and variations stay; an empty feature list would still be a setting, so the key
    // goes away when the user gave no features
    SkUniqueCFRef<CFMutableDictionaryRef> cursiveAttrs(
            CFDictionaryCreateMutableCopy(kCFAllocatorDefault, 0, descAttrs.get()));
    SkASSERT_RELEASE(cursiveAttrs);
    if (features.userFeatures) {
        CFDictionarySetValue(
                cursiveAttrs.get(), kCTFontFeatureSettingsAttribute, features.userFeatures.get());
    } else {
        CFDictionaryRemoveValue(cursiveAttrs.get(), kCTFontFeatureSettingsAttribute);
    }
    return &fEntries.emplace_back(style,
                                  std::move(font),
                                  std::move(base),
                                  SkUniqueCFRef<CFDictionaryRef>(cursiveAttrs.release()));
}

CTFontRef CoreTextRequestedFonts::Entry::cursiveFont() {
    SkASSERT(fCursiveAttrs);
    if (fCursiveFont) {
        return fCursiveFont.get();
    }
    fCursiveFont = copy_with_attributes(fBase.get(), span_font_size(fStyle), fCursiveAttrs.get());
    return fCursiveFont.get();
}

}  // namespace textlayout
}  // namespace skia
