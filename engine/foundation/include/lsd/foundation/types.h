#pragma once
// Fixed-width types and small helpers used across LSD_Engine.
#include <cstddef>
#include <cstdint>

namespace lsd {

using u8  = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i8  = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using f32 = float;
using f64 = double;
using usize = std::size_t;

constexpr usize KiB = 1024;
constexpr usize MiB = 1024 * KiB;
constexpr usize GiB = 1024 * MiB;

constexpr usize align_up(usize value, usize alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

} // namespace lsd
