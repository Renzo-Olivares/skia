// Copyright 2026 Google LLC
#include "modules/skparagraph/src/CoreTextFontRegistry.h"

#include "include/core/SkFontMgr.h"
#include "include/core/SkFourByteTag.h"
#include "include/core/SkSpan.h"
#include "include/core/SkStream.h"
#include "include/core/SkString.h"
#include "include/ports/SkTypeface_mac.h"
#include "include/private/SkTemplates.h"
#include "include/private/SkTo.h"
#include "modules/skparagraph/include/FontCollection.h"
#include "modules/skparagraph/include/TextStyle.h"
#include "modules/skparagraph/src/FontCollectionFaceCache.h"
#include "src/core/SkNoDestructor.h"
#include "src/utils/mac/SkUniqueCFRef.h"

#include <dlfcn.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <string_view>

namespace skia {
namespace textlayout {

namespace {

// Returns a CFData over base[0, len), bytes that stream owns, and hands stream to the CFData:
// descriptors parsed from it stay in fDescriptorCache and back CTFonts after the caller of
// openStream and even the typeface are gone, matching cfdata_from_skdata in SkTypeface_mac_ct.cpp
SkUniqueCFRef<CFDataRef> cfdata_owning_stream(std::unique_ptr<SkStreamAsset> stream,
                                              const void* base,
                                              size_t len) {
    CFAllocatorContext context = {};
    context.info = stream.get();
    context.deallocate = [](void*, void* info) { delete static_cast<SkStreamAsset*>(info); };
    SkUniqueCFRef<CFAllocatorRef> deallocator(CFAllocatorCreate(kCFAllocatorDefault, &context));
    SkASSERT_RELEASE(deallocator);
    SkUniqueCFRef<CFDataRef> data(CFDataCreateWithBytesNoCopy(
            kCFAllocatorDefault, static_cast<const UInt8*>(base), len, deallocator.get()));
    SkASSERT_RELEASE(data);
    stream.release();
    return data;
}

// Returns an owning reference to a borrowed non-null CF object; a template so the result keeps
// the caller's CF ref type
template <typename CFRef> SkUniqueCFRef<CFRef> cf_retain(CFRef ref) {
    SkASSERT(ref);
    CFRetain(ref);
    return SkUniqueCFRef<CFRef>(ref);
}

// Apple's system UI designs, selected through the CoreText traits dictionary key underlying
// NSFontDescriptorSystemDesign / UIFontDescriptorSystemDesign
enum class SystemUiDesign { kDefault, kRounded, kSerif, kMonospaced };

struct SystemUiDesignTrait {
    SystemUiDesign design;
    std::string_view familySuffix;
    CFStringRef trait;  // NSCTFontUIFontDesignTrait value; null keeps the default design
};

// Indexed by SystemUiDesign; CoreText exports no public constants for the trait values
const std::array<SystemUiDesignTrait, 4>& system_ui_design_traits() {
    static const std::array<SystemUiDesignTrait, 4> kTraits = {{
            {SystemUiDesign::kDefault, "", nullptr},
            {SystemUiDesign::kRounded, "Rounded", CFSTR("NSCTFontUIFontDesignRounded")},
            {SystemUiDesign::kSerif, "Serif", CFSTR("NSCTFontUIFontDesignSerif")},
            {SystemUiDesign::kMonospaced, "Monospaced", CFSTR("NSCTFontUIFontDesignMonospaced")},
    }};
    return kTraits;
}

CFStringRef system_ui_design_trait(SystemUiDesign design) {
    const SystemUiDesignTrait& entry = system_ui_design_traits()[static_cast<size_t>(design)];
    SkASSERT(entry.design == design);
    return entry.trait;
}

// Returns the design whose suffix (Rounded, Serif, Monospaced) ends familyName, else kDefault
SystemUiDesign system_ui_design_for_family(const SkString& familyName) {
    std::string_view name(familyName.c_str(), familyName.size());
    for (const SystemUiDesignTrait& entry : system_ui_design_traits()) {
        if (!entry.familySuffix.empty() && name.ends_with(entry.familySuffix)) {
            return entry.design;
        }
    }
    return SystemUiDesign::kDefault;
}

// Maps [0, 1000] CSS weights linearly across Apple's UIFontWeight / NSFontWeight table,
// matching Skia's CoreText typeface port without linking an unexported core symbol
CGFloat ct_weight_for_css_weight(int cssWeight) {
    static constexpr std::array<CGFloat, 11> kDefaultWeights{
            -1.00, -0.80, -0.60, -0.40, 0.00, 0.23, 0.30, 0.40, 0.56, 0.62, 1.00};
#if defined(SK_BUILD_FOR_IOS)
#define SK_PARAGRAPH_KIT_WEIGHT_PREFIX "UI"
#else
#define SK_PARAGRAPH_KIT_WEIGHT_PREFIX "NS"
#endif
    static constexpr std::array<const char*, 9> kWeightNames{
            SK_PARAGRAPH_KIT_WEIGHT_PREFIX "FontWeightUltraLight",
            SK_PARAGRAPH_KIT_WEIGHT_PREFIX "FontWeightThin",
            SK_PARAGRAPH_KIT_WEIGHT_PREFIX "FontWeightLight",
            SK_PARAGRAPH_KIT_WEIGHT_PREFIX "FontWeightRegular",
            SK_PARAGRAPH_KIT_WEIGHT_PREFIX "FontWeightMedium",
            SK_PARAGRAPH_KIT_WEIGHT_PREFIX "FontWeightSemibold",
            SK_PARAGRAPH_KIT_WEIGHT_PREFIX "FontWeightBold",
            SK_PARAGRAPH_KIT_WEIGHT_PREFIX "FontWeightHeavy",
            SK_PARAGRAPH_KIT_WEIGHT_PREFIX "FontWeightBlack",
    };
#undef SK_PARAGRAPH_KIT_WEIGHT_PREFIX

    // Read once per process; without AppKit/UIKit in the process the kit constants are missing
    // and the whole table falls back to kDefaultWeights, the ends stay at -1 and 1
    static const std::array<CGFloat, 11> weights = [] {
        std::array<CGFloat, 11> loaded = kDefaultWeights;
        for (size_t i = 0; i < kWeightNames.size(); ++i) {
            void* ptr = dlsym(RTLD_DEFAULT, kWeightNames[i]);
            if (!ptr) {
                return kDefaultWeights;
            }
            loaded[i + 1] = *static_cast<const CGFloat*>(ptr);
        }
        return loaded;
    }();

    if (cssWeight <= 0) {
        return weights[0];
    }
    if (cssWeight >= 1000) {
        return weights[10];
    }
    int step = cssWeight / 100;
    return weights[step] + ((cssWeight - step * 100) * (weights[step + 1] - weights[step])) / 100.0;
}

// SkFontStyle widths [0, 10] map linearly onto CoreText's [-0.5, 0.5], as in
// SkCTFontCTWidthForCSSWidth
CGFloat ct_width_for_css_width(int cssWidth) { return cssWidth / 10.0 - 0.5; }

// Adds key -> a CFNumber of type read from value to dict
void add_number(CFMutableDictionaryRef dict,
                CFStringRef key,
                CFNumberType type,
                const void* value) {
    SkUniqueCFRef<CFNumberRef> number(CFNumberCreate(kCFAllocatorDefault, type, value));
    SkASSERT_RELEASE(number);
    CFDictionaryAddValue(dict, key, number.get());
}

// Returns a non-null descriptor whose attributes map keys[i] to values[i]
SkUniqueCFRef<CTFontDescriptorRef> make_descriptor(SkSpan<const void*> keys,
                                                   SkSpan<const void*> values) {
    SkASSERT(keys.size() == values.size());
    SkUniqueCFRef<CFDictionaryRef> attrs(CFDictionaryCreate(kCFAllocatorDefault,
                                                            keys.data(),
                                                            values.data(),
                                                            SkTo<CFIndex>(keys.size()),
                                                            &kCFTypeDictionaryKeyCallBacks,
                                                            &kCFTypeDictionaryValueCallBacks));
    SkASSERT_RELEASE(attrs);
    SkUniqueCFRef<CTFontDescriptorRef> desc(CTFontDescriptorCreateWithAttributes(attrs.get()));
    SkASSERT_RELEASE(desc);
    return desc;
}

// Builds a mutable CoreText traits dictionary with style's weight, slant, and optional width
SkUniqueCFRef<CFMutableDictionaryRef> make_base_traits(SkFontStyle style, bool includeNormalWidth) {
    SkUniqueCFRef<CFMutableDictionaryRef> cfTraits(
            CFDictionaryCreateMutable(kCFAllocatorDefault,
                                      0,
                                      &kCFTypeDictionaryKeyCallBacks,
                                      &kCFTypeDictionaryValueCallBacks));
    SkASSERT_RELEASE(cfTraits);
    CGFloat ctWeight = ct_weight_for_css_weight(style.weight());
    add_number(cfTraits.get(), kCTFontWeightTrait, kCFNumberCGFloatType, &ctWeight);
    if (includeNormalWidth || style.width() != SkFontStyle::kNormal_Width) {
        CGFloat ctWidth = ct_width_for_css_width(style.width());
        add_number(cfTraits.get(), kCTFontWidthTrait, kCFNumberCGFloatType, &ctWidth);
    }

    // Same slope as SkFontMgr_mac_ct.cpp (taken from WebKit), so an italic request matches the
    // faces Skia's CoreText font manager would pick
    constexpr CGFloat kSystemFontItalicSlope = 0.07;
    CGFloat ctSlant = (style.slant() == SkFontStyle::kUpright_Slant) ? 0.0 : kSystemFontItalicSlope;
    add_number(cfTraits.get(), kCTFontSlantTrait, kCFNumberCGFloatType, &ctSlant);
    return cfTraits;
}

// Returns a descriptor carrying kCTFontTraitsAttribute for restyling .AppleSystemUIFont to style
// and design
SkUniqueCFRef<CTFontDescriptorRef> make_traits_descriptor(SkFontStyle style,
                                                          SystemUiDesign design) {
    CFStringRef designTrait = system_ui_design_trait(design);
    SkUniqueCFRef<CFMutableDictionaryRef> cfTraits =
            make_base_traits(style, /*includeNormalWidth=*/false);
    // System UI restyling sets kCTFontTraitItalic on non-upright requests and
    // kCTFontTraitUIOptimized alongside NSCTFontUIFontDesignTrait for the UI design
    uint32_t symTraits = (style.slant() != SkFontStyle::kUpright_Slant ? kCTFontTraitItalic : 0) |
                         (designTrait ? kCTFontTraitUIOptimized : 0);
    if (symTraits != 0) {
        add_number(cfTraits.get(), kCTFontSymbolicTrait, kCFNumberSInt32Type, &symTraits);
    }
    // CoreText exports no public constant for the key backing UIFontDescriptorSystemDesign
    if (designTrait) {
        CFDictionaryAddValue(cfTraits.get(), CFSTR("NSCTFontUIFontDesignTrait"), designTrait);
    }
    std::array<const void*, 1> keys = {kCTFontTraitsAttribute};
    std::array<const void*, 1> values = {cfTraits.get()};
    return make_descriptor(keys, values);
}

// .AppleSystemUIFont triggers system cascade lists and optical size selection (Text vs Display)
SkUniqueCFRef<CTFontRef> create_system_ui_font(CGFloat size) {
    // CTFontCreateWithName returns a best match, never null: a miss gives a substitute font
    SkUniqueCFRef<CTFontRef> font(CTFontCreateWithName(CFSTR(".AppleSystemUIFont"), size, nullptr));
    SkASSERT_RELEASE(font);
    return font;
}

// Returns the non-null system UI font at size restyled to style and design
SkUniqueCFRef<CTFontRef> styled_system_ui_font(SkFontStyle style,
                                               SystemUiDesign design,
                                               CGFloat size) {
    SkUniqueCFRef<CTFontRef> sysFont = create_system_ui_font(size);
    SkUniqueCFRef<CTFontDescriptorRef> traitsDesc = make_traits_descriptor(style, design);
    // Copy from sysFont instead of building from traitsDesc alone so optical sizing and the
    // system cascade lists survive restyling
    SkUniqueCFRef<CTFontRef> styled(
            CTFontCreateCopyWithAttributes(sysFont.get(), size, nullptr, traitsDesc.get()));
    SkASSERT_RELEASE(styled);
    return styled;
}

// Converts a non-null CFString to UTF-8
SkString cfstring_to_skstring(CFStringRef str) {
    SkASSERT(str);
    if (const char* cstr = CFStringGetCStringPtr(str, kCFStringEncodingUTF8)) {
        return SkString(cstr);
    }
    const CFIndex maxBytes =
            CFStringGetMaximumSizeForEncoding(CFStringGetLength(str), kCFStringEncodingUTF8) + 1;
    SkString out(SkToSizeT(maxBytes));
    if (!CFStringGetCString(str, out.data(), maxBytes, kCFStringEncodingUTF8)) {
        return SkString();
    }
    out.resize(strlen(out.c_str()));
    return out;
}

// Returns a non-null kCTFontVariationAttribute value for coords, which must not be empty. The
// last value given for an axis wins (css-fonts-4)
SkUniqueCFRef<CFDictionaryRef> make_variation(
        SkSpan<const SkFontArguments::VariationPosition::Coordinate> coords) {
    SkASSERT(!coords.empty());
    SkUniqueCFRef<CFMutableDictionaryRef> variation(
            CFDictionaryCreateMutable(kCFAllocatorDefault,
                                      0,
                                      &kCFTypeDictionaryKeyCallBacks,
                                      &kCFTypeDictionaryValueCallBacks));
    SkASSERT_RELEASE(variation);
    // SetValue replaces an earlier value for the same axis, which gives last-wins
    for (const auto& coord : coords) {
        // CoreFoundation has no unsigned 32-bit CFNumberType; SInt64 keeps tags >= 0x80000000
        // positive instead of sign-extending them
        int64_t axisTag = coord.axis;
        double axisValue = coord.value;
        SkUniqueCFRef<CFNumberRef> tag(
                CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &axisTag));
        SkUniqueCFRef<CFNumberRef> val(
                CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType, &axisValue));
        SkASSERT_RELEASE(tag && val);
        CFDictionarySetValue(variation.get(), tag.get(), val.get());
    }
    return SkUniqueCFRef<CFDictionaryRef>(variation.release());
}

// A family name the system UI font reports, its UI design, and whether varying moved it off
// .AppleSystemUIFont* onto its underlying family (.SF NS, .SF NS Rounded, .New York, .SF NS Mono)
struct SystemUiFamily {
    SkUniqueCFRef<CFStringRef> name;
    SkString utf8Name;
    SystemUiDesign design;
    bool varied;
};

SystemUiFamily make_system_ui_family(CTFontRef ctFont, SystemUiDesign design, bool varied) {
    SkUniqueCFRef<CFStringRef> cfName(CTFontCopyFamilyName(ctFont));
    SkASSERT_RELEASE(cfName);
    SkString utf8 = cfstring_to_skstring(cfName.get());
    return {std::move(cfName), std::move(utf8), design, varied};
}

// Returns the eight system UI family entries, queried from CoreText once per process: varying wght
// and opsz moves each UI design off .AppleSystemUIFont* to its underlying family, while cascade
// faces such as .SF Arabic stay out
const std::array<SystemUiFamily, 8>& system_ui_families() {
    static const SkNoDestructor<std::array<SystemUiFamily, 8>> kFamilies([] {
        // Rounded and Monospaced expose wght without opsz, so probing both axes renames all four
        // designs to the family names FontCollection::cloneTypeface produces
        constexpr std::array<SkFontArguments::VariationPosition::Coordinate, 2> kProbeCoords = {{
                {SkSetFourByteTag('w', 'g', 'h', 't'), 500.0f},
                {SkSetFourByteTag('o', 'p', 's', 'z'), 20.0f},
        }};
        SkUniqueCFRef<CFDictionaryRef> probeAttrs =
                CoreTextFontRegistry::makeVariationAttrs(make_variation(kProbeCoords).get());
        SkUniqueCFRef<CTFontDescriptorRef> probeDesc(
                CTFontDescriptorCreateWithAttributes(probeAttrs.get()));
        SkASSERT_RELEASE(probeDesc);
        std::array<SystemUiFamily, 8> families;
        const std::array<SystemUiDesignTrait, 4>& designs = system_ui_design_traits();
        for (size_t i = 0; i < designs.size(); ++i) {
            SystemUiDesign design = designs[i].design;
            SkUniqueCFRef<CTFontRef> font = styled_system_ui_font(SkFontStyle(), design, 0.0);
            SkUniqueCFRef<CTFontRef> clone(
                    CTFontCreateCopyWithAttributes(font.get(), 0.0, nullptr, probeDesc.get()));
            SkASSERT_RELEASE(clone);
            families[2 * i] = make_system_ui_family(font.get(), design, false);
            families[2 * i + 1] = make_system_ui_family(clone.get(), design, true);
        }
        return families;
    }());
    return *kFamilies;
}

const SystemUiFamily* find_system_ui_family_by_name(CFStringRef familyName) {
    if (!familyName) {
        return nullptr;
    }
    for (const SystemUiFamily& family : system_ui_families()) {
        if (CFEqual(family.name.get(), familyName)) {
            return &family;
        }
    }
    return nullptr;
}

// Returns the system UI entry for ctFont's family, or null for a null or non-system font
const SystemUiFamily* find_system_ui_family(CTFontRef ctFont) {
    if (!ctFont) {
        return nullptr;
    }
    // Subsetted CGFont-backed CTFonts with no SFNT name table return a null family name
    SkUniqueCFRef<CFStringRef> familyName(CTFontCopyFamilyName(ctFont));
    return find_system_ui_family_by_name(familyName.get());
}

// Returns the UI design for a system UI typeface (kDefault for a null typeface), or std::nullopt
// for other faces
std::optional<SystemUiDesign> system_ui_design(const SkTypeface* typeface) {
    if (!typeface) {
        return SystemUiDesign::kDefault;
    }
    // Faces that are not CTFont-backed (custom or test typefaces) are never the system font
    const SystemUiFamily* family = find_system_ui_family(SkTypeface_GetCTFontRef(typeface));
    return family ? std::optional<SystemUiDesign>(family->design) : std::nullopt;
}

// Returns a descriptor that requests familyName in style; null for an empty or ill-formed UTF-8
// family name
SkUniqueCFRef<CTFontDescriptorRef> make_family_descriptor(const SkString& familyName,
                                                          SkFontStyle style) {
    if (familyName.isEmpty()) {
        return nullptr;
    }
    SkUniqueCFRef<CFStringRef> cfFamily(CFStringCreateWithCString(
            kCFAllocatorDefault, familyName.c_str(), kCFStringEncodingUTF8));
    if (!cfFamily) {
        return nullptr;
    }
    // Installed families pass weight, width and slant without kCTFontSymbolicTrait, matching
    // SkFontMgr_Mac's create_descriptor so FontCollection and CoreText pick the same face
    SkUniqueCFRef<CFMutableDictionaryRef> traits =
            make_base_traits(style, /*includeNormalWidth=*/true);
    std::array<const void*, 2> keys = {kCTFontFamilyNameAttribute, kCTFontTraitsAttribute};
    std::array<const void*, 2> values = {cfFamily.get(), traits.get()};
    return make_descriptor(keys, values);
}

// True for names that start with .AppleSystemUIFont, .SF or .NewYork, for the Flutter aliases
// CupertinoSystemText and CupertinoSystemDisplay, and for an empty name
bool is_system_ui_alias(std::string_view name) {
    // An empty family name requests the platform default UI font (.AppleSystemUIFont)
    if (name.empty()) {
        return true;
    }
    // Flutter's CupertinoTextThemeData passes CupertinoSystemText and CupertinoSystemDisplay as
    // family names; treating them as Apple system UI font aliases prevents findTypefaces from
    // skipping them and falling through to the first fontFamilyFallback entry
    return name.starts_with(".AppleSystemUIFont") || name.starts_with(".SF") ||
           name.starts_with(".NewYork") || name == "CupertinoSystemText" ||
           name == "CupertinoSystemDisplay";
}

// Returns a typeface for familyName in style, resolving system UI aliases directly and requiring
// an exact family match for installed fonts (nullptr when no family matches)
sk_sp<SkTypeface> create_platform_family(const SkString& familyName, SkFontStyle style) {
    // System UI names and aliases are not in CoreText's family table, so mandatory family
    // matching below would reject them; 0.0 instantiates at CoreText's default 12pt size
    if (is_system_ui_alias({familyName.c_str(), familyName.size()})) {
        SkUniqueCFRef<CTFontRef> sysFont =
                styled_system_ui_font(style, system_ui_design_for_family(familyName), 0.0);
        sk_sp<SkTypeface> tf = SkMakeTypefaceFromCTFont(sysFont.get());
        SkASSERT_RELEASE(tf);
        return tf;
    }

    SkUniqueCFRef<CTFontDescriptorRef> request = make_family_descriptor(familyName, style);
    if (!request) {
        return nullptr;
    }
    // Only the family is mandatory, so a missing family yields null instead of whichever
    // installed family CoreText considers closest
    static const SkNoDestructor<SkUniqueCFRef<CFSetRef>> kMandatoryFamily([] {
        const void* key = kCTFontFamilyNameAttribute;
        SkUniqueCFRef<CFSetRef> set(
                CFSetCreate(kCFAllocatorDefault, &key, 1, &kCFTypeSetCallBacks));
        SkASSERT_RELEASE(set);
        return set;
    }());
    // CoreText's nearest-trait match, as in SkFontMgr_Mac::onMatchFamilyStyle, which
    // FontCollection::matchTypeface tries first, so both engines pick the same installed face
    SkUniqueCFRef<CTFontDescriptorRef> resolved(
            CTFontDescriptorCreateMatchingFontDescriptor(request.get(), kMandatoryFamily->get()));
    if (!resolved) {
        return nullptr;
    }
    // Size 0.0 picks CoreText's default 12pt size as in SkFontMgr_Mac, so SkMakeTypefaceFromCTFont
    // hits Skia's CFEqual-keyed typeface cache while createFont later applies the run's size
    SkUniqueCFRef<CTFontRef> font(CTFontCreateWithFontDescriptor(resolved.get(), 0.0, nullptr));
    SkASSERT_RELEASE(font);
    sk_sp<SkTypeface> tf = SkMakeTypefaceFromCTFont(font.get());
    SkASSERT_RELEASE(tf);
    return tf;
}

// Attaches non-CTFont typeface variation coordinates (e.g. Fontations clones whose openStream
// serves raw unvaried SFNT bytes) to desc so primary and cascade descriptors preserve them
SkUniqueCFRef<CTFontDescriptorRef> apply_typeface_variations(
        SkTypeface* typeface, SkUniqueCFRef<CTFontDescriptorRef> desc) {
    const int axisCount = typeface->getVariationDesignPosition({});
    if (axisCount <= 0) {
        return desc;
    }
    skia_private::AutoSTMalloc<8, SkFontArguments::VariationPosition::Coordinate> coords(axisCount);
    if (typeface->getVariationDesignPosition({coords.get(), SkToSizeT(axisCount)}) == axisCount) {
        SkUniqueCFRef<CFDictionaryRef> attrs = CoreTextFontRegistry::makeVariationAttrs(
                make_variation({coords.get(), SkToSizeT(axisCount)}).get());
        desc.reset(CTFontDescriptorCreateCopyWithAttributes(desc.get(), attrs.get()));
        SkASSERT_RELEASE(desc);
    }
    return desc;
}

// Parses typeface's font data into CoreText descriptors and returns the one for its ttcIndex
// face with any cloned variation coordinates applied; null when the face has no readable data or
// CoreText cannot parse it
SkUniqueCFRef<CTFontDescriptorRef> descriptor_from_font_data(SkTypeface* typeface) {
    int ttcIndex = 0;
    std::unique_ptr<SkStreamAsset> stream = typeface->openStream(&ttcIndex);
    if (!stream || !stream->hasLength()) {
        return nullptr;
    }
    size_t len = stream->getLength();
    if (len == 0) {
        return nullptr;
    }
    SkUniqueCFRef<CFDataRef> cfData;
    // Memory-backed font streams share their underlying buffer directly to avoid copying
    // multi-megabyte font files on descriptor creation; the CFData keeps the stream alive
    if (const void* base = stream->getMemoryBase()) {
        cfData = cfdata_owning_stream(std::move(stream), base, len);
    } else {
        stream->rewind();
        SkUniqueCFRef<CFMutableDataRef> mutableData(CFDataCreateMutable(kCFAllocatorDefault, len));
        SkASSERT_RELEASE(mutableData);
        CFDataSetLength(mutableData.get(), len);
        if (stream->read(CFDataGetMutableBytePtr(mutableData.get()), len) == len) {
            cfData.reset(mutableData.release());
        }
    }
    if (!cfData) {
        return nullptr;
    }
    // CTFontManagerCreateFontDescriptorsFromData creates in-memory descriptors
    // without fontd IPC roundtrips
    SkUniqueCFRef<CFArrayRef> descs(CTFontManagerCreateFontDescriptorsFromData(cfData.get()));
    if (!descs) {
        return nullptr;
    }
    CFIndex count = CFArrayGetCount(descs.get());
    if (count == 0) {
        return nullptr;
    }
    // TODO(jlavrova): an out-of-range ttcIndex silently shapes with face 0 — remove when it is
    // shown whether CoreText can parse fewer faces than the typeface's collection holds
    CFIndex idx = (ttcIndex >= 0 && ttcIndex < count) ? ttcIndex : 0;
    // A CFArray of CF types holds no null elements and idx is in range
    CTFontDescriptorRef d = (CTFontDescriptorRef)CFArrayGetValueAtIndex(descs.get(), idx);
    return apply_typeface_variations(typeface, cf_retain(d));
}

// Builds an uncached non-null descriptor for a non-null typeface: a system UI face gets a
// .AppleSystemUIFont descriptor with its weight, slant and design, any other face one parsed
// from its font data, else one for its family name and style, else plain .AppleSystemUIFont
SkUniqueCFRef<CTFontDescriptorRef> create_descriptor(SkTypeface* typeface) {
    SkASSERT(typeface);
    // Apple system UI font requires dynamic instantiation to resolve optical sizing and system
    // cascade
    if (std::optional<SystemUiDesign> design = system_ui_design(typeface)) {
        SkUniqueCFRef<CTFontRef> sysFont =
                styled_system_ui_font(CoreTextFontRegistry::faceStyle(typeface), *design, 0.0);
        SkUniqueCFRef<CTFontDescriptorRef> desc(CTFontCopyFontDescriptor(sysFont.get()));
        SkASSERT_RELEASE(desc);
        return desc;
    }

    if (SkUniqueCFRef<CTFontDescriptorRef> desc = descriptor_from_font_data(typeface)) {
        return desc;
    }

    SkString familyName;
    typeface->getFamilyName(&familyName);
    if (SkUniqueCFRef<CTFontDescriptorRef> desc =
                make_family_descriptor(familyName, typeface->fontStyle())) {
        return desc;
    }

    // Degrade to plain .AppleSystemUIFont (0.0 = default 12pt size) when a custom SkTypeface has
    // neither readable stream bytes nor a family name, so cascade building still gets a descriptor
    SkUniqueCFRef<CTFontRef> sysFont = create_system_ui_font(0.0);
    SkUniqueCFRef<CTFontDescriptorRef> desc(CTFontCopyFontDescriptor(sysFont.get()));
    SkASSERT_RELEASE(desc);
    return desc;
}

}  // namespace

