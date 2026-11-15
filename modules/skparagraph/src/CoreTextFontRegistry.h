// Copyright 2026 Google LLC
#ifndef CoreTextFontRegistry_DEFINED
#define CoreTextFontRegistry_DEFINED

#include "include/core/SkSpan.h"
#include "include/core/SkString.h"
#include "include/core/SkTypeface.h"
#include "include/core/SkTypes.h"
#include "include/private/SkMutex.h"
#include "modules/skparagraph/include/FontArguments.h"
#include "src/core/SkLRUCache.h"

#include <array>
#include <optional>
#include <string_view>
#include <vector>

#include <CoreText/CoreText.h>
#include "src/utils/mac/SkUniqueCFRef.h"

// Befriended below: Instance() never destroys the registry
template <typename T> class SkNoDestructor;
class SkFontMgr;

namespace skia {
namespace textlayout {

class FontCollection;
class TextStyle;

// Resolves TextStyle families to SkTypefaces and SkTypefaces to CTFonts for the CoreText
// pipeline. Each family is searched in FontCollection's user managers (dynamic, asset, test), then
// installed CoreText families by exact name; with no match: default families, a user manager's
// default face, then .AppleSystemUIFont. FontCollection's default manager is never queried
class CoreTextFontRegistry {
public:
    // Returns the process-wide registry; its caches are shared by every paragraph and guarded by
    // an internal mutex, so any thread may use it
    static CoreTextFontRegistry& Instance();

    // Drops cached parsed descriptors and installed system-family resolutions so newly registered
    // or unregistered process fonts take effect
    void clearCaches();

    // In-process descriptor cache indexed by SkTypefaceID to eliminate fontd IPC during run
    // attribution; typeface must be non-null and the returned descriptor is never null
    SkUniqueCFRef<CTFontDescriptorRef> copyDescriptor(SkTypeface* typeface);

    // Returns a non-null CTFont at size, which must be positive. A system UI face or null typeface
    // becomes .AppleSystemUIFont in style's weight, width, slant and UI design, resolving optical
    // size and symbolic traits; any other CTFont-backed face copies its CTFont, else copyDescriptor
    SkUniqueCFRef<CTFontRef> createFont(SkTypeface* typeface, SkFontStyle style, CGFloat size);

    // Returns the faces for familyNames in the class search order, cloned for fontArgs. Fallback
    // disabled searches only user registries, so the result may be empty. The collection caches
    // the result until its managers, families or fallback setting change
    std::vector<sk_sp<SkTypeface>> findTypefaces(FontCollection& fontCollection,
                                                 const std::vector<SkString>& familyNames,
                                                 SkFontStyle fontStyle,
                                                 const std::optional<FontArguments>& fontArgs);

    // Returns the faces a CoreText span of style shapes with: style's explicit typeface, cloned
    // through fontCollection's variation cache when FontArguments are set, else findTypefaces for
    // its families. May be empty
    std::vector<sk_sp<SkTypeface>> resolveTypefaces(FontCollection& fontCollection,
                                                    const TextStyle& style);

    // Stateless helpers: they read no registry cache

    // Returns the non-null typeface's own style, made italic when its CTFont is: system UI italic
    // faces (.SFNS-RegularItalic) set kCTFontTraitItalic but omit kCTFontSlantTrait from
    // CTFontCopyFontDescriptor, so typeface->fontStyle() alone reports upright
    static SkFontStyle faceStyle(const SkTypeface* typeface);

    // True for a CTFont-backed system UI face (.AppleSystemUIFont in any design, or its opsz clone
    // .SF NS / .New York), which needs descriptor synthesis to keep the system cascade lists across
    // scripts; cascade faces such as .SF Arabic are drawn as given. Also true for a null typeface,
    // which stands for the default .AppleSystemUIFont
    static bool isSystemUiFace(const SkTypeface* typeface);

    // True when ctFamilyName (from a CTFont) and typeface (with family typefaceFamilyName) are
    // CTFont-backed system UI faces of the same UI design, including across .AppleSystemUIFont*
    // and its varied .SF NS* / .New York family
    static bool sameSystemFontDesign(CFStringRef ctFamilyName,
                                     const SkTypeface* typeface,
                                     std::string_view typefaceFamilyName);

