// lsd_reflect_gen: reads one C++ header, finds LSD_STRUCT / LSD_PROPERTY / LSD_FUNCTION
// annotations and writes a .gen.cpp with the reflection tables (see lsd/reflect/reflect.h).
//
//   lsd_reflect_gen <header.h> <output.gen.cpp>
//
// It is a small tokenizer, not a C++ compiler: it understands namespaces, structs and simple
// field declarations. Anything it cannot handle is reported as an error with file and line.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>
#include <vector>

namespace {

enum class Kind { Ident, Number, String, Punct };

struct Token {
    Kind kind;
    std::string text;
    int line;
};

std::string g_header;
int g_errors = 0;

void error(int line, const std::string& message) {
    std::fprintf(stderr, "%s(%d): error: %s\n", g_header.c_str(), line, message.c_str());
    ++g_errors;
}

bool is_ident_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool is_ident(char c) { return is_ident_start(c) || (c >= '0' && c <= '9'); }

std::vector<Token> tokenize(const std::string& src) {
    std::vector<Token> tokens;
    int line = 1;
    bool line_start = true;
    for (size_t i = 0; i < src.size();) {
        const char c = src[i];
        if (c == '\n') { ++line; line_start = true; ++i; continue; }
        if (c == ' ' || c == '\t' || c == '\r') { ++i; continue; }
        if (c == '#' && line_start) {  // preprocessor line, with \ continuations
            while (i < src.size() && src[i] != '\n') {
                if (src[i] == '\\' && i + 1 < src.size() && src[i + 1] == '\n') { ++line; ++i; }
                ++i;
            }
            continue;
        }
        line_start = false;
        if (c == '/' && i + 1 < src.size() && src[i + 1] == '/') {
            while (i < src.size() && src[i] != '\n') ++i;
            continue;
        }
        if (c == '/' && i + 1 < src.size() && src[i + 1] == '*') {
            i += 2;
            while (i + 1 < src.size() && !(src[i] == '*' && src[i + 1] == '/')) { if (src[i] == '\n') ++line; ++i; }
            i += 2;
            continue;
        }
        const size_t start = i;
        if (c == '"' || c == '\'') {
            ++i;
            while (i < src.size() && src[i] != c) { if (src[i] == '\\') ++i; ++i; }
            ++i;
            tokens.push_back({Kind::String, src.substr(start, i - start), line});
        } else if (is_ident_start(c)) {
            while (i < src.size() && is_ident(src[i])) ++i;
            tokens.push_back({Kind::Ident, src.substr(start, i - start), line});
        } else if (c >= '0' && c <= '9') {
            while (i < src.size() && (is_ident(src[i]) || src[i] == '.' || src[i] == '\'')) ++i;
            tokens.push_back({Kind::Number, src.substr(start, i - start), line});
        } else if (c == ':' && i + 1 < src.size() && src[i + 1] == ':') {
            i += 2;
            tokens.push_back({Kind::Punct, "::", line});
        } else {
            ++i;
            tokens.push_back({Kind::Punct, std::string(1, c), line});
        }
    }
    return tokens;
}

struct Arg {
    std::string key;
    std::string value;  // empty for flags
    int line;
};

struct Field {
    std::string name;
    std::string type;  // FieldType enumerator
    std::string struct_type;
    unsigned flags = 0;
    std::string category = "\"\"", tooltip = "\"\"", units = "\"\"";
    std::string min = "0", max = "0";
    bool has_min = false, has_max = false;
};

struct Struct {
    std::string qualified_name;  // ns::Name
    unsigned flags = 0;
    std::string category = "\"\"";
    bool has_generated = false;
    int line = 0;
    std::vector<Field> fields;
};

class Parser {
public:
    explicit Parser(std::vector<Token> tokens) : t_(std::move(tokens)) {}

    std::vector<Struct> run() {
        while (i_ < t_.size()) step();
        return structs_;
    }

private:
    struct Scope {
        std::string ns;   // namespace name for namespace scopes
        int struct_index = -1;
    };

