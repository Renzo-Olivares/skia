// Copyright 2026 Google LLC
#ifndef CoreTextAttrStringBuilder_DEFINED
#define CoreTextAttrStringBuilder_DEFINED

#include "include/core/SkColor.h"
#include "include/core/SkFourByteTag.h"
#include "include/core/SkRefCnt.h"
#include "include/core/SkTypes.h"
#include "include/private/SkTArray.h"
#include "include/private/SkTemplates.h"
#include "modules/skparagraph/include/DartTypes.h"
#include "modules/skparagraph/include/FontCollection.h"
#include "modules/skparagraph/include/ParagraphStyle.h"
#include "modules/skparagraph/include/TextStyle.h"
#include "modules/skparagraph/src/CoreTextFontRegistry.h"
#include "modules/skparagraph/src/CoreTextIndexMap.h"

#include "src/utils/mac/SkUniqueCFRef.h"
#include <CoreText/CoreText.h>

namespace skia {
namespace textlayout {

class CoreTextAttrStringBuilder {
public:
    // Decodes utf8, which must outlive the builder, once: the same pass fills the index map, the
    // UTF-16 text for build() and the script runs. Fonts come from fontCollection in build()
    CoreTextAttrStringBuilder(const char* utf8, size_t utf8Bytes,
                              TextDirection baseDirection,
                              sk_sp<FontCollection> fontCollection);

    // Records style for utf8Range clipped to the text; an empty or out-of-text range is ignored,
    // and a span that follows the previous one and differs only in paint extends that one
    void addStyle(TextRange utf8Range, const TextStyle& style);

    // Returns the text as a CFAttributedString with the base writing direction and, per recorded
    // span, its font, features, variations and locale; null if CoreFoundation fails
    SkUniqueCFRef<CFAttributedStringRef> build();

    const CoreTextIndexMap& indexMap() const { return fIndexMap; }

    // Script of the itemized run holding utf8Index; like SkShaper's script iterator, common
    // code points join the preceding script, or the following one at the start of the text
    SkFourByteTag scriptAt(size_t utf8Index) const;

    // Returns the script of c for scriptAt: the listed block's script, a non-cursive placeholder
    // for a letter of an unlisted script, and 0 for code points that join the neighbouring run
    // (digits, punctuation, spaces, symbols, combining marks outside listed blocks)
    static SkFourByteTag ResolveCodepointScript(SkUnichar c);

private:
    const char* fUtf8;
    size_t fUtf8Bytes;
    TextDirection fBaseDirection;
    sk_sp<FontCollection> fFontCollection;
    CoreTextIndexMap fIndexMap;
    // Sized to the UTF-8 length, an upper bound on the UTF-16 length
    skia_private::AutoSTArray<256, uint16_t> fUtf16;

    struct StyleSpan {
        TextRange fRange;
        TextStyle fStyle;
    };
    skia_private::STArray<8, StyleSpan> fStyles;

    struct ScriptRun {
        TextRange fRange;
        SkFourByteTag fScript;
    };
    skia_private::STArray<4, ScriptRun> fScriptRuns;
};

}  // namespace textlayout
}  // namespace skia

#endif  // CoreTextAttrStringBuilder_DEFINED
