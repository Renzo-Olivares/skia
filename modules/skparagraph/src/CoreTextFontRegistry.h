// Copyright 2026 Google LLC
#ifndef CoreTextFontRegistry_DEFINED
#define CoreTextFontRegistry_DEFINED

#include "include/core/SkFont.h"
#include "include/core/SkString.h"
#include "include/core/SkTypeface.h"
#include "include/core/SkTypes.h"
#include "include/private/SkMutex.h"
#include "modules/skparagraph/include/FontArguments.h"
#include "modules/skparagraph/include/ParagraphStyle.h"
#include "src/core/SkLRUCache.h"

#include <optional>
#include <vector>

#include "src/utils/mac/SkUniqueCFRef.h"
#include <CoreText/CoreText.h>

namespace skia {
namespace textlayout {

class FontCollection;

class CoreTextFontRegistry {
public:
    // Returns the process-wide registry; its caches are shared by every paragraph and guarded by
    // fMutex, so any thread may use it
    static CoreTextFontRegistry& Instance();

    // In-process descriptor cache indexed by SkTypefaceID to eliminate fontd IPC during run
    // attribution
    SkUniqueCFRef<CTFontDescriptorRef> getDescriptor(SkTypeface* typeface);

    // Resolves optical sizing and symbolic traits via .AppleSystemUIFont when typeface is a system
    // UI font
    SkUniqueCFRef<CTFontRef> createFont(SkTypeface* typeface,
                                        const SkFontStyle& style,
                                        CGFloat size);

    // System UI fonts require dynamic descriptor synthesis to preserve cascade lists across scripts
    static bool isAppleSystemFont(const SkTypeface* typeface);

    // True for names that start with .AppleSystemUIFont, .SF or .NewYork, for the Flutter
    // aliases CupertinoSystemText and CupertinoSystemDisplay, and for a null or empty name
    static bool isAppleSystemFont(const char* familyName);

    // Never consults FontCollection's default manager: each family is looked up in the user
    // registries, then in CoreText by exact family name; when none matches, the collection's
    // default families, a user registry face and finally .AppleSystemUIFont stand in. With font
    // fallback disabled only user registries are searched, so the result may be empty
    std::vector<sk_sp<SkTypeface>> findTypefaces(FontCollection& fontCollection,
                                                 const std::vector<SkString>& familyNames,
                                                 SkFontStyle fontStyle,
                                                 const std::optional<FontArguments>& fontArgs);

private:
    CoreTextFontRegistry() = default;
    ~CoreTextFontRegistry() = default;
    CoreTextFontRegistry(const CoreTextFontRegistry&) = delete;
    CoreTextFontRegistry& operator=(const CoreTextFontRegistry&) = delete;

    // Builds an uncached descriptor for a non-null typeface: a system UI face gets a
    // .AppleSystemUIFont descriptor with its weight, slant and design, any other face one parsed
    // from its font data, or else one for its family name and style. Null if all of them fail
    SkUniqueCFRef<CTFontDescriptorRef> createDescriptorInternal(SkTypeface* typeface);

    // Returns the face for one family: the user registries first (dynamic, asset, test), then an
    // installed family through matchSystemFamily, unless user managers exist without a default
    // manager or font fallback is disabled. Null when nothing matches
    sk_sp<SkTypeface> matchFamily(FontCollection& fontCollection,
                                  const SkString& familyName,
                                  SkFontStyle fontStyle);
    // Returns the installed family that matches familyName exactly in fontStyle, or null, and
    // caches either answer in fSystemFamilyCache
    sk_sp<SkTypeface> matchSystemFamily(const SkString& familyName, SkFontStyle fontStyle);

    // Only data-backed faces land here; bounded because every web font or test face would
    // otherwise pin its parsed descriptor for the process lifetime
    static constexpr int kDescriptorCacheSize = 64;
    SkMutex fMutex;
    SkLRUCache<SkTypefaceID, SkUniqueCFRef<CTFontDescriptorRef>> fDescriptorCache
            SK_GUARDED_BY(fMutex){kDescriptorCacheSize};

    // Installed families do not depend on the FontCollection, so one process-wide cache serves
    // every paragraph; a null entry records a family CoreText does not have
    struct SystemFamilyKey {
        SkString fFamilyName;
        SkFontStyle fFontStyle;
        bool operator==(const SystemFamilyKey& other) const {
            return fFamilyName == other.fFamilyName && fFontStyle == other.fFontStyle;
        }
        struct Hash {
            uint32_t operator()(const SystemFamilyKey& key) const {
                return SkGoodHash()(key.fFamilyName) ^ SkGoodHash()(key.fFontStyle);
            }
        };
    };
    static constexpr int kSystemFamilyCacheSize = 64;
    SkLRUCache<SystemFamilyKey, sk_sp<SkTypeface>, SystemFamilyKey::Hash> fSystemFamilyCache
            SK_GUARDED_BY(fMutex){kSystemFamilyCacheSize};
};

}  // namespace textlayout
}  // namespace skia

#endif  // CoreTextFontRegistry_DEFINED
