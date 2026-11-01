#pragma once
// Data tables (Unreal's Data Table from CSV): a .csv file in Content is the asset. The first row
// names the columns, the first column names the rows ("Pistol", "Rifle"...). Cells are text;
// readers turn them into numbers where they look like numbers. Being plain CSV, tables open in
// Excel / Google Sheets and merge in source control.
#include "lsd/foundation/array.h"

namespace lsd::asset {

constexpr char kDataTableExtension[] = ".csv";

class DataTable {
public:
    explicit DataTable(Allocator& allocator) : chars_(allocator), cells_(allocator) {}

    // RFC 4180 CSV: commas, "quoted, cells" with "" for a quote, line breaks inside quotes,
    // \n or \r\n line ends. Short rows are padded with empty cells. False if a quote is unclosed.
    bool parse(const char* text, usize length);
    bool load(const char* path);

    u32 rows() const { return rows_; }        // including the header row
    u32 columns() const { return columns_; }  // including the row-name column
    const char* cell(u32 row, u32 column) const;  // "" outside the table
    i32 find_row(const char* name) const;     // by first-column text, header excluded; -1 if none
    i32 find_column(const char* name) const;  // by header text; -1 if none

private:
    Array<char> chars_;  // every cell, NUL-terminated
    Array<u32> cells_;   // offset of each cell in chars_, row by row (rows_ x columns_)
    u32 rows_ = 0, columns_ = 0;
};

// Writes cells (row by row, `columns` per row) as CSV, quoting where needed.
void write_csv(const char* const* cells, u32 rows, u32 columns, Array<char>& out);

// Typed columns (Unreal's row struct): a header "Damage:number" names the column Damage and types it.
// Auto (no type) reads numbers where cells look like numbers, text otherwise.
enum class ColumnType : u8 { Auto, Number, Int, Bool, String, Vector };
// The header's type, and its name without the ":type" into `name`. Unknown types count as Auto.
ColumnType column_type(const char* header, char* name, usize capacity);
const char* to_string(ColumnType type);
// Does the cell fit its column's type? (empty cells always do)
bool cell_fits(ColumnType type, const char* cell);
// The table as JSON: an object of rows, each an object of typed values ({x, y, z} for vectors).
void export_json(const DataTable& table, Array<char>& out);

} // namespace lsd::asset
