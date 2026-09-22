#include "change_planner/core/utf8.hpp"

#include <cstdint>
#include <string>

namespace cplan {

bool append_utf8(std::string& out, std::uint32_t code_point) {
  if (code_point > 0x10FFFFu) {
    return false;
  }
  if (code_point >= 0xD800u && code_point <= 0xDFFFu) {
    return false;
  }
  if (code_point < 0x80u) {
    out.push_back(static_cast<char>(code_point));
  } else if (code_point < 0x800u) {
    out.push_back(static_cast<char>(0xC0u | (code_point >> 6)));
    out.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
  } else if (code_point < 0x10000u) {
    out.push_back(static_cast<char>(0xE0u | (code_point >> 12)));
    out.push_back(static_cast<char>(0x80u | ((code_point >> 6) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
  } else {
    out.push_back(static_cast<char>(0xF0u | (code_point >> 18)));
    out.push_back(static_cast<char>(0x80u | ((code_point >> 12) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | ((code_point >> 6) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
  }
  return true;
}

bool is_valid_utf8(std::string_view text) noexcept {
  std::size_t index = 0;
  while (index < text.size()) {
    const auto first = static_cast<unsigned char>(text[index]);
    if (first < 0x80u) {
      ++index;
      continue;
    }
    std::size_t continuations = 0;
    std::uint32_t code_point = 0;
    if ((first & 0xE0u) == 0xC0u) {
      continuations = 1;
      code_point = first & 0x1Fu;
    } else if ((first & 0xF0u) == 0xE0u) {
      continuations = 2;
      code_point = first & 0x0Fu;
    } else if ((first & 0xF8u) == 0xF0u) {
      continuations = 3;
      code_point = first & 0x07u;
    } else {
      return false;
    }
    if (index + continuations >= text.size()) {
      return false;
    }
    for (std::size_t k = 1; k <= continuations; ++k) {
      const auto next = static_cast<unsigned char>(text[index + k]);
      if ((next & 0xC0u) != 0x80u) {
        return false;
      }
      code_point = (code_point << 6) | (next & 0x3Fu);
    }
    if (continuations == 1 && code_point < 0x80u) {
      return false;
    }
    if (continuations == 2 && code_point < 0x800u) {
      return false;
    }
    if (continuations == 3 && code_point < 0x10000u) {
      return false;
    }
    if (code_point > 0x10FFFFu) {
      return false;
    }
    if (code_point >= 0xD800u && code_point <= 0xDFFFu) {
      return false;
    }
    index += continuations + 1;
  }
  return true;
}

}  // namespace cplan