SkUniqueCFRef<CFDictionaryRef> CoreTextFontRegistry::makeVariationAttrs(CFDictionaryRef variation) {
    SkASSERT(variation);
    const void* key = kCTFontVariationAttribute;
    const void* value = variation;
    SkUniqueCFRef<CFDictionaryRef> attrs(CFDictionaryCreate(kCFAllocatorDefault,
                                                            &key,
                                                            &value,
                                                            1,
                                                            &kCFTypeDictionaryKeyCallBacks,
                                                            &kCFTypeDictionaryValueCallBacks));
    SkASSERT_RELEASE(attrs);
    return attrs;
}

CoreTextFontRegistry& CoreTextFontRegistry::Instance() {
    // Never destroyed: a worker thread may still hold a cache mutex while exit() runs destructors
    static SkNoDestructor<CoreTextFontRegistry> registry;
    return *registry;
}

void CoreTextFontRegistry::clearCaches() {
    SkAutoMutexExclusive lock(fCacheMutex);
    fDescriptorCache.reset();
    fPlatformFamilyCache.reset();
}

SkFontStyle CoreTextFontRegistry::faceStyle(const SkTypeface* typeface) {
    SkASSERT(typeface);
    SkFontStyle style = typeface->fontStyle();
    if (style.slant() != SkFontStyle::kUpright_Slant) {
        return style;
    }
    CTFontRef ctFont = SkTypeface_GetCTFontRef(typeface);
    if (!ctFont) {
        return style;
    }
    if ((CTFontGetSymbolicTraits(ctFont) & kCTFontTraitItalic) == 0 &&
        CTFontGetSlantAngle(ctFont) == 0) {
        return style;
    }
    return SkFontStyle(style.weight(), style.width(), SkFontStyle::kItalic_Slant);
}

