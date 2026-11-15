// Copyright 2026 Google LLC
#ifndef FontCollectionFaceCache_DEFINED
#define FontCollectionFaceCache_DEFINED

#include "include/core/SkFontStyle.h"
#include "include/core/SkString.h"
#include "include/core/SkTypeface.h"
#include "include/private/SkMutex.h"
#include "modules/skparagraph/include/FontArguments.h"
#include "modules/skparagraph/include/FontCollection.h"
#include "src/core/SkTHash.h"

#include <optional>
#include <string>
#include <vector>

// Shared by FontCollection.cpp and CoreTextFontRegistry.cpp, which resolve family lists into the
// same cache

namespace skia {
namespace textlayout {

struct FontCollection::FaceCache {
    struct FamilyKey {
        std::vector<SkString> fFamilyNames;
        SkFontStyle fFontStyle;
        std::optional<FontArguments> fFontArguments;
        // HarfBuzz and CoreText paragraphs can share a collection but resolve the same families
        // differently (CoreText never asks fDefaultFontManager), so neither may reuse the other's
        bool fCoreText = false;

        bool operator==(const FamilyKey& other) const {
            return fFamilyNames == other.fFamilyNames && fFontStyle == other.fFontStyle &&
                   fFontArguments == other.fFontArguments && fCoreText == other.fCoreText;
        }

        struct Hasher {
            size_t operator()(const FamilyKey& key) const {
                size_t hash = 0;
                for (const SkString& family : key.fFamilyNames) {
                    hash ^= std::hash<std::string>()(family.c_str());
                }
                return hash ^ std::hash<uint32_t>()(key.fFontStyle.weight()) ^
                       std::hash<uint32_t>()(key.fFontStyle.slant()) ^
                       std::hash<std::optional<FontArguments>>()(key.fFontArguments) ^
                       std::hash<bool>()(key.fCoreText);
            }
        };
    };

    // Returns a copy of the cached list for key, or nullopt. The lock is released before the
    // caller resolves a miss, so font manager queries do not block cache hits and cloneTypeface
    // can lock fVariationCache safely
    std::optional<std::vector<sk_sp<SkTypeface>>> find(const FamilyKey& key) {
        SkAutoMutexExclusive lock(fMutex);
        if (auto* found = fTypefaces.find(key)) {
            return *found;
        }
        return std::nullopt;
    }

    // Stores typefaces under key and returns them; reuses the cached vector if another thread
    // populated key while unlocked
    std::vector<sk_sp<SkTypeface>> insert(const FamilyKey& key,
                                          std::vector<sk_sp<SkTypeface>> typefaces) {
        SkAutoMutexExclusive lock(fMutex);
        if (auto* found = fTypefaces.find(key)) {
            return *found;
        }
        return *fTypefaces.set(key, std::move(typefaces));
    }

    // Synchronize fTypefaces inside the pimpl struct to preserve FontCollection's ABI layout
    mutable SkMutex fMutex;
    skia_private::THashMap<FamilyKey, std::vector<sk_sp<SkTypeface>>, FamilyKey::Hasher> fTypefaces
            SK_GUARDED_BY(fMutex);
};

}  // namespace textlayout
}  // namespace skia

#endif  // FontCollectionFaceCache_DEFINED