    const Token& peek(size_t ahead = 0) const {
        static const Token eof{Kind::Punct, "", 0};
        return i_ + ahead < t_.size() ? t_[i_ + ahead] : eof;
    }
    bool is(const char* text, size_t ahead = 0) const { return peek(ahead).text == text; }
    int current_struct() const { return scopes_.empty() ? -1 : scopes_.back().struct_index; }

    std::string namespace_prefix() const {
        std::string prefix;
        for (const Scope& s : scopes_) if (!s.ns.empty()) prefix += s.ns + "::";
        return prefix;
    }

    void step() {
        const Token& tok = t_[i_];
        if (tok.text == "namespace") {
            ++i_;
            std::string name;
            while (i_ < t_.size() && !is("{") && !is(";") && !is("=")) name += t_[i_++].text;
            if (is("{")) { ++i_; scopes_.push_back({name.empty() ? "(anonymous)" : name}); }
            return;
        }
        if (tok.text == "LSD_STRUCT") { ++i_; parse_struct(parse_args(), tok.line); return; }
        if (tok.text == "LSD_PROPERTY") {
            ++i_;
            std::vector<Arg> args = parse_args();
            if (current_struct() < 0) { error(tok.line, "LSD_PROPERTY outside an LSD_STRUCT"); return; }
            parse_field(args, tok.line);
            return;
        }
        if (tok.text == "LSD_FUNCTION") {
            ++i_;
            std::vector<Arg> args = parse_args();
            check_args(args, {"Callable"}, {"Category"}, "LSD_FUNCTION");
            // shortcut: functions are validated but not generated until visual scripting (milestone 4).
            return;
        }
        if (tok.text == "LSD_GENERATED") {
            if (current_struct() >= 0) structs_[static_cast<size_t>(current_struct())].has_generated = true;
            ++i_;
            return;
        }
        if (tok.text == "{") { scopes_.push_back({}); ++i_; return; }
        if (tok.text == "}") {
            if (!scopes_.empty()) {
                const int s = scopes_.back().struct_index;
                if (s >= 0 && !structs_[static_cast<size_t>(s)].has_generated) {
                    error(structs_[static_cast<size_t>(s)].line, "add LSD_GENERATED() inside the body of " + structs_[static_cast<size_t>(s)].qualified_name);
                }
                scopes_.pop_back();
            }
            ++i_;
            return;
        }
        ++i_;
    }

    std::vector<Arg> parse_args() {
        std::vector<Arg> args;
        if (!is("(")) { error(peek().line, "expected '(' after annotation"); return args; }
        ++i_;
        while (i_ < t_.size() && !is(")")) {
            Arg arg{peek().text, "", peek().line};
            if (peek().kind != Kind::Ident) { error(arg.line, "expected a flag or Key = value, got '" + arg.key + "'"); }
            ++i_;
            if (is("=")) {
                ++i_;
                if (is("-")) { arg.value = "-"; ++i_; }
                arg.value += peek().text;
                ++i_;
            }
            args.push_back(arg);
            if (is(",")) ++i_;
            else if (!is(")")) { error(peek().line, "expected ',' or ')' in annotation"); break; }
        }
        ++i_;  // ')'
        return args;
    }

    bool check_args(const std::vector<Arg>& args, std::initializer_list<const char*> flags,
                    std::initializer_list<const char*> keys, const char* what) {
        bool ok = true;
        for (const Arg& a : args) {
            bool known = false;
            for (const char* f : (a.value.empty() ? flags : keys)) known = known || a.key == f;
            if (!known) {
                error(a.line, std::string("unknown ") + (a.value.empty() ? "flag" : "key") + " '" + a.key + "' in " + what);
                ok = false;
            }
        }
        return ok;
    }

