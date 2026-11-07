#pragma once
// Localisation (Unreal's String Tables): a Data Table (.csv) whose first column holds keys and the
// others one language each, named by their header ("en", "fr", "de"...):
//
//   Key,en,fr
//   MENU_PLAY,Play,Jouer
//
// text(key) gives the current language's text, falling back to the first language, then the key.
// format_text fills in arguments (Unreal's FText::Format):
//
//   SCORE,"Score: {0}"            {0}..{9}: the arguments in order
//   APPLES,"{0} {0}|plural(one=apple,other=apples)"     a word picked by the number's plural form
//
// Plural forms (CLDR's, for whole numbers): zero (when given, for 0), one, two, few, many, other; which numbers
// take which follows the language: English 1 one; French 0 and 1 one; Russian / Ukrainian 1, 21 one, 2-4, 22-24
// few, 5-20 many; Polish 1 one, 2-4, 22-24 few, others many; Czech / Slovak 2-4 few; Arabic zero, one, two,
// few (3-10), many (11-99); Hebrew two; Japanese, Chinese, Korean only other. A missing form uses other.
#include "lsd/asset/data_table.h"

namespace lsd::asset {

// Writes `pattern` with its {N} arguments filled in to `out` (always terminated, cut to fit).
void format_text(const char* pattern, const char* const* args, u32 arg_count, const char* language, char* out, usize capacity);

class Localization {
public:
    explicit Localization(Allocator& allocator) : table_(allocator) {}
    bool load(const char* csv_path);  // false (and empty) if it cannot be read
    bool set_language(const char* code);  // false if the table has no such column (the language is still remembered)
    const char* language() const { return language_; }
    // The text for `key`, or nullptr if the table has no such key.
    const char* find(const char* key) const;
    const char* text(const char* key) const {  // never null: the key itself when missing
        const char* t = find(key);
        return t ? t : key;
    }
    // text(key) with arguments filled in, in this language's plural rules.
    void format(const char* key, const char* const* args, u32 arg_count, char* out, usize capacity) const {
        format_text(text(key), args, arg_count, language_, out, capacity);
    }
    u32 language_count() const { return table_.columns() > 1 ? table_.columns() - 1 : 0; }
    const char* language_code(u32 index) const { return table_.cell(0, index + 1); }

private:
    DataTable table_;
    u32 column_ = 1;
    char language_[16] = "en";
};

} // namespace lsd::asset