SkUniqueCFRef<CTFontDescriptorRef> CoreTextFontRegistry::copyDescriptor(SkTypeface* typeface) {
    // Callers pass faces from findTypefaces, which drops nulls, and createFont sends a null
    // typeface down its system UI branch before reaching here
    SkASSERT(typeface);

    // A CTFont-backed face already owns its descriptor, so copying it is a retain and caching
    // it would only duplicate state the typeface keeps alive
    if (!isSystemUiFace(typeface)) {
        if (CTFontRef ctFont = SkTypeface_GetCTFontRef(typeface)) {
            SkUniqueCFRef<CTFontDescriptorRef> desc(CTFontCopyFontDescriptor(ctFont));
            SkASSERT_RELEASE(desc);
            return desc;
        }
    }

    SkTypefaceID id = typeface->uniqueID();
    {
        SkAutoMutexExclusive lock(fCacheMutex);
        if (auto* found = fDescriptorCache.find(id)) {
            return cf_retain(found->get());
        }
    }

    // Stream reads and CoreText descriptor parsing run outside fCacheMutex so concurrent
    // paragraphs do not block on font I/O
    SkUniqueCFRef<CTFontDescriptorRef> desc = create_descriptor(typeface);
    SkAutoMutexExclusive lock(fCacheMutex);
    fDescriptorCache.insert_or_update(id, cf_retain(desc.get()));
    return desc;
}

