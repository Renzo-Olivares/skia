/*
 * Copyright 2026 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "include/core/SkSpan.h"
#include "include/core/SkString.h"
#include "include/core/SkTypes.h"
#include "include/private/SkAssert.h"
#include "include/private/SkMutex.h"
#include "include/private/SkTArray.h"
#include "include/private/SkTemplates.h"
#include "modules/skunicode/include/SkUnicode.h"
#include "modules/skunicode/include/SkUnicode_apple.h"
#include "modules/skunicode/src/SkBidiFactory_icu_subset.h"
#include "modules/skunicode/src/SkUnicode_hardcoded.h"
#include "modules/skunicode/src/SkUnicode_icu_bidi.h"
#include "src/core/SkBitmaskEnum.h"
#include "src/core/SkNoDestructor.h"
#include "src/core/SkUTF.h"
#include "src/utils/mac/SkUniqueCFRef.h"

#include <CoreFoundation/CoreFoundation.h>

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace skia_private;

namespace {

void CFDefaultDeallocate(UniChar* ptr) { CFAllocatorDeallocate(kCFAllocatorDefault, ptr); }

// CoreFoundation UTF-8 decoding always drops a leading EF BB BF as a byte order mark, but here
// U+FEFF is a real ZWNBSP that owns code units. CFStringCreateWithCharactersNoCopy never strips it,
// and CF would allocate a UTF-16 buffer for this non-ASCII text anyway, so it adopts ours instead
SkUniqueCFRef<CFStringRef> MakeCFStringKeepingLeadingZwnbsp(std::string_view utf8) {
    SkASSERT(utf8.starts_with("\xEF\xBB\xBF"));
    std::string_view afterBom = utf8.substr(3);
    // LIFETIME: this probe borrows the caller's bytes and dies before the if body. SkUTF accepts
    // overlongs and encoded surrogates, CF does not. Let CF reject ill-formed text after the BOM,
    // so the result does not depend on whether the text starts with U+FEFF
    if (!SkUniqueCFRef<CFStringRef>(CFStringCreateWithBytesNoCopy(kCFAllocatorDefault,
                                                                  (const UInt8*)afterBom.data(),
                                                                  SkToInt(afterBom.size()),
                                                                  kCFStringEncodingUTF8,
                                                                  false,
                                                                  kCFAllocatorNull))) {
        return nullptr;
    }
    // UTF-8 never needs more UTF-16 code units than it has bytes
    const int utf8Units = SkToInt(utf8.size());
    std::unique_ptr<UniChar, SkFunctionObject<CFDefaultDeallocate>> utf16(static_cast<UniChar*>(
            CFAllocatorAllocate(kCFAllocatorDefault, utf8Units * CFIndex{sizeof(UniChar)}, 0)));
    SkASSERT_RELEASE(utf16);
    // utf8 starts with valid U+FEFF and CF already validated afterBom
    const int utf16Units = SkUTF::UTF8ToUTF16(utf16.get(), utf8Units, utf8.data(), utf8.size());
    SkASSERT(utf16Units >= 1);
    // kCFAllocatorDefault as contentsDeallocator hands the buffer to CF, which frees it
    SkUniqueCFRef<CFStringRef> string(CFStringCreateWithCharactersNoCopy(
            kCFAllocatorDefault, utf16.release(), utf16Units, kCFAllocatorDefault));
    SkASSERT_RELEASE(string);
    return string;
}

// Wrap caller-owned buffers with kCFAllocatorNull to avoid copying text when the encoding is valid
SkUniqueCFRef<CFStringRef> MakeCFStringNoCopy(const char utf8[], int utf8Units) {
    // LIFETIME: the returned CFString borrows utf8, so callers keep the buffer alive until it is
    // released (TokenizerPool::give drops its reference). A leading BOM gets an owned UTF-16 copy
    // instead, since CF UTF-8 decoding would strip that U+FEFF
    if (std::string_view text(utf8, SkToSizeT(utf8Units)); text.starts_with("\xEF\xBB\xBF")) {
        return MakeCFStringKeepingLeadingZwnbsp(text);
    }
    return SkUniqueCFRef<CFStringRef>(CFStringCreateWithBytesNoCopy(kCFAllocatorDefault,
                                                                    (const UInt8*)utf8,
                                                                    utf8Units,
                                                                    kCFStringEncodingUTF8,
                                                                    false,
                                                                    kCFAllocatorNull));
}

SkUniqueCFRef<CFStringRef> MakeCFStringNoCopy(const char16_t utf16[], int utf16Units) {
    return SkUniqueCFRef<CFStringRef>(CFStringCreateWithCharactersNoCopy(
            kCFAllocatorDefault, (const UniChar*)utf16, utf16Units, kCFAllocatorNull));
}

// Returning nullptr instructs CoreFoundation to use the default locale without allocating
SkUniqueCFRef<CFLocaleRef> MakeCFLocale(const char* locale) {
    if (locale == nullptr || locale[0] == '\0') {
        return nullptr;
    }
    SkUniqueCFRef<CFStringRef> locStr(
            CFStringCreateWithCString(kCFAllocatorDefault, locale, kCFStringEncodingUTF8));
    if (!locStr) {
        return nullptr;
    }
    return SkUniqueCFRef<CFLocaleRef>(CFLocaleCreate(kCFAllocatorDefault, locStr.get()));
}

// CoreFoundation reports boundaries in UTF-16 code units; build an inline UTF-16-to-UTF-8 offset
// table once per string
struct Utf16ToUtf8Mapper {
    STArray<256, size_t> fU16ToU8;
    int fUtf8Units;

    Utf16ToUtf8Mapper(const char utf8[], int utf8Units) : fUtf8Units(utf8Units) {
        SkUnicode::extractUtfConversionMapping(
                SkSpan<const char>(utf8, utf8Units),
                [&](size_t u8) { fU16ToU8.push_back(u8); },
                [](size_t) {});
    }

    // Clamp UTF-16 indices to [0, fUtf8Units] so past-the-end CFRange endpoints map to the buffer end
    SkUnicode::Position operator()(CFIndex pos16) const {
        if (pos16 <= 0) {
            return 0;
        }
        if (SkToInt(pos16) < fU16ToU8.size()) {
            return SkTo<SkUnicode::Position>(fU16ToU8[pos16]);
        }
        return fUtf8Units;
    }
};

// Idle default-locale tokenizers per unit, shared by all threads. CFStringTokenizerCreate loads
// NaturalLanguage word taggers (up to 2/3 of a CJK getWords call); CFStringTokenizerSetString on
// an idle tokenizer keeps them. A tokenizer is used by one thread at a time, outside the lock
class TokenizerPool {
public:
    static TokenizerPool& Get() {
        static SkNoDestructor<TokenizerPool> pool;
        return *pool;
    }

    // Returns an idle tokenizer for unit already set to str, or nullptr when none is idle
    SkUniqueCFRef<CFStringTokenizerRef> take(CFStringRef str, CFRange range, CFOptionFlags unit) {
        SkUniqueCFRef<CFStringTokenizerRef> tokenizer;
        {
            SkAutoMutexExclusive lock(fMutex);
            SkASSERT(unit <= kCFStringTokenizerUnitWordBoundary);
            auto& idle = fIdle[unit];
            if (idle.empty()) {
                return nullptr;
            }
            tokenizer = std::move(idle.back());
            idle.pop_back();
        }
        CFStringTokenizerSetString(tokenizer.get(), str, range);
        return tokenizer;
    }

    void give(SkUniqueCFRef<CFStringTokenizerRef> tokenizer, CFOptionFlags unit) {
        // The tokenizer retains its string, which may borrow the caller's text buffer
        CFStringTokenizerSetString(tokenizer.get(), CFSTR(""), CFRangeMake(0, 0));
        SkAutoMutexExclusive lock(fMutex);
        SkASSERT(unit <= kCFStringTokenizerUnitWordBoundary);
        auto& idle = fIdle[unit];
        if (idle.size() < kCapacity) {
            idle.push_back(std::move(tokenizer));
        }
    }

private:
    static constexpr int kCapacity = 2;
    static_assert(kCFStringTokenizerUnitWordBoundary == 4);

    SkMutex fMutex;
    std::array<STArray<kCapacity, SkUniqueCFRef<CFStringTokenizerRef>>,
               kCFStringTokenizerUnitWordBoundary + 1>
            fIdle;
};

// Tokenize [0, len16) with CFStringTokenizer and invoke callback with each token's UTF-16 CFRange
template <typename Callback>
bool Tokenize(CFStringRef str,
              CFIndex len16,
              CFOptionFlags unit,
              const char* locale,
              Callback&& callback) {
    if (len16 <= 0 || !str) {
        return true;
    }
    const CFRange range = CFRangeMake(0, len16);
    SkUniqueCFRef<CFLocaleRef> cfLoc = MakeCFLocale(locale);
    // Only default-locale tokenizers are recycled: a locale is fixed at creation
    SkUniqueCFRef<CFStringTokenizerRef> tokenizer;
    if (!cfLoc) {
        tokenizer = TokenizerPool::Get().take(str, range, unit);
    }
    if (!tokenizer) {
        tokenizer.reset(
                CFStringTokenizerCreate(kCFAllocatorDefault, str, range, unit, cfLoc.get()));
        if (!tokenizer) {
            return false;
        }
    }

    CFStringTokenizerTokenType tokenType = CFStringTokenizerAdvanceToNextToken(tokenizer.get());
    // Bound iteration count so degenerate zero-length tokens cannot cause an infinite loop
    CFIndex guard = 0;
    const CFIndex maxSteps = 2 * len16 + 2;
    while (tokenType != kCFStringTokenizerTokenNone && ++guard <= maxSteps) {
        callback(CFStringTokenizerGetCurrentTokenRange(tokenizer.get()));
        tokenType = CFStringTokenizerAdvanceToNextToken(tokenizer.get());
    }
    if (!cfLoc) {
        TokenizerPool::Get().give(std::move(tokenizer), unit);
    }
    return true;
}

// True for code units that CoreFoundation never joins with each other into one grapheme cluster:
// Grapheme_Cluster_Break=Other letters, digits, punctuation and spaces of common scripts, and
// Hangul syllables (no UAX #29 rule joins two of them). Checked for all 40497^2 pairs on macOS
bool IsStandaloneGrapheme(UniChar c) {
    // Hand-picked, not generated by tools/unicode/generate_skunicode_tables.py. After an edit run
    // SkUnicode_Apple_GraphemeFastPathMatchesComposedRanges: it fails if a range admits a BMP code
    // unit that CF joins to a preceding standalone base
    static constexpr std::array<std::pair<UniChar, UniChar>, 23> kRanges{{
            {0x0020, 0x007E}, {0x00A0, 0x00AC}, {0x00AE, 0x02FF}, {0x0370, 0x0377},
            {0x037A, 0x037F}, {0x0384, 0x038A}, {0x038C, 0x038C}, {0x038E, 0x03A1},
            {0x03A3, 0x0482}, {0x048A, 0x052F}, {0x05D0, 0x05EA}, {0x0620, 0x064A},
            {0x0660, 0x066F}, {0x0671, 0x06D3}, {0x0904, 0x0939}, {0x0E01, 0x0E30},
            {0x3000, 0x3029}, {0x3041, 0x3096}, {0x30A0, 0x30FF}, {0x3400, 0x4DBF},
            {0x4E00, 0x9FFF}, {0xAC00, 0xD7A3}, {0xFF01, 0xFF60},
    }};
    static_assert(std::is_sorted(kRanges.begin(), kRanges.end()));
    auto next =
            std::upper_bound(kRanges.begin(), kRanges.end(), c, [](UniChar v, const auto& range) {
                return v < range.first;
            });
    return next != kRanges.begin() && c <= std::prev(next)->second;
}

// Emit start-of-cluster and end-of-text UTF-16 offsets with guaranteed forward progress even on
// zero-length ranges from malformed surrogates
template <typename Callback>
void ForEachGraphemeBoundary(CFStringRef str, CFIndex len16, Callback&& callback) {
    CFStringInlineBuffer buffer;
    CFStringInitInlineBuffer(str, &buffer, CFRangeMake(0, len16));
    CFIndex idx = 0;
    while (idx < len16) {
        // idx starts a cluster, so two standalone code units make a one-unit cluster without
        // the per-cluster CoreFoundation call
        if (IsStandaloneGrapheme(CFStringGetCharacterFromInlineBuffer(&buffer, idx)) &&
            (idx + 1 == len16 ||
             IsStandaloneGrapheme(CFStringGetCharacterFromInlineBuffer(&buffer, idx + 1)))) {
            callback(idx);
            ++idx;
            continue;
        }
        CFRange r = CFStringGetRangeOfComposedCharactersAtIndex(str, idx);
        callback(r.location);
        CFIndex next = std::max(idx + 1, r.location + std::max<CFIndex>(r.length, 1));
        // CFStringGetRangeOfComposedCharactersAtIndex splits CRLF; coalesce per UAX #29 GB3
        if (next < len16 && CFStringGetCharacterFromInlineBuffer(&buffer, idx) == '\r' &&
            CFStringGetCharacterFromInlineBuffer(&buffer, next) == '\n') {
            ++next;
        }
        idx = next;
    }
    callback(len16);
}

class SkBreakIterator_apple : public SkBreakIterator {
public:
    SkBreakIterator_apple(SkUnicode* unicode, SkUnicode::BreakType type, const char* locale)
            : fUnicode(unicode), fType(type), fLocale(locale ? locale : "") {}

    ~SkBreakIterator_apple() override = default;

    Position first() override { return fBreaks.empty() ? 0 : fBreaks[(fIndex = 0)]; }
    Position current() override { return fBreaks.empty() ? 0 : fBreaks[fIndex]; }
    Position next() override {
        if (fIndex + 1 < SkTo<Position>(fBreaks.size())) {
            return fBreaks[++fIndex];
        }
        fIndex = fBreaks.size();
        return -1;
    }
    Status status() override {
        if (fIndex < SkTo<Position>(fStatuses.size())) {
            return fStatuses[fIndex];
        }
        return 0;
    }
    bool isDone() override { return fIndex >= SkTo<Position>(fBreaks.size()); }

    bool setText(const char utftext8[], int utf8Units) override {
        this->reset();
        // Reject negative lengths or null non-empty buffers
        if (utf8Units < 0 || (utf8Units > 0 && !utftext8)) {
            return false;
        }
        // Empty inputs still emit a boundary at 0 to match ICU BreakIterator::first() and
        // SkParagraph empty-text expectations
        if (utf8Units == 0) {
            this->addBreak(0, fType == SkUnicode::BreakType::kLines
                                      ? SkUnicode::CodeUnitFlags::kSoftLineBreakBefore
                                      : 0);
            return true;
        }

        // CFStringCreateWithBytesNoCopy validates UTF-8 and returns nullptr on ill-formed sequences
        SkUniqueCFRef<CFStringRef> str = MakeCFStringNoCopy(utftext8, utf8Units);
        if (!str) {
            return false;
        }

        // Populate soft line, grapheme, word, or sentence boundaries mapped to UTF-8 byte offsets
        if (!this->populateBreaks(str.get(),
                                  CFStringGetLength(str.get()),
                                  utf8Units,
                                  Utf16ToUtf8Mapper(utftext8, utf8Units))) {
            return false;
        }

        // CFStringTokenizer does not distinguish mandatory breaks from soft wrap opportunities;
        // scan codepoints to tag positions after LF/BK characters with kHardLineBreakBefore
        if (fType == SkUnicode::BreakType::kLines) {
            fUnicode->forEachCodepoint(
                    utftext8, utf8Units, [&](SkUnichar unichar, int32_t, int32_t end, size_t) {
                        if (fUnicode->isHardBreak(unichar)) {
                            this->addHardBreak(end);
                        }
                    });
        }
        return true;
    }

    bool setText(const char16_t utftext16[], int utf16Units) override {
        this->reset();
        // Reject negative lengths or null non-empty buffers
        if (utf16Units < 0 || (utf16Units > 0 && !utftext16)) {
            return false;
        }
        // Empty inputs still emit a boundary at 0 to match ICU BreakIterator::first() and
        // SkParagraph empty-text expectations
        if (utf16Units == 0) {
            this->addBreak(0, fType == SkUnicode::BreakType::kLines
                                      ? SkUnicode::CodeUnitFlags::kSoftLineBreakBefore
                                      : 0);
            return true;
        }

        SkUniqueCFRef<CFStringRef> str = MakeCFStringNoCopy(utftext16, utf16Units);
        if (!str) {
            return false;
        }

        // Populate soft line, grapheme, word, or sentence boundaries directly in UTF-16 coordinates
        if (!this->populateBreaks(str.get(),
                                  utf16Units,
                                  utf16Units,
                                  [](CFIndex pos16) { return SkTo<Position>(pos16); })) {
            return false;
        }

        // Tag positions immediately following UAX #14 LF/BK codepoints with kHardLineBreakBefore
        if (fType == SkUnicode::BreakType::kLines) {
            fUnicode->forEachCodepoint(
                    utftext16, utf16Units, [&](SkUnichar unichar, int32_t, int32_t end) {
                        if (fUnicode->isHardBreak(unichar)) {
                            this->addHardBreak(end);
                        }
                    });
        }
        return true;
    }

private:
    void reset() {
        fBreaks.clear();
        fStatuses.clear();
        fIndex = 0;
    }

    // Maintain strictly increasing break positions and deduplicate overlapping tokenizer boundaries
    void addBreak(Position pos, Status status) {
        if (fBreaks.empty() || fBreaks.back() < pos) {
            fBreaks.push_back(pos);
            fStatuses.push_back(status);
        }
    }

    // Upgrade an existing soft break at pos to a hard break, or insert a new break carrying both
    // hard and soft flags when CFStringTokenizer omitted the boundary
    void addHardBreak(Position pos) {
        auto it = std::lower_bound(fBreaks.begin(), fBreaks.end(), pos);
        auto idx = std::distance(fBreaks.begin(), it);
        if (it != fBreaks.end() && *it == pos) {
            fStatuses[idx] |= SkUnicode::CodeUnitFlags::kHardLineBreakBefore;
        } else {
            fBreaks.insert(it, pos);
            fStatuses.insert(fStatuses.begin() + idx,
                             SkUnicode::CodeUnitFlags::kHardLineBreakBefore |
                                     SkUnicode::CodeUnitFlags::kSoftLineBreakBefore);
        }
    }

    // Always bracket token boundaries with 0 and totalUnits so inputs skipped by CFStringTokenizer
    // (such as standalone CR or punctuation-only strings) still yield valid [0, end] break ranges
    template <typename MapFn>
    bool populateBreaks(CFStringRef str, CFIndex len16, Position totalUnits, MapFn&& map16) {
        const char* loc = fLocale.empty() ? nullptr : fLocale.c_str();
        if (fType == SkUnicode::BreakType::kLines) {
            // Line-break tokens span between wrap opportunities, so candidate breaks occur at the
            // end of each token (r.location + r.length)
            this->addBreak(0, SkUnicode::CodeUnitFlags::kSoftLineBreakBefore);
            if (!Tokenize(str, len16, kCFStringTokenizerUnitLineBreak, loc,
                          [&](CFRange r) {
                              this->addBreak(map16(r.location + r.length),
                                             SkUnicode::CodeUnitFlags::kSoftLineBreakBefore);
                          })) {
                return false;
            }
            this->addBreak(totalUnits, SkUnicode::CodeUnitFlags::kSoftLineBreakBefore);
        } else if (fType == SkUnicode::BreakType::kGraphemes) {
            // CFStringTokenizer does not expose extended grapheme clusters; use composed character
            // ranges instead
            ForEachGraphemeBoundary(str, len16, [&](CFIndex pos16) {
                this->addBreak(map16(pos16), 0);
            });
            this->addBreak(totalUnits, 0);
        } else {
            // Word and sentence tokens report unit starts at r.location
            CFOptionFlags unit = (fType == SkUnicode::BreakType::kWords)
                                         ? kCFStringTokenizerUnitWordBoundary
                                         : kCFStringTokenizerUnitSentence;
            this->addBreak(0, 0);
            if (!Tokenize(str, len16, unit, loc, [&](CFRange r) {
                    this->addBreak(map16(r.location), 0);
                })) {
                return false;
            }
            this->addBreak(totalUnits, 0);
        }
        return true;
    }

    SkUnicode* fUnicode;
    SkUnicode::BreakType fType;
    std::string fLocale;
    std::vector<Position> fBreaks;
    std::vector<Status> fStatuses;
    Position fIndex = 0;
};

}  // namespace

class SkUnicode_apple : public SkUnicodeHardCodedCharProperties {
public:
    SkUnicode_apple() = default;
    ~SkUnicode_apple() override = default;

    std::unique_ptr<SkBidiIterator> makeBidiIterator(const uint16_t text[],
                                                     int count,
                                                     SkBidiIterator::Direction dir) override {
        return fBidiFact->MakeIterator(text, count, dir);
    }

    std::unique_ptr<SkBidiIterator> makeBidiIterator(const char text[],
                                                     int count,
                                                     SkBidiIterator::Direction dir) override {
        return fBidiFact->MakeIterator(text, count, dir);
    }

    std::unique_ptr<SkBreakIterator> makeBreakIterator(const char locale[],
                                                       BreakType breakType) override {
        return std::make_unique<SkBreakIterator_apple>(this, breakType, locale);
    }

    std::unique_ptr<SkBreakIterator> makeBreakIterator(BreakType breakType) override {
        return this->makeBreakIterator(nullptr, breakType);
    }

    SkString toUpper(const SkString& str) override { return this->toUpper(str, nullptr); }

    SkString toUpper(const SkString& str, const char* locale) override {
        if (str.isEmpty()) {
            return SkString();
        }
        // Manage CFString and CFLocale lifetimes via SkUniqueCFRef across early returns
        SkUniqueCFRef<CFStringRef> cfStr = MakeCFStringNoCopy(str.c_str(), SkToInt(str.size()));
        if (!cfStr) {
            return str;
        }
        SkUniqueCFRef<CFMutableStringRef> mutStr(
                CFStringCreateMutableCopy(kCFAllocatorDefault, 0, cfStr.get()));
        if (!mutStr) {
            return str;
        }
        SkUniqueCFRef<CFLocaleRef> cfLoc = MakeCFLocale(locale);
        CFStringUppercase(mutStr.get(), cfLoc.get());
        CFIndex len = CFStringGetLength(mutStr.get());
        CFIndex maxBytes = CFStringGetMaximumSizeForEncoding(len, kCFStringEncodingUTF8);
        SkString result;
        result.resize(maxBytes);
        CFIndex usedBytes = 0;
        CFStringGetBytes(mutStr.get(),
                         CFRangeMake(0, len),
                         kCFStringEncodingUTF8,
                         0,
                         false,
                         (UInt8*)result.data(),
                         maxBytes,
                         &usedBytes);
        // Trim output buffer from worst-case UTF-8 reservation to the actual encoded byte count
        result.resize(usedBytes);
        return result;
    }

    void reorderVisual(const BidiLevel runLevels[],
                       int levelsCount,
                       int32_t logicalFromVisual[]) override {
        if (levelsCount <= 0) {
            return;
        }
        SkASSERT(runLevels != nullptr);
        SkASSERT(logicalFromVisual != nullptr);
        fBidiFact->bidi_reorderVisual(runLevels, levelsCount, logicalFromVisual);
    }

    bool getBidiRegions(const char utf8[],
                        int utf8Units,
                        TextDirection dir,
                        std::vector<BidiRegion>* results) override {
        return fBidiFact->ExtractBidi(utf8, utf8Units, dir, results);
    }

    bool getWords(const char utf8[],
                  int utf8Units,
                  const char* locale,
                  std::vector<Position>* results) override {
        results->clear();
        // Reject negative lengths or null non-empty buffers
        if (utf8Units < 0 || (utf8Units > 0 && !utf8)) {
            return false;
        }
        // Empty text is a valid no-op that returns true with an empty result vector
        if (utf8Units == 0) {
            return true;
        }

        SkUniqueCFRef<CFStringRef> str = MakeCFStringNoCopy(utf8, utf8Units);
        if (!str) {
            return false;
        }

        // Tokenize word boundaries in UTF-16 code units; only append the end-of-text sentinel when
        // at least one word token was found so whitespace-only strings stay empty
        CFIndex len16 = CFStringGetLength(str.get());
        bool ok = Tokenize(str.get(), len16, kCFStringTokenizerUnitWordBoundary, locale,
                           [&](CFRange r) { results->push_back(r.location); });
        if (!results->empty()) {
            results->push_back(len16);
        }
        return ok;
    }

    bool getUtf8Words(const char utf8[],
                      int utf8Units,
                      const char* locale,
                      std::vector<Position>* results) override {
        // Delegate input validation and UTF-16 word tokenization to getWords
        if (!this->getWords(utf8, utf8Units, locale, results)) {
            return false;
        }
        if (results->empty()) {
            return true;
        }

        // Convert UTF-16 word boundaries populated by getWords in-place to UTF-8 byte offsets
        Utf16ToUtf8Mapper map16(utf8, utf8Units);
        for (Position& pos : *results) {
            pos = map16(pos);
        }
        return true;
    }

    bool getSentences(const char utf8[],
                      int utf8Units,
                      const char* locale,
                      std::vector<Position>* results) override {
        results->clear();
        // Reject negative lengths or null non-empty buffers
        if (utf8Units < 0 || (utf8Units > 0 && !utf8)) {
            return false;
        }
        // Empty text is a valid no-op that returns true with an empty result vector
        if (utf8Units == 0) {
            return true;
        }

        SkUniqueCFRef<CFStringRef> str = MakeCFStringNoCopy(utf8, utf8Units);
        if (!str) {
            return false;
        }

        // Tokenize sentence starts in UTF-16 and map them directly to UTF-8 byte offsets, appending
        // the trailing end-of-text sentinel only when at least one sentence was emitted
        CFIndex len16 = CFStringGetLength(str.get());
        Utf16ToUtf8Mapper map16(utf8, utf8Units);

        bool ok = Tokenize(str.get(), len16, kCFStringTokenizerUnitSentence, locale,
                           [&](CFRange r) { results->push_back(map16(r.location)); });
        if (!results->empty()) {
            results->push_back(utf8Units);
        }
        return ok;
    }

    bool computeCodeUnitFlags(char utf8[],
                              int utf8Units,
                              bool replaceTabs,
                              TArray<CodeUnitFlags, true>* results) override {
        results->clear();
        // Reject negative lengths or null non-empty buffers
        if (utf8Units < 0 || (utf8Units > 0 && !utf8)) {
            return false;
        }
        results->push_back_n(utf8Units + 1, CodeUnitFlags::kNoCodeUnitFlag);
        // Empty text still requires a 1-element sentinel with kSoftLineBreakBefore | kGraphemeStart
        // at index 0 to match ICU and Libgrapheme
        if (utf8Units == 0) {
            (*results)[0] = CodeUnitFlags::kSoftLineBreakBefore | CodeUnitFlags::kGraphemeStart;
            return true;
        }

        SkUniqueCFRef<CFStringRef> str = MakeCFStringNoCopy(utf8, utf8Units);
        if (!str) {
            return false;
        }

        // Map UTF-16 grapheme and line break boundaries to UTF-8 byte positions and apply
        // per-codepoint property flags across all bytes of each sequence
        this->computeBreakFlags(str.get(),
                                CFStringGetLength(str.get()),
                                utf8Units,
                                Utf16ToUtf8Mapper(utf8, utf8Units),
                                results);

        this->forEachCodepoint(utf8,
                               utf8Units,
                               [&](SkUnichar unichar, int32_t start, int32_t end, size_t) {
                                   this->applyCodepointFlags(
                                           unichar, start, end, replaceTabs, utf8, results);
                               });
        return true;
    }

    bool computeCodeUnitFlags(char16_t utf16[],
                              int utf16Units,
                              bool replaceTabs,
                              TArray<CodeUnitFlags, true>* results) override {
        results->clear();
        // Reject negative lengths or null non-empty buffers
        if (utf16Units < 0 || (utf16Units > 0 && !utf16)) {
            return false;
        }
        results->push_back_n(utf16Units + 1, CodeUnitFlags::kNoCodeUnitFlag);
        // Empty text still requires a 1-element sentinel with kSoftLineBreakBefore | kGraphemeStart
        // at index 0 to match ICU and Libgrapheme
        if (utf16Units == 0) {
            (*results)[0] = CodeUnitFlags::kSoftLineBreakBefore | CodeUnitFlags::kGraphemeStart;
            return true;
        }

        SkUniqueCFRef<CFStringRef> str = MakeCFStringNoCopy(utf16, utf16Units);
        if (!str) {
            return false;
        }

        // Populate grapheme, line break, and codepoint property flags directly in UTF-16 coordinates
        this->computeBreakFlags(str.get(),
                                utf16Units,
                                utf16Units,
                                [](CFIndex pos16) { return SkTo<Position>(pos16); },
                                results);

        this->forEachCodepoint(
                utf16, utf16Units, [&](SkUnichar unichar, int32_t start, int32_t end) {
                    this->applyCodepointFlags(unichar, start, end, replaceTabs, utf16, results);
                });
        return true;
    }

private:
    // Share grapheme and soft line break flag population between UTF-8 and UTF-16 overloads via
    // a coordinate-mapping callback, always marking index 0 and the trailing sentinel at units
    template <typename MapFn>
    void computeBreakFlags(CFStringRef str,
                           CFIndex len16,
                           int units,
                           MapFn&& map16,
                           TArray<CodeUnitFlags, true>* results) {
        ForEachGraphemeBoundary(str, len16, [&](CFIndex pos16) {
            (*results)[map16(pos16)] |= CodeUnitFlags::kGraphemeStart;
        });
        (*results)[units] |= CodeUnitFlags::kGraphemeStart;

        (*results)[0] |= CodeUnitFlags::kSoftLineBreakBefore;
        Tokenize(str, len16, kCFStringTokenizerUnitLineBreak, nullptr,
                 [&](CFRange r) {
                     (*results)[map16(r.location + r.length)] |=
                             CodeUnitFlags::kSoftLineBreakBefore;
                 });
        (*results)[units] |= CodeUnitFlags::kSoftLineBreakBefore;
    }

    // Share per-codepoint property flag assignment and optional tab-to-space replacement between
    // UTF-8 and UTF-16 overloads
    template <typename CharT>
    void applyCodepointFlags(SkUnichar unichar,
                             int32_t start,
                             int32_t end,
                             bool replaceTabs,
                             CharT text[],
                             TArray<CodeUnitFlags, true>* results) {
        // Line break flags describe the boundary before a code unit, so a mandatory break on
        // [start, end) marks index end immediately after the newline
        if (this->isHardBreak(unichar)) {
            (*results)[end] |=
                    (CodeUnitFlags::kHardLineBreakBefore | CodeUnitFlags::kSoftLineBreakBefore);
        }
        // Rewrite tabs to spaces before evaluating character classes so replaced tabs receive
        // space/whitespace break flags instead of kControl
        if (replaceTabs && this->isTabulation(unichar)) {
            results->at(start) |= CodeUnitFlags::kTabulation;
            unichar = ' ';
            text[start] = ' ';
        }
        CodeUnitFlags flags = CodeUnitFlags::kNoCodeUnitFlag;
        if (this->isSpace(unichar)) {
            flags |= CodeUnitFlags::kPartOfIntraWordBreak;
        }
        if (this->isWhitespace(unichar)) {
            flags |= CodeUnitFlags::kPartOfWhiteSpaceBreak;
        }
        if (this->isControl(unichar)) {
            flags |= CodeUnitFlags::kControl;
        }
        if (this->isIdeographic(unichar)) {
            flags |= CodeUnitFlags::kIdeographic;
        }
        if (this->isDefaultIgnorable(unichar)) {
            flags |= CodeUnitFlags::kDefaultIgnorable;
        }
        // Replicate character-class flags across all UTF-8 bytes or UTF-16 surrogates in [start, end)
        for (auto i = start; i < end; ++i) {
            results->at(i) |= flags;
        }
    }

    sk_sp<SkBidiFactory> fBidiFact = sk_make_sp<SkBidiSubsetFactory>();
};

namespace SkUnicodes::Apple {
sk_sp<SkUnicode> Make() { return sk_make_sp<SkUnicode_apple>(); }
}  // namespace SkUnicodes::Apple
