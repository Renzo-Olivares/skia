/*
 * Copyright 2026 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "include/core/SkFontArguments.h"
#include "include/core/SkFontStyle.h"
#include "include/core/SkRefCnt.h"
#include "include/core/SkStream.h"
#include "include/core/SkString.h"
#include "include/core/SkTypeface.h"
#include "include/core/SkTypes.h"
#include "modules/skparagraph/include/FontCollection.h"
#include "modules/skparagraph/include/TypefaceFontProvider.h"
#include "modules/skparagraph/src/CoreTextFontRegistry.h"
#include "modules/skparagraph/tests/coretext/CoreTextTestUtils.h"
#include "src/core/SkAdvancedTypefaceMetrics.h"
#include "src/core/SkScalerContext.h"
#include "src/utils/mac/SkUniqueCFRef.h"
#include "tests/Test.h"
#include "tools/Resources.h"
#include "tools/fonts/FontToolUtils.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreText/CoreText.h>
#include <string.h>

#include <array>
#include <atomic>
#include <memory>
#include <optional>
#include <vector>

using namespace skia::textlayout;
using namespace skia::textlayout::coretext_test;

// With no requested or default family found, a user face stands in before the system font, as
// in FontCollection::getFontManagerOrder, whether font fallback is enabled or not
DEF_TEST(CoreTextPipeline_MissingFamily_UserFaceStandsIn, reporter) {
    sk_sp<SkTypeface> roboto = ToolUtils::CreateTypefaceFromResource("fonts/Roboto-Regular.ttf");
    if (!roboto) {
        return;
    }
    auto provider = sk_make_sp<TypefaceFontProvider>();
    provider->registerTypeface(roboto, SkString("FaceA"));
    auto fc = sk_make_sp<FontCollection>();
    fc->setDefaultFontManager(ToolUtils::TestFontMgr(), "NoSuchDefaultFamily");
    fc->setDynamicFontManager(provider);
    std::vector<SkString> families = {SkString("NoSuchFamily")};

    for (bool fallback : {true, false}) {
        skiatest::ReporterContext ctx(reporter, SkStringPrintf("fallback %d", fallback));
        if (fallback) {
            fc->enableFontFallback();
        } else {
            fc->disableFontFallback();
        }
        auto expected = fc->findTypefaces(families, SkFontStyle());
        auto actual = CoreTextFontRegistry::Instance().findTypefaces(
                *fc, families, SkFontStyle(), std::nullopt);
        REPORTER_ASSERT(reporter,
                        expected.size() == 1 && expected[0]->uniqueID() == roboto->uniqueID(),
                        "got %zu faces",
                        expected.size());
        REPORTER_ASSERT(reporter,
                        actual.size() == 1 && actual[0]->uniqueID() == roboto->uniqueID(),
                        "got %zu faces",
                        actual.size());
    }
}

namespace {
// Font data copies handed out by StubCloneTypeface::onOpenStream and not freed yet
std::atomic<int> gLiveStubFontDataCopies{0};

class StubCloneTypeface final : public SkTypeface {
public:
    // A face named family whose clone fails when failClone; with fontData, each openStream
    // returns a fresh copy that only the stream owns, counted in gLiveStubFontDataCopies
    static sk_sp<SkTypeface> Make(const char* family,
                                  bool failClone,
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
                copy,
                fFontData->size(),
                [](const void* ptr, void*) {
                    --gLiveStubFontDataCopies;
                    sk_free(const_cast<void*>(ptr));
                },
                nullptr));
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

// A user face whose variation clone fails is dropped, as FontCollection drops it, so the next
// family stands in instead of a null entry reaching the attributed string; with every family
// failing, the default family stands in, and with that failing too, .AppleSystemUIFont
DEF_TEST(CoreTextPipeline_FontRegistry_NullCloneTypefaceSkipped, reporter) {
    sk_sp<SkTypeface> valid = StubCloneTypeface::Make("ValidClone", false);
    auto provider = sk_make_sp<TypefaceFontProvider>();
    provider->registerTypeface(StubCloneTypeface::Make("NullClone", true), SkString("NullClone"));
    provider->registerTypeface(valid, SkString("ValidClone"));

    auto fc = sk_make_sp<FontCollection>();
    fc->setDefaultFontManager(ToolUtils::TestFontMgr());
    fc->setDynamicFontManager(provider);
    fc->disableFontFallback();

    SkFontArguments::VariationPosition::Coordinate coord = {SkSetFourByteTag('w', 'g', 'h', 't'),
                                                            500.0f};
    SkFontArguments skArgs;
    skArgs.setVariationDesignPosition({&coord, 1});
    FontArguments fontArgs(skArgs);
    std::vector<SkString> families = {SkString("NullClone"), SkString("ValidClone")};

    auto expected = fc->findTypefaces(families, SkFontStyle(), fontArgs);
    auto actual =
            CoreTextFontRegistry::Instance().findTypefaces(*fc, families, SkFontStyle(), fontArgs);
    REPORTER_ASSERT(reporter,
                    expected.size() == 1 && expected[0] != nullptr,
                    "expected size %zu",
                    expected.size());
    REPORTER_ASSERT(reporter,
                    actual.size() == 1 && actual[0] != nullptr &&
                            actual[0]->uniqueID() == valid->uniqueID(),
                    "actual size=%zu front=%p",
                    actual.size(),
                    actual.empty() ? nullptr : actual[0].get());

    fc->setDefaultFontManager(ToolUtils::TestFontMgr(), "ValidClone");
    std::vector<SkString> onlyNullClone = {SkString("NullClone")};
    auto expectedStandIn = fc->findTypefaces(onlyNullClone, SkFontStyle(), fontArgs);
    auto actualStandIn = CoreTextFontRegistry::Instance().findTypefaces(
            *fc, onlyNullClone, SkFontStyle(), fontArgs);
    REPORTER_ASSERT(reporter,
                    expectedStandIn.size() == 1 && expectedStandIn[0] != nullptr &&
                            expectedStandIn[0]->uniqueID() == valid->uniqueID(),
                    "expectedStandIn size %zu",
                    expectedStandIn.size());
    REPORTER_ASSERT(reporter,
                    actualStandIn.size() == 1 && actualStandIn[0] != nullptr &&
                            actualStandIn[0]->uniqueID() == valid->uniqueID(),
                    "actualStandIn size=%zu front=%p",
                    actualStandIn.size(),
                    actualStandIn.empty() ? nullptr : actualStandIn[0].get());

    fc->setDefaultFontManager(ToolUtils::TestFontMgr());
    fc->enableFontFallback();
    auto sysStandIn = CoreTextFontRegistry::Instance().findTypefaces(
            *fc, onlyNullClone, SkFontStyle(), fontArgs);
    REPORTER_ASSERT(reporter,
                    sysStandIn.size() == 1 && sysStandIn[0] != nullptr,
                    "sysStandIn size %zu",
                    sysStandIn.size());
}

namespace {
// Installed-family requests where CoreText's nearest-trait match on macOS 26 differs from the
// SkFontStyleSet_Mac style distance: w500 -> Bold, italic -> upright Bold, w900 -> Condensed,
// Optima w800 -> ExtraBlack, plus a condensed request that needs the width trait
struct InstalledStyleCase {
    const char* family;
    SkFontStyle style;
};

constexpr std::array<InstalledStyleCase, 6> kInstalledStyleCases{{
        {"Georgia", SkFontStyle(500, SkFontStyle::kNormal_Width, SkFontStyle::kUpright_Slant)},
        {"Didot", SkFontStyle(700, SkFontStyle::kNormal_Width, SkFontStyle::kItalic_Slant)},
        {"Futura", SkFontStyle(700, SkFontStyle::kNormal_Width, SkFontStyle::kItalic_Slant)},
        {"Helvetica Neue",
         SkFontStyle(900, SkFontStyle::kNormal_Width, SkFontStyle::kUpright_Slant)},
        {"Optima", SkFontStyle(800, SkFontStyle::kNormal_Width, SkFontStyle::kUpright_Slant)},
        {"American Typewriter",
         SkFontStyle(300, SkFontStyle::kCondensed_Width, SkFontStyle::kUpright_Slant)},
}};

SkString installed_style_context(const InstalledStyleCase& c) {
    return SkStringPrintf("%s w%d slant %d", c.family, c.style.weight(), (int)c.style.slant());
}
}  // namespace

// Installed families resolve to the face FontCollection gets from SkFontMgr_Mac's
// matchFamilyStyle, so switching engines keeps the face Flutter apps use
DEF_TEST(CoreTextPipeline_FontRegistry_InstalledFamilyFace, reporter) {
    auto fontCollection = sk_make_sp<FontCollection>();
    fontCollection->setDefaultFontManager(ToolUtils::TestFontMgr());
    for (const InstalledStyleCase& c : kInstalledStyleCases) {
        if (!has_font_families({c.family})) {
            continue;
        }
        skiatest::ReporterContext context(reporter, installed_style_context(c));
        std::vector<SkString> families = {SkString(c.family)};
        auto expected = fontCollection->findTypefaces(families, c.style);
        auto actual = CoreTextFontRegistry::Instance().findTypefaces(
                *fontCollection, families, c.style, std::nullopt);
        REPORTER_ASSERT(reporter, expected.size() == 1, "expected size %zu", expected.size());
        REPORTER_ASSERT(reporter, actual.size() == 1, "actual size %zu", actual.size());
        if (expected.size() != 1 || actual.size() != 1) {
            continue;
        }
        SkString want = postscript_name(*expected[0]);
        SkString got = postscript_name(*actual[0]);
        REPORTER_ASSERT(reporter, got == want, "got %s, want %s", got.c_str(), want.c_str());
    }
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
            CoreTextFontRegistry::Instance().copyDescriptor(face.get());
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
    REPORTER_ASSERT(reporter, font != nullptr);
    if (!font) {
        return;
    }
    const CFIndex glyphCount = CTFontGetGlyphCount(font.get());
    REPORTER_ASSERT(reporter, glyphCount > 0, "got %ld glyphs", (long)glyphCount);
}