SkUniqueCFRef<CTFontRef> CoreTextFontRegistry::createFont(SkTypeface* typeface,
                                                          SkFontStyle style,
                                                          CGFloat size) {
    // The single default for a non-positive TextStyle size lives in the callers (14, the same as
    // the shaping path), so a zero here would silently become CoreText's own default of 12
    SkASSERT(size > 0);

    if (std::optional<SystemUiDesign> design = system_ui_design(typeface)) {
        return styled_system_ui_font(style, *design, size);
    }

    // Copy directly from a backing CTFont so unregistered CGFont-backed faces (whose synthetic
    // PostScript name CTFontCreateWithFontDescriptor cannot look up) keep their underlying CGFont
    if (CTFontRef ctFont = SkTypeface_GetCTFontRef(typeface)) {
        SkUniqueCFRef<CTFontRef> font(
                CTFontCreateCopyWithAttributes(ctFont, size, nullptr, nullptr));
        SkASSERT_RELEASE(font);
        return font;
    }

    SkUniqueCFRef<CTFontDescriptorRef> desc = this->copyDescriptor(typeface);
    SkUniqueCFRef<CTFontRef> font(CTFontCreateWithFontDescriptor(desc.get(), size, nullptr));
    SkASSERT_RELEASE(font);
    return font;
}