    void parse_struct(const std::vector<Arg>& args, int line) {
        Struct s;
        s.line = line;
        check_args(args, {"Component", "Color"}, {"Category"}, "LSD_STRUCT");
        for (const Arg& a : args) {
            if (a.key == "Component") s.flags |= 1u;
            if (a.key == "Color") s.flags |= 2u;
            if (a.key == "Category") s.category = a.value;
        }
        if (!is("struct") && !is("class")) { error(peek().line, "LSD_STRUCT must be followed by a struct"); return; }
        ++i_;
        if (peek().kind != Kind::Ident) { error(peek().line, "expected a struct name"); return; }
        s.qualified_name = namespace_prefix() + peek().text;
        ++i_;
        if (is("final")) ++i_;
        if (is(":")) { error(peek().line, "reflected structs cannot inherit; use composition"); return; }
        if (!is("{")) { error(peek().line, "expected '{' after struct name"); return; }
        ++i_;
        structs_.push_back(s);
        scopes_.push_back({"", static_cast<int>(structs_.size() - 1)});
    }

    void parse_field(const std::vector<Arg>& args, int line) {
        Field f;
        check_args(args, {"Edit", "Visible", "Save", "EditorOnly"}, {"Category", "Tooltip", "Units", "Min", "Max"}, "LSD_PROPERTY");
        for (const Arg& a : args) {
            if (a.key == "Edit") f.flags |= 1u;
            else if (a.key == "Visible") f.flags |= 2u;
            else if (a.key == "Save") f.flags |= 4u;
            else if (a.key == "EditorOnly") f.flags |= 8u;
            else if (a.key == "Category") f.category = a.value;
            else if (a.key == "Tooltip") f.tooltip = a.value;
            else if (a.key == "Units") f.units = a.value;
            else if (a.key == "Min") { f.min = a.value; f.has_min = true; }
            else if (a.key == "Max") { f.max = a.value; f.has_max = true; }
        }
        // Declaration: <type tokens> <name> [= init | {init}] ;
        std::vector<Token> decl;
        while (i_ < t_.size() && !is(";") && !is("=") && !is("{")) decl.push_back(t_[i_++]);
        int depth = 0;
        while (i_ < t_.size() && !(depth == 0 && is(";"))) {
            if (is("{") || is("(")) ++depth;
            if (is("}") || is(")")) --depth;
            ++i_;
        }
        ++i_;  // ';'
        if (decl.size() < 2) { error(line, "could not read the field after LSD_PROPERTY"); return; }
        // `char name[N]` is a fixed-size string; any other array is rejected below.
        size_t type_end = decl.size() - 1;
        if (decl.size() >= 5 && decl.back().text == "]" && decl[decl.size() - 3].text == "[" &&
            decl[decl.size() - 4].kind == Kind::Ident) {
            type_end = decl.size() - 4;
            f.name = decl[type_end].text;
            std::string element;
            for (size_t k = 0; k < type_end; ++k) element += (element.empty() ? "" : " ") + decl[k].text;
            if (element != "char") {
                error(decl[0].line, "field '" + f.name + "': only char arrays (strings) are reflected");
                return;
            }
            f.type = "String";
            structs_[static_cast<size_t>(current_struct())].fields.push_back(f);
            return;
        }
        if (decl.back().kind != Kind::Ident) { error(line, "could not read the field after LSD_PROPERTY"); return; }
        f.name = decl.back().text;
        std::string type;
        for (size_t k = 0; k < type_end; ++k) {
            const std::string& part = decl[k].text;
            if (part == "[" || part == "*" || part == "&" || part == "<" || part == "static") {
                error(decl[k].line, "field '" + f.name + "': arrays, pointers, references, templates and statics are not reflected yet");
                return;
            }
            if (part == "const" || part == "mutable") continue;
            if (!type.empty() && part != "::" && type.back() != ':') type += ' ';
            type += part;
        }
        if (!map_type(type, f)) return;
        structs_[static_cast<size_t>(current_struct())].fields.push_back(f);
    }

