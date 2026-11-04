// Line markers for JavaScript breakpoints (QuickJS has no debugger hooks): a small tokenizer finds
// where a statement starts a line and puts `__lsd_line(n, self);` there. It only needs to be sure,
// not complete: a line it is unsure about gets no marker (a breakpoint there stops at the next one).
#include "js.h"

#include <cctype>
#include <cstring>
#include <vector>

namespace lsd::script::js {

namespace {

bool word_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$' || static_cast<unsigned char>(c) >= 0x80; }

bool one_of(const std::string& w, std::initializer_list<const char*> list) {
    for (const char* s : list) {
        if (w == s) return true;
    }
    return false;
}

// After these, `/` starts a regular expression rather than a division.
bool regex_after(const std::string& last) {
    if (last.empty()) return true;
    if (word_char(last.back())) return one_of(last, {"return", "typeof", "case", "do", "else", "in", "of", "new", "delete", "void", "throw", "instanceof", "yield", "await"});
    return !one_of(last, {")", "]", "}"});
}

} // namespace

std::string instrument(const std::string& src) {
    std::string out;
    out.reserve(src.size() + src.size() / 2);
    // Brace kinds: 'b' a block (statements may go inside), 'o' an object literal or class body,
    // 't' a template literal's ${ } (back to the template's text when it closes).
    std::vector<char> braces;
    int parens = 0, brackets = 0;
    std::string last;           // the previous token ("" at the start)
    char last_closed = 0;       // the kind of the brace `last` == "}" closed
    bool pending_class = false; // `class` seen: its next { is a class body
    bool line_start = true;
    i32 line = 1;
    usize i = 0;
    const usize n = src.size();

    // Inside a template literal's text until its end or a ${.
    auto template_text = [&]() {
        while (i < n) {
            const char c = src[i];
            if (c == '\\' && i + 1 < n) {
                out += src.substr(i, 2);
                line += src[i + 1] == '\n';
                i += 2;
                continue;
            }
            out += c;
            ++i;
            if (c == '\n') ++line;
            if (c == '`') {
                last = "`";
                return;
            }
            if (c == '$' && i < n && src[i] == '{') {
                out += '{';
                ++i;
                braces.push_back('t');
                last = "{";
                return;
            }
        }
    };

    while (i < n) {
        const char c = src[i];
        if (c == '\n') {
            out += c;
            ++i;
            ++line;
            line_start = true;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r') {
            out += c;
            ++i;
            continue;
        }
        // Comments.
        if (c == '/' && i + 1 < n && src[i + 1] == '/') {
            const usize end = src.find('\n', i);
            const usize stop = end == std::string::npos ? n : end;
            out.append(src, i, stop - i);
            i = stop;
            continue;
        }
        if (c == '/' && i + 1 < n && src[i + 1] == '*') {
            const usize end = src.find("*/", i + 2);
            const usize stop = end == std::string::npos ? n : end + 2;
            for (usize k = i; k < stop; ++k) line += src[k] == '\n';
            out.append(src, i, stop - i);
            i = stop;
            continue;
        }
        // A statement starting this line: the marker goes before it.
        if (line_start) {
            line_start = false;
            usize w = i;
            while (w < n && word_char(src[w])) ++w;
            const std::string first = w > i ? src.substr(i, w - i) : std::string(1, c);
            const bool in_block = parens == 0 && brackets == 0 && (braces.empty() || braces.back() == 'b');
            const bool ended = last.empty() || last == ";" || last == "{" || (last == "}" && last_closed == 'b');
            bool starts = in_block && ended && !one_of(first, {"else", "catch", "finally", "case", "default", "while", "}", ")", "]"});
            // After a closing brace the line may continue its expression (ASI hazards).
            if (last == "}" && std::strchr("([`+-*/%.,?:=<>&|^", c)) starts = false;
            if (last.empty() && (c == '"' || c == '\'')) starts = false;  // "use strict" must stay first
            if (starts) {
                char marker[96];
                std::snprintf(marker, sizeof marker, "__lsd_line(%d,typeof self!==\"undefined\"?self:void 0);", line);
                out += marker;
            }
        }
        // Strings.
        if (c == '"' || c == '\'') {
            usize k = i + 1;
            while (k < n && src[k] != c && src[k] != '\n') k += src[k] == '\\' ? 2 : 1;
            k = k < n ? k + 1 : n;
            for (usize m = i; m < k; ++m) line += src[m] == '\n';  // line continuations
            out.append(src, i, k - i);
            i = k;
            last = "\"";
            continue;
        }
        if (c == '`') {
            out += c;
            ++i;
            template_text();
            continue;
        }
        // Regular expressions.
        if (c == '/' && regex_after(last)) {
            usize k = i + 1;
            bool in_class = false;
            while (k < n && src[k] != '\n') {
                if (src[k] == '\\') {
                    k += 2;
                    continue;
                }
                if (src[k] == '[') in_class = true;
                else if (src[k] == ']') in_class = false;
                else if (src[k] == '/' && !in_class) break;
                ++k;
            }
            ++k;
            while (k < n && word_char(src[k])) ++k;  // flags
            k = k < n ? k : n;
            out.append(src, i, k - i);
            i = k;
            last = "/re/";
            continue;
        }
        // Words and numbers.
        if (word_char(c)) {
            usize k = i;
            while (k < n && (word_char(src[k]) || (src[k] == '.' && std::isdigit(static_cast<unsigned char>(c))))) ++k;
            last = src.substr(i, k - i);
            if (last == "class") pending_class = true;
            out.append(src, i, k - i);
            i = k;
            continue;
        }
        // Punctuation.
        if (c == '=' && i + 1 < n && src[i + 1] == '>') {
            out += "=>";
            i += 2;
            last = "=>";
            continue;
        }
        switch (c) {
            case '(': ++parens; break;
            case ')': parens -= parens > 0; break;
            case '[': ++brackets; break;
            case ']': brackets -= brackets > 0; break;
            case '{': {
                char kind = 'o';
                if (pending_class) pending_class = false;  // a class body holds methods, not statements
                else if (one_of(last, {")", "=>", ";", "{", "}", "", "else", "try", "finally", "do"})) kind = 'b';
                else if (last == ":" && !braces.empty() && braces.back() == 'b') kind = 'b';  // case 1: { ... } / a label
                braces.push_back(kind);
                break;
            }
            case '}': {
                last_closed = braces.empty() ? 'b' : braces.back();
                if (!braces.empty()) braces.pop_back();
                out += c;
                ++i;
                if (last_closed == 't') {  // back into the template's text
                    template_text();
                    continue;
                }
                last = "}";
                continue;
            }
            default: break;
        }
        out += c;
        ++i;
        last = std::string(1, c);
    }
    return out;
}

} // namespace lsd::script::js