std::vector<sk_sp<SkTypeface>> CoreTextFontRegistry::findTypefaces(
        FontCollection& fontCollection,
        const std::vector<SkString>& familyNames,
        SkFontStyle fontStyle,
        const std::optional<FontArguments>& fontArgs) {
    FontCollection::FaceCache::FamilyKey familyKey{
            familyNames, fontStyle, fontArgs, /*coreText=*/true};
    if (auto cached = fontCollection.fFaceCache->find(familyKey)) {
        return std::move(*cached);
    }

    std::vector<sk_sp<SkTypeface>> typefaces;
    for (const SkString& familyName : familyNames) {
        // SkTypeface::makeClone returns null when variation cloning fails; drop null entries
        // before checking empty() so the next family or standIn takes over
        sk_sp<SkTypeface> match = WithFontArgs(
                fontCollection, this->matchFamily(fontCollection, familyName, fontStyle), fontArgs);
        if (match) {
            typefaces.push_back(std::move(match));
        }
    }
    if (typefaces.empty()) {
        if (sk_sp<SkTypeface> face =
                    this->standIn(fontCollection, familyNames, fontStyle, fontArgs)) {
            typefaces.push_back(std::move(face));
        }
    }
    return fontCollection.fFaceCache->insert(familyKey, std::move(typefaces));
}