    static bool map_type(std::string type, Field& f) {
        for (const char* prefix : {"::", "lsd::", "std::"}) {
            if (type.rfind(prefix, 0) == 0) type.erase(0, std::string(prefix).size());
        }
        struct Map { const char* names[3]; const char* field_type; };
        static const Map maps[] = {
            {{"bool", "bool", "bool"}, "Bool"},
            {{"i32", "int32_t", "int"}, "I32"},
            {{"u32", "uint32_t", "unsigned int"}, "U32"},
            {{"i64", "int64_t", "long long"}, "I64"},
            {{"u64", "uint64_t", "unsigned long long"}, "U64"},
            {{"f32", "float", "float"}, "F32"},
            {{"f64", "double", "double"}, "F64"},
        };
        for (const Map& m : maps) {
            for (const char* n : m.names) {
                if (type == n) { f.type = m.field_type; return true; }
            }
        }
        f.type = "Struct";  // any other named type must itself be an LSD_STRUCT (checked by the compiler)
        f.struct_type = type;
        return true;
    }

    std::vector<Token> t_;
    size_t i_ = 0;
    std::vector<Scope> scopes_;
    std::vector<Struct> structs_;
};

std::string generate(const std::vector<Struct>& structs, const std::string& include_path) {
    std::ostringstream o;
    o << "// Generated by lsd_reflect_gen from " << include_path << ". Do not edit.\n";
    o << "#include \"" << include_path << "\"\n#include <cstddef>\n";
    for (const Struct& s : structs) {
        o << "\nconst ::lsd::reflect::TypeInfo& " << s.qualified_name << "::static_type() {\n";
        o << "    using Self = " << s.qualified_name << ";\n";
        if (!s.fields.empty()) {
            o << "    static const ::lsd::reflect::Property properties[] = {\n";
            for (const Field& f : s.fields) {
                o << "        {\"" << f.name << "\", ::lsd::reflect::FieldType::" << f.type
                  << ", static_cast<::lsd::u32>(offsetof(Self, " << f.name << ")), static_cast<::lsd::u32>(sizeof(Self::" << f.name
                  << ")), " << f.flags << "u, "
                  << f.category << ", " << f.tooltip << ", " << f.units << ", "
                  << f.min << ", " << f.max << ", " << (f.has_min ? "true" : "false") << ", "
                  << (f.has_max ? "true" : "false") << ", "
                  << (f.type == "Struct" ? "&" + f.struct_type + "::static_type()" : "nullptr") << "},\n";
            }
            o << "    };\n";
        }
        o << "    static const ::lsd::reflect::TypeInfo info{\"" << s.qualified_name.substr(s.qualified_name.rfind(':') == std::string::npos ? 0 : s.qualified_name.rfind(':') + 1)
          << "\", LSD_ID(\"" << s.qualified_name << "\"), sizeof(Self), alignof(Self), " << s.flags << "u, "
          << s.category << ", " << (s.fields.empty() ? "nullptr" : "properties") << ", " << s.fields.size() << "u};\n";
        o << "    return info;\n}\n";
    }
    return o.str();
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: lsd_reflect_gen <header.h> <output.gen.cpp>\n");
        return 2;
    }
    g_header = argv[1];
    std::ifstream in(g_header, std::ios::binary);
    if (!in) { std::fprintf(stderr, "lsd_reflect_gen: cannot open %s\n", argv[1]); return 1; }
    std::stringstream source;
    source << in.rdbuf();

    const std::vector<Struct> structs = Parser(tokenize(source.str())).run();
    if (g_errors) return 1;

    std::string include_path = g_header;
    for (char& c : include_path) if (c == '\\') c = '/';
    const std::string output = generate(structs, include_path);

    // Only rewrite when the content changes, so unchanged headers do not trigger recompiles.
    std::ifstream existing(argv[2], std::ios::binary);
    std::stringstream old;
    old << existing.rdbuf();
    if (existing && old.str() == output) return 0;
    std::ofstream out(argv[2], std::ios::binary);
    out << output;
    return out ? 0 : 1;
}
