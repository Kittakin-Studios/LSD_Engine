#include "lsd/asset/data_table.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace lsd::asset {

bool DataTable::parse(const char* text, usize length) {
    chars_.clear();
    cells_.clear();
    rows_ = columns_ = 0;
    Array<u32> row_lengths(default_allocator());  // cells in each row as read
    Array<u32> raw(default_allocator());          // cell offsets as read (rows may be short)
    const char* p = text;
    const char* end = text + length;
    if (length >= 3 && static_cast<unsigned char>(p[0]) == 0xEF && static_cast<unsigned char>(p[1]) == 0xBB && static_cast<unsigned char>(p[2]) == 0xBF) p += 3;  // Excel's UTF-8 mark
    u32 in_row = 0;
    while (p < end) {
        raw.push_back(static_cast<u32>(chars_.size()));
        ++in_row;
        if (*p == '"') {  // quoted cell
            ++p;
            for (;;) {
                if (p >= end) return false;  // unclosed quote
                if (*p == '"') {
                    if (p + 1 < end && p[1] == '"') {
                        chars_.push_back('"');
                        p += 2;
                        continue;
                    }
                    ++p;
                    break;
                }
                chars_.push_back(*p++);
            }
            while (p < end && *p != ',' && *p != '\n' && *p != '\r') ++p;  // stray text after the quote is dropped
        } else {
            while (p < end && *p != ',' && *p != '\n' && *p != '\r') chars_.push_back(*p++);
        }
        chars_.push_back('\0');
        if (p < end && *p == ',') {
            ++p;
            if (p == end || *p == '\n' || *p == '\r') {  // trailing comma: one more empty cell
                raw.push_back(static_cast<u32>(chars_.size()));
                chars_.push_back('\0');
                ++in_row;
            } else {
                continue;
            }
        }
        if (p < end && *p == '\r') ++p;
        if (p < end && *p == '\n') ++p;
        row_lengths.push_back(in_row);
        if (in_row > columns_) columns_ = in_row;
        in_row = 0;
    }
    if (in_row) {
        row_lengths.push_back(in_row);
        if (in_row > columns_) columns_ = in_row;
    }
    // Pad short rows to a full grid.
    const u32 empty = static_cast<u32>(chars_.size());
    chars_.push_back('\0');
    usize next = 0;
    for (const u32 count : row_lengths) {
        if (count == 1 && chars_[raw[next]] == '\0') {  // blank line
            next += 1;
            continue;
        }
        for (u32 c = 0; c < columns_; ++c) cells_.push_back(c < count ? raw[next + c] : empty);
        next += count;
        ++rows_;
    }
    return true;
}

bool DataTable::load(const char* path) {
    FILE* file = std::fopen(path, "rb");
    if (!file) return false;
    Array<char> text(default_allocator());
    char chunk[8192];
    for (usize n; (n = std::fread(chunk, 1, sizeof chunk, file)) > 0;) {
        const usize at = text.size();
        text.resize(at + n);
        std::memcpy(text.data() + at, chunk, n);
    }
    std::fclose(file);
    return parse(text.data(), text.size());
}

const char* DataTable::cell(u32 row, u32 column) const {
    if (row >= rows_ || column >= columns_) return "";
    return chars_.data() + cells_[row * columns_ + column];
}

i32 DataTable::find_row(const char* name) const {
    for (u32 r = 1; r < rows_; ++r) {
        if (std::strcmp(cell(r, 0), name) == 0) return static_cast<i32>(r);
    }
    return -1;
}

i32 DataTable::find_column(const char* name) const {
    for (u32 c = 0; c < columns_; ++c) {
        if (std::strcmp(cell(0, c), name) == 0) return static_cast<i32>(c);
    }
    return -1;
}

void write_csv(const char* const* cells, u32 rows, u32 columns, Array<char>& out) {
    out.clear();
    for (u32 r = 0; r < rows; ++r) {
        for (u32 c = 0; c < columns; ++c) {
            if (c) out.push_back(',');
            const char* text = cells[r * columns + c];
            const bool quote = std::strpbrk(text, ",\"\r\n") != nullptr || (text[0] == ' ');
            if (quote) out.push_back('"');
            for (const char* t = text; *t; ++t) {
                if (*t == '"') out.push_back('"');
                out.push_back(*t);
            }
            if (quote) out.push_back('"');
        }
        out.push_back('\n');
    }
}