std::vector<sk_sp<SkTypeface>> CoreTextFontRegistry::resolveTypefaces(
        FontCollection& fontCollection, const TextStyle& style) {
    sk_sp<SkTypeface> typeface = style.refTypeface();
    const std::optional<FontArguments>& fontArgs = style.getFontArguments();
    if (!typeface) {
        return this->findTypefaces(
                fontCollection, style.getFontFamilies(), style.getFontStyle(), fontArgs);
    }
    if (fontArgs) {
        // Cloned through FontCollection's variation cache even for system UI faces so
        // computeEmptyMetrics, placeholders and LastResort runs get the varied SkTypeface; a
        // face the arguments cannot vary keeps the caller's face instead of dropping to system UI
        if (sk_sp<SkTypeface> clone = fontCollection.cloneTypeface(typeface, *fontArgs)) {
            typeface = std::move(clone);
        }
    }
    return {std::move(typeface)};
}

sk_sp<SkTypeface> CoreTextFontRegistry::matchFamily(const FontCollection& fontCollection,
                                                    const SkString& familyName,
                                                    SkFontStyle fontStyle) {
    // Flutter prepends "" ahead of fontFamilyFallback when fontFamily is null; skipping empty
    // names lets later families resolve before standIn falls back to .AppleSystemUIFont
    if (familyName.isEmpty()) {
        return nullptr;
    }
    // A face the client registered under a family name wins over an installed one of the same
    // name, in the same dynamic -> asset -> test order as getFontManagerOrder
    for (SkFontMgr* manager : UserFontManagers(fontCollection)) {
        if (!manager) {
            continue;
        }
        sk_sp<SkFontStyleSet> set(manager->matchFamily(familyName.c_str()));
        if (!set || set->count() == 0) {
            continue;
        }
        if (sk_sp<SkTypeface> match = set->matchStyle(fontStyle)) {
            return match;
        }
    }
    // When the client registered user font managers without a defaultFontManager (e.g. isolated
    // asset-only tests), do not resolve system families ahead of later user families in the list
    if (!fontCollection.fDefaultFontManager &&
        (fontCollection.fDynamicFontManager || fontCollection.fAssetFontManager ||
         fontCollection.fTestFontManager)) {
        return nullptr;
    }
    // disableFontFallback drops the default manager from getFontManagerOrder, so FontCollection
    // sees only user faces; installed families stay hidden here for the same reason
    if (!fontCollection.fEnableFontFallback) {
        return nullptr;
    }
    return this->matchPlatformFamily(familyName, fontStyle);
}

