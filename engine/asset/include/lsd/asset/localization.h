#pragma once
// Localisation (Unreal's String Tables): a Data Table (.csv) whose first column holds keys and the
// others one language each, named by their header ("en", "fr", "de"...):
//
//   Key,en,fr
//   MENU_PLAY,Play,Jouer
//
// text(key) gives the current language's text, falling back to the first language, then the key.
#include "lsd/asset/data_table.h"

namespace lsd::asset {

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
    u32 language_count() const { return table_.columns() > 1 ? table_.columns() - 1 : 0; }
    const char* language_code(u32 index) const { return table_.cell(0, index + 1); }

private:
    DataTable table_;
    u32 column_ = 1;
    char language_[16] = "en";
};

} // namespace lsd::asset