ColumnType column_type(const char* header, char* name, usize capacity) {
    const char* colon = std::strrchr(header, ':');
    const usize length = colon ? static_cast<usize>(colon - header) : std::strlen(header);
    std::snprintf(name, capacity, "%.*s", static_cast<int>(length), header);
    if (!colon) return ColumnType::Auto;
    for (const ColumnType t : {ColumnType::Number, ColumnType::Int, ColumnType::Bool, ColumnType::String, ColumnType::Vector}) {
        if (std::strcmp(colon + 1, to_string(t)) == 0) return t;
    }
    std::snprintf(name, capacity, "%s", header);  // "a:b" that is not a type: part of the name
    return ColumnType::Auto;
}

const char* to_string(ColumnType type) {
    switch (type) {
        case ColumnType::Number: return "number";
        case ColumnType::Int: return "int";
        case ColumnType::Bool: return "bool";
        case ColumnType::String: return "string";
        case ColumnType::Vector: return "vector";
        case ColumnType::Auto: break;
    }
    return "auto";
}

namespace {

bool whole_number(const char* text, double& out) {
    char* stop = nullptr;
    out = std::strtod(text, &stop);
    return text[0] && stop && *stop == '\0';
}

bool vector_of(const char* text, double v[3]) {
    char extra = 0;
    return std::sscanf(text, "%lf , %lf , %lf %c", &v[0], &v[1], &v[2], &extra) == 3;
}

void append_text(Array<char>& out, const char* text) {
    for (; *text; ++text) out.push_back(*text);
}

void append_json_string(Array<char>& out, const char* text) {
    out.push_back('"');
    for (; *text; ++text) {
        if (*text == '"' || *text == '\\') out.push_back('\\');
        if (*text == '\n') {
            append_text(out, "\\n");
            continue;
        }
        out.push_back(*text);
    }
    out.push_back('"');
}

} // namespace

bool cell_fits(ColumnType type, const char* cell) {
    if (!cell[0]) return true;
    double number = 0.0, v[3];
    switch (type) {
        case ColumnType::Number: return whole_number(cell, number);
        case ColumnType::Int: return whole_number(cell, number) && number == static_cast<double>(static_cast<long long>(number));
        case ColumnType::Bool: return std::strcmp(cell, "true") == 0 || std::strcmp(cell, "false") == 0 || std::strcmp(cell, "1") == 0 || std::strcmp(cell, "0") == 0;
        case ColumnType::Vector: return vector_of(cell, v);
        case ColumnType::String:
        case ColumnType::Auto: break;
    }
    return true;
}

void export_json(const DataTable& table, Array<char>& out) {
    out.clear();
    append_text(out, "{");
    for (u32 r = 1; r < table.rows(); ++r) {
        append_text(out, r > 1 ? ",\n  " : "\n  ");
        append_json_string(out, table.cell(r, 0));
        append_text(out, ": {");
        for (u32 c = 1; c < table.columns(); ++c) {
            char name[128];
            const ColumnType type = column_type(table.cell(0, c), name, sizeof name);
            const char* cell = table.cell(r, c);
            if (c > 1) append_text(out, ", ");
            append_json_string(out, name);
            append_text(out, ": ");
            double number = 0.0, v[3];
            char buffer[128];
            if ((type == ColumnType::Number || type == ColumnType::Int || type == ColumnType::Auto) && whole_number(cell, number)) {
                std::snprintf(buffer, sizeof buffer, "%.17g", number);
                append_text(out, buffer);
            } else if (type == ColumnType::Bool) {
                append_text(out, std::strcmp(cell, "true") == 0 || std::strcmp(cell, "1") == 0 ? "true" : "false");
            } else if (type == ColumnType::Vector && vector_of(cell, v)) {
                std::snprintf(buffer, sizeof buffer, "{\"x\": %.9g, \"y\": %.9g, \"z\": %.9g}", v[0], v[1], v[2]);
                append_text(out, buffer);
            } else if ((type == ColumnType::Number || type == ColumnType::Int) && !cell[0]) {
                append_text(out, "0");
            } else {
                append_json_string(out, cell);
            }
        }
        append_text(out, "}");
    }
    append_text(out, "\n}\n");
}

} // namespace lsd::asset
