#include "rml_edit.h"

#include <cctype>
#include <cstring>
#include <utility>
#include <vector>

namespace lsd::editor {

namespace {

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\n' || s[a] == '\r')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\n' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

std::string tag_name(const std::string& rml, size_t begin) {
    size_t i = begin + 1;
    while (i < rml.size() && rml[i] != ' ' && rml[i] != '>' && rml[i] != '/' && rml[i] != '\n' && rml[i] != '\t') ++i;
    return rml.substr(begin + 1, i - begin - 1);
}

// The attribute's value range inside [begin, end); false if the tag does not have it.
bool find_attribute(const std::string& rml, size_t begin, size_t end, const char* name, size_t& value_begin, size_t& value_end) {
    const std::string key = std::string(name) + "=\"";
    for (size_t at = rml.find(key, begin); at != std::string::npos && at < end; at = rml.find(key, at + 1)) {
        const char before = rml[at - 1];
        if (before != ' ' && before != '\n' && before != '\t') continue;  // "data-id=" is not "id="
        value_begin = at + key.size();
        value_end = rml.find('"', value_begin);
        return value_end != std::string::npos && value_end < end;
    }
    return false;
}

using Properties = std::vector<std::pair<std::string, std::string>>;

Properties parse_style(const std::string& style) {
    Properties out;
    size_t start = 0;
    while (start < style.size()) {
        size_t semi = style.find(';', start);
        if (semi == std::string::npos) semi = style.size();
        const std::string item = style.substr(start, semi - start);
        const size_t colon = item.find(':');
        if (colon != std::string::npos) out.emplace_back(trim(item.substr(0, colon)), trim(item.substr(colon + 1)));
        start = semi + 1;
    }
    return out;
}

// One past the element's closing tag (or its own end if it closes itself / has no closing tag).
size_t element_end(const std::string& rml, size_t begin, size_t tag_end) {
    if (rml[tag_end - 2] == '/') return tag_end;
    const std::string name = tag_name(rml, begin);
    int depth = 1;
    size_t i = tag_end;
    while (depth > 0) {
        i = rml.find('<', i);
        if (i == std::string::npos) return tag_end;
        if (rml.compare(i + 1, name.size() + 1, "/" + name) == 0) {
            --depth;
            const size_t close = rml.find('>', i);
            if (close == std::string::npos) return tag_end;
            i = close + 1;
            if (depth == 0) return i;
            continue;
        }
        if (rml.compare(i + 1, name.size(), name) == 0) {
            const char after = rml[i + 1 + name.size()];
            const size_t close = rml.find('>', i);
            if ((after == ' ' || after == '>' || after == '/') && close != std::string::npos && rml[close - 1] != '/') ++depth;
        }
        ++i;
    }
    return i;
}

} // namespace

bool rml_find_tag(const std::string& rml, const char* id, size_t& begin, size_t& end) {
    const std::string key = std::string("id=\"") + id + "\"";
    for (size_t at = rml.find(key); at != std::string::npos; at = rml.find(key, at + 1)) {
        if (at == 0 || (rml[at - 1] != ' ' && rml[at - 1] != '\n' && rml[at - 1] != '\t')) continue;
        begin = rml.rfind('<', at);
        end = rml.find('>', at);
        if (begin == std::string::npos || end == std::string::npos) return false;
        if (rml.find('>', begin) < at) continue;  // the id is in text, not inside a tag
        ++end;
        return true;
    }
    return false;
}

std::string rml_attribute(const std::string& rml, const char* id, const char* name) {
    size_t begin, end, a, b;
    if (!rml_find_tag(rml, id, begin, end) || !find_attribute(rml, begin, end, name, a, b)) return {};
    return rml.substr(a, b - a);
}

bool rml_set_attribute(std::string& rml, const char* id, const char* name, const char* value) {
    size_t begin, end, a, b;
    if (!rml_find_tag(rml, id, begin, end)) return false;
    if (find_attribute(rml, begin, end, name, a, b)) {
        if (value[0]) {
            rml.replace(a, b - a, value);
        } else {  // remove ` name="..."`
            const size_t from = a - std::strlen(name) - 3;
            rml.erase(from, b + 1 - from);
        }
        return true;
    }
    if (!value[0]) return true;
    const size_t insert_at = rml[end - 2] == '/' ? end - 2 : end - 1;
    rml.insert(insert_at, std::string(" ") + name + "=\"" + value + "\"");
    return true;
}

std::string rml_style(const std::string& rml, const char* id, const char* property) {
    for (const auto& p : parse_style(rml_attribute(rml, id, "style"))) {
        if (p.first == property) return p.second;
    }
    return {};
}

bool rml_set_style(std::string& rml, const char* id, const char* property, const char* value) {
    size_t begin, end;
    if (!rml_find_tag(rml, id, begin, end)) return false;
    Properties props = parse_style(rml_attribute(rml, id, "style"));
    bool found = false;
    for (size_t i = 0; i < props.size(); ++i) {
        if (props[i].first != property) continue;
        found = true;
        if (value[0]) props[i].second = value;
        else props.erase(props.begin() + static_cast<std::ptrdiff_t>(i));
        break;
    }
    if (!found && value[0]) props.emplace_back(property, value);
    std::string style;
    for (const auto& p : props) style += (style.empty() ? "" : " ") + p.first + ": " + p.second + ";";
    return rml_set_attribute(rml, id, "style", style.c_str());
}

bool rml_text(const std::string& rml, const char* id, std::string& out) {
    size_t begin, end;
    if (!rml_find_tag(rml, id, begin, end) || rml[end - 2] == '/') return false;
    const size_t close = element_end(rml, begin, end);
    const size_t inner_end = rml.rfind('<', close - 1);
    if (inner_end == std::string::npos || inner_end < end) return false;
    const std::string inner = rml.substr(end, inner_end - end);
    if (inner.find('<') != std::string::npos) return false;  // has child elements
    out = inner;
    return true;
}

bool rml_set_text(std::string& rml, const char* id, const char* text) {
    std::string old;
    if (!rml_text(rml, id, old)) return false;
    size_t begin, end;
    rml_find_tag(rml, id, begin, end);
    rml.replace(end, old.size(), text);
    return true;
}

bool rml_append_to_body(std::string& rml, const std::string& markup) {
    const size_t at = rml.rfind("</body>");
    if (at == std::string::npos) return false;
    rml.insert(at, markup + "\n");
    return true;
}

bool rml_remove(std::string& rml, const char* id) {
    size_t begin, end;
    if (!rml_find_tag(rml, id, begin, end)) return false;
    size_t stop = element_end(rml, begin, end);
    if (stop < rml.size() && rml[stop] == '\n') ++stop;
    size_t start = begin;
    while (start > 0 && (rml[start - 1] == ' ' || rml[start - 1] == '\t')) --start;  // its indentation too
    rml.erase(start, stop - start);
    return true;
}

std::vector<RmlNode> rml_outline(const std::string& rml) {
    std::vector<RmlNode> out;
    size_t at = rml.find("<body");
    if (at == std::string::npos) return out;
    at = rml.find('>', at);
    int depth = 0;
    while (at != std::string::npos && (at = rml.find('<', at + 1)) != std::string::npos) {
        if (rml.compare(at, 4, "<!--") == 0) {  // a comment: skipped whole
            at = rml.find("-->", at);
            continue;
        }
        const size_t close = rml.find('>', at);
        if (close == std::string::npos) break;
        if (rml[at + 1] == '/') {  // an end tag
            if (rml.compare(at, 7, "</body>") == 0) break;
            --depth;
            at = close;
            continue;
        }
        size_t name_end = at + 1;
        while (name_end < close && !std::isspace(static_cast<unsigned char>(rml[name_end])) && rml[name_end] != '/' && rml[name_end] != '>') ++name_end;
        RmlNode node;
        node.tag = rml.substr(at + 1, name_end - at - 1);
        node.depth = depth;
        const size_t id = rml.find("id=\"", at);
        if (id != std::string::npos && id < close && std::isspace(static_cast<unsigned char>(rml[id - 1]))) {
            const size_t end = rml.find('"', id + 4);
            if (end != std::string::npos && end < close) node.id = rml.substr(id + 4, end - id - 4);
        }
        out.push_back(node);
        if (rml[close - 1] != '/') ++depth;  // <img .../> has no end tag
        at = close;
    }
    return out;
}

std::string rml_unique_id(const std::string& rml, const char* base) {
    for (int n = 1;; ++n) {
        const std::string id = std::string(base) + std::to_string(n);
        size_t b, e;
        if (!rml_find_tag(rml, id.c_str(), b, e)) return id;
    }
}

} // namespace lsd::editor
