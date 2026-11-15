// Copyright 2026 Google LLC
#ifndef CoreTextSpanFonts_DEFINED
#define CoreTextSpanFonts_DEFINED

#include "include/core/SkRefCnt.h"
#include "include/private/SkTArray.h"
#include "modules/skparagraph/include/FontCollection.h"
#include "modules/skparagraph/include/TextStyle.h"

#include <CoreText/CoreText.h>
#include "src/utils/mac/SkUniqueCFRef.h"

#include <utility>

namespace skia {
namespace textlayout {

// Styles that share all font, metric, rasterization, and shaping attributes can coalesce into
// one CFAttributedString style span so paint-only boundaries (color, decoration, shadows) do not
// split CTRuns or ligatures
bool can_coalesce_styles(const TextStyle& a, const TextStyle& b);

// Fonts we ask CoreText for, one CTFont per distinct span style; CoreText may still bind a run
// to a cascade face. The caller keeps them across the '\n' segments of one paragraph: CoreText
// fills a per-CTFont glyph advance cache on first use
class CoreTextRequestedFonts {
public:
    class Entry {
    public:
        // base and cursiveAttrs are what cursiveFont() restyles, set only for a letter-spaced
        // span: font's cascade and variations with the user features alone
        Entry(const TextStyle& style,
              SkUniqueCFRef<CTFontRef> font,
              SkUniqueCFRef<CTFontRef> base,
              SkUniqueCFRef<CFDictionaryRef> cursiveAttrs)
                : fStyle(style)
                , fFont(std::move(font))
                , fBase(std::move(base))
                , fCursiveAttrs(std::move(cursiveAttrs)) {}

        const TextStyle& style() const { return fStyle; }

        // The non-null span font
        CTFontRef font() const { return fFont.get(); }

        // Returns the non-null span font for cursive runs of a letter-spaced span, made on the
        // first call: fBase with fCursiveAttrs, so the letterSpacing liga=0 is gone
        CTFontRef cursiveFont();

    private:
        TextStyle fStyle;
        SkUniqueCFRef<CTFontRef> fFont;
        SkUniqueCFRef<CTFontRef> fBase;
        SkUniqueCFRef<CFDictionaryRef> fCursiveAttrs;
        // Lazily built result of cursiveFont()
        SkUniqueCFRef<CTFontRef> fCursiveFont;
    };

    // Fonts come from non-null fontCollection and follow its fallback setting
    explicit CoreTextRequestedFonts(sk_sp<FontCollection> fontCollection);

    // Returns the entry for style, first making its span font when no entry has one: the primary
    // typeface at the span size with the cascade, features and variations of style. The pointer
    // stays valid until the next entry is made
    Entry* findOrMake(const TextStyle& style);

    // Number of span fonts made so far
    int size() const { return fEntries.size(); }

private:
    sk_sp<FontCollection> fFontCollection;
    // A paragraph has few distinct span fonts, so lookup is a linear scan
    skia_private::STArray<4, Entry> fEntries;
};

}  // namespace textlayout
}  // namespace skia

#endif  // CoreTextSpanFonts_DEFINED