sk_sp<SkTypeface> CoreTextFontRegistry::standIn(FontCollection& fontCollection,
                                                SkSpan<const SkString> triedFamilies,
                                                SkFontStyle fontStyle,
                                                const std::optional<FontArguments>& fontArgs) {
    for (const SkString& familyName : fontCollection.fDefaultFamilyNames) {
        if (std::find(triedFamilies.begin(), triedFamilies.end(), familyName) !=
            triedFamilies.end()) {
            continue;
        }
        if (sk_sp<SkTypeface> match =
                    WithFontArgs(fontCollection,
                                 this->matchFamily(fontCollection, familyName, fontStyle),
                                 fontArgs)) {
            return match;
        }
    }
    // Same order as getFontManagerOrder: user registries give the stand-in first; the
    // system one stands in last and only with fallback enabled
    for (SkFontMgr* manager : UserFontManagers(fontCollection)) {
        if (!manager) {
            continue;
        }
        // legacyMakeTypeface(nullptr, ...) picks TypefaceFontProvider's first family, whereas
        // matchFamilyStyle(nullptr, ...) looks up a null family name and returns null
        if (sk_sp<SkTypeface> match = WithFontArgs(
                    fontCollection, manager->legacyMakeTypeface(nullptr, fontStyle), fontArgs)) {
            return match;
        }
    }
    if (!fontCollection.fEnableFontFallback) {
        return nullptr;
    }
    return WithFontArgs(fontCollection,
                        this->matchPlatformFamily(SkString(".AppleSystemUIFont"), fontStyle),
                        fontArgs);
}

