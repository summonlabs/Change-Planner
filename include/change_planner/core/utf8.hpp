#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace cplan {

// Strict UTF-8 validation: rejects overlong encodings, surrogate code points,
// values above U+10FFFF and truncated sequences.
[[nodiscard]] bool is_valid_utf8(std::string_view text) noexcept;

// Appends the UTF-8 encoding of a code point. Returns false for surrogates and
// values above U+10FFFF.
[[nodiscard]] bool append_utf8(std::string& out, std::uint32_t code_point);

}  // namespace cplan
