#include "lsd/asset/localization.h"

#include <cstdio>
#include <cstring>

namespace lsd::asset {

bool Localization::load(const char* csv_path) {
    const bool ok = table_.load(csv_path);
    set_language(language_);
    return ok;
}

bool Localization::set_language(const char* code) {
    std::snprintf(language_, sizeof language_, "%s", code);
    column_ = 1;
    for (u32 c = 1; c < table_.columns(); ++c) {
        if (std::strcmp(table_.cell(0, c), code) == 0) {
            column_ = c;
            return true;
        }
    }
    return false;
}

const char* Localization::find(const char* key) const {
    for (u32 r = 1; r < table_.rows(); ++r) {
        if (std::strcmp(table_.cell(r, 0), key) != 0) continue;
        const char* t = table_.cell(r, column_);
        return t[0] ? t : table_.cell(r, 1);  // an untranslated cell: the first language
    }
    return nullptr;
}

} // namespace lsd::asset