sk_sp<SkTypeface> CoreTextFontRegistry::WithFontArgs(FontCollection& fontCollection,
                                                     sk_sp<SkTypeface> face,
                                                     const std::optional<FontArguments>& fontArgs) {
    return (face && fontArgs) ? fontCollection.cloneTypeface(face, *fontArgs) : face;
}

std::array<SkFontMgr*, 3> CoreTextFontRegistry::UserFontManagers(
        const FontCollection& fontCollection) {
    return {fontCollection.fDynamicFontManager.get(),
            fontCollection.fAssetFontManager.get(),
            fontCollection.fTestFontManager.get()};
}

sk_sp<SkTypeface> CoreTextFontRegistry::matchPlatformFamily(const SkString& familyName,
                                                            SkFontStyle fontStyle) {
    PlatformFamilyKey key{familyName, fontStyle};
    {
        SkAutoMutexExclusive lock(fCacheMutex);
        if (sk_sp<SkTypeface>* found = fPlatformFamilyCache.find(key)) {
            return *found;
        }
    }
    // Resolved outside the lock: descriptor matching may wait on fontd, and a racing thread
    // computes the same face, so the later insert only refreshes an equal entry
    sk_sp<SkTypeface> typeface = create_platform_family(familyName, fontStyle);
    // A miss stays uncached so a font registered later in the process can still match
    if (!typeface) {
        return nullptr;
    }
    SkAutoMutexExclusive lock(fCacheMutex);
    fPlatformFamilyCache.insert_or_update(std::move(key), typeface);
    return typeface;
}

bool CoreTextFontRegistry::isSystemUiFace(const SkTypeface* tf) {
    return system_ui_design(tf).has_value();
}

bool CoreTextFontRegistry::sameSystemFontDesign(CFStringRef ctFamilyName,
                                                const SkTypeface* typeface,
                                                std::string_view typefaceFamilyName) {
    // Non-CTFont faces (asset or test fonts) never match a system UI design even by family name
    if (!typeface || !SkTypeface_GetCTFontRef(typeface)) {
        return false;
    }
    const SystemUiFamily* a = find_system_ui_family_by_name(ctFamilyName);
    if (!a) {
        return false;
    }
    for (const SystemUiFamily& b : system_ui_families()) {
        if (std::string_view(b.utf8Name.c_str(), b.utf8Name.size()) == typefaceFamilyName) {
            return a->design == b.design;
        }
    }
    return false;
}

SkUniqueCFRef<CFDictionaryRef> CoreTextFontRegistry::makeSpanVariation(
        const SkTypeface* typeface,
        SkSpan<const SkFontArguments::VariationPosition::Coordinate> extraCoords) {
    CTFontRef ctFont = typeface ? SkTypeface_GetCTFontRef(typeface) : nullptr;
    const SystemUiFamily* family = find_system_ui_family(ctFont);
    // getVariationDesignPosition fills fvar defaults that CTFontCopyVariation omits on macOS
    // 10.12+, while skipping unvaried .AppleSystemUIFont* leaves point-size opsz intact
    const int baseCount = (family && family->varied) ? typeface->getVariationDesignPosition({}) : 0;
    if (baseCount <= 0) {
        return extraCoords.empty() ? nullptr : make_variation(extraCoords);
    }
    const size_t baseLen = SkToSizeT(baseCount);
    skia_private::AutoSTMalloc<8, SkFontArguments::VariationPosition::Coordinate> merged(
            baseLen + extraCoords.size());
    const int read = typeface->getVariationDesignPosition({merged.get(), baseLen});
    const size_t kept = read == baseCount ? baseLen : 0;
    std::copy(extraCoords.begin(), extraCoords.end(), merged.get() + kept);
    const size_t total = kept + extraCoords.size();
    return total > 0 ? make_variation({merged.get(), total}) : nullptr;
}

}  // namespace textlayout
}  // namespace skia
