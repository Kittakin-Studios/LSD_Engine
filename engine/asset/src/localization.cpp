#include "lsd/asset/localization.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace lsd::asset {

namespace {

// This number's plural form in this language (CLDR's cardinal rules for whole numbers): "one", "two",
// "few", "many" or "other". A text without that form uses "other". Fractions are "other" (French: below 2 "one").
const char* plural_form(f64 n, const char* language) {
    auto is = [&](const char* code) { return std::strncmp(language, code, 2) == 0 && (language[2] == '\0' || language[2] == '-' || language[2] == '_'); };
    if (is("fr") || is("pt")) return n >= 0.0 && n < 2.0 ? "one" : "other";
    if (is("ja") || is("zh") || is("ko") || is("th") || is("vi") || is("id") || is("ms")) return "other";  // no plural forms
    if (n < 0.0) n = -n;
    if (n != static_cast<f64>(static_cast<u64>(n))) return "other";
    const u64 i = static_cast<u64>(n), mod10 = i % 10, mod100 = i % 100;
    if (is("ru") || is("uk") || is("be")) {
        if (mod10 == 1 && mod100 != 11) return "one";
        if (mod10 >= 2 && mod10 <= 4 && (mod100 < 12 || mod100 > 14)) return "few";
        return "many";
    }
    if (is("pl")) {
        if (i == 1) return "one";
        if (mod10 >= 2 && mod10 <= 4 && (mod100 < 12 || mod100 > 14)) return "few";
        return "many";
    }
    if (is("cs") || is("sk")) return i == 1 ? "one" : i >= 2 && i <= 4 ? "few" : "other";
    if (is("ar")) {
        if (i == 0) return "zero";
        if (i == 1) return "one";
        if (i == 2) return "two";
        if (mod100 >= 3 && mod100 <= 10) return "few";
        if (mod100 >= 11) return "many";
        return "other";
    }
    if (is("he")) return i == 1 ? "one" : i == 2 ? "two" : "other";
    return i == 1 ? "one" : "other";  // English, German, Spanish, Italian, Dutch, Swedish... (and the default)
}

// The `name=` choice of a "plural(one=apple,other=apples)" body [begin, end), or false.
bool plural_choice(const char* begin, const char* end, const char* name, const char*& from, const char*& to) {
    const usize length = std::strlen(name);
    for (const char* p = begin; p < end;) {
        const char* comma = p;
        while (comma < end && *comma != ',') ++comma;
        if (static_cast<usize>(comma - p) > length && std::strncmp(p, name, length) == 0 && p[length] == '=') {
            from = p + length + 1;
            to = comma;
            return true;
        }
        p = comma + 1;
    }
    return false;
}

} // namespace

void format_text(const char* pattern, const char* const* args, u32 arg_count, const char* language, char* out, usize capacity) {
    if (capacity == 0) return;
    usize n = 0;
    auto put = [&](const char* from, const char* to) {
        while (from < to && n + 1 < capacity) out[n++] = *from++;
    };
    for (const char* p = pattern; *p;) {
        // {N} or {N}|plural(...); anything else is copied as it is.
        if (p[0] == '{' && p[1] >= '0' && p[1] <= '9' && p[2] == '}') {
            const u32 index = static_cast<u32>(p[1] - '0');
            const char* arg = index < arg_count && args[index] ? args[index] : "";
            p += 3;
            if (std::strncmp(p, "|plural(", 8) == 0) {
                const char* body = p + 8;
                const char* close = std::strchr(body, ')');
                if (close) {
                    const f64 value = std::atof(arg);
                    const char *from = nullptr, *to = nullptr;
                    const bool found = (value == 0.0 && plural_choice(body, close, "zero", from, to)) ||  // zero= written: always at 0
                                       plural_choice(body, close, plural_form(value, language), from, to) ||
                                       plural_choice(body, close, "other", from, to);
                    if (found) put(from, to);
                    p = close + 1;
                    continue;
                }
            }
            put(arg, arg + std::strlen(arg));
            continue;
        }
        put(p, p + 1);
        ++p;
    }
    out[n] = '\0';
}

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