    // Returns the kCTFontVariationAttribute dictionary for typeface and extraCoords, or null with
    // no axes: a varied system UI face (.SF NS* / .New York) seeds all its coordinates (including
    // fvar-default opsz) before extraCoords override them, while unvaried .AppleSystemUIFont*
    // seeds none so point-size opsz still applies
    static SkUniqueCFRef<CFDictionaryRef> makeSpanVariation(
            const SkTypeface* typeface,
            SkSpan<const SkFontArguments::VariationPosition::Coordinate> extraCoords);

    // Returns non-null descriptor attributes that hold only kCTFontVariationAttribute = variation
    static SkUniqueCFRef<CFDictionaryRef> makeVariationAttrs(CFDictionaryRef variation);

private:
    friend class SkNoDestructor<CoreTextFontRegistry>;
    CoreTextFontRegistry() = default;
    ~CoreTextFontRegistry() = default;
    CoreTextFontRegistry(const CoreTextFontRegistry&) = delete;
    CoreTextFontRegistry& operator=(const CoreTextFontRegistry&) = delete;

    // Returns the face for one family from the user managers, then matchPlatformFamily unless user
    // managers exist without a default manager or font fallback is disabled; null on no match
    sk_sp<SkTypeface> matchFamily(const FontCollection& fontCollection,
                                  const SkString& familyName,
                                  SkFontStyle fontStyle);

    // Returns the face that stands in when no requested family matched, skipping default families
    // already in triedFamilies; .AppleSystemUIFont only with font fallback enabled. Cloned for
    // fontArgs
    sk_sp<SkTypeface> standIn(FontCollection& fontCollection,
                              SkSpan<const SkString> triedFamilies,
                              SkFontStyle fontStyle,
                              const std::optional<FontArguments>& fontArgs);

    // Returns face cloned for fontArgs through fontCollection's variation cache, or face itself
    // without fontArgs; null for a null face or a failed clone
    static sk_sp<SkTypeface> WithFontArgs(FontCollection& fontCollection,
                                          sk_sp<SkTypeface> face,
                                          const std::optional<FontArguments>& fontArgs);

    // Returns fontCollection's user managers in getFontManagerOrder order (dynamic, asset, test);
    // entries may be null
    static std::array<SkFontMgr*, 3> UserFontManagers(const FontCollection& fontCollection);

    // Returns the installed family that matches familyName exactly in fontStyle, or null; caches
    // only a match, so a family registered later resolves once FontCollection::clearCaches drops
    // the collection's own cached miss
    sk_sp<SkTypeface> matchPlatformFamily(const SkString& familyName, SkFontStyle fontStyle);

    // Both caches are leaves guarded by one mutex: no code path holds it while taking it again
    SkMutex fCacheMutex;

    // Caches descriptors by SkTypefaceID; bounded so transient web or test faces do not pin
    // parsed descriptors for the process lifetime
    static constexpr int kDescriptorCacheSize = 64;
    SkLRUCache<SkTypefaceID, SkUniqueCFRef<CTFontDescriptorRef>> fDescriptorCache
            SK_GUARDED_BY(fCacheMutex){kDescriptorCacheSize};

    // Installed families do not depend on the FontCollection, so one process-wide cache serves
    // every paragraph
    struct PlatformFamilyKey {
        SkString fFamilyName;
        SkFontStyle fFontStyle;
        bool operator==(const PlatformFamilyKey& other) const {
            return fFamilyName == other.fFamilyName && fFontStyle == other.fFontStyle;
        }
        struct Hash {
            uint32_t operator()(const PlatformFamilyKey& key) const {
                return SkGoodHash()(key.fFamilyName) ^ SkGoodHash()(key.fFontStyle);
            }
        };
    };
    static constexpr int kSystemFamilyCacheSize = 64;
    SkLRUCache<PlatformFamilyKey, sk_sp<SkTypeface>, PlatformFamilyKey::Hash> fPlatformFamilyCache
            SK_GUARDED_BY(fCacheMutex){kSystemFamilyCacheSize};
};

}  // namespace textlayout
}  // namespace skia

#endif  // CoreTextFontRegistry_DEFINED
