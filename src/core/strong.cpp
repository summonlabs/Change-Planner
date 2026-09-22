#include "change_planner/core/strong.hpp"

#include <cstddef>

namespace cplan {

bool is_valid_identity_text(std::string_view text) noexcept {
  if (text.empty() || text.size() > kMaxIdentityLength) {
    return false;
  }
  for (const char raw : text) {
    const auto byte = static_cast<unsigned char>(raw);
    if (byte < 0x21 || byte > 0x7E) {
      return false;  // control characters, space and non-ASCII are rejected
    }
    switch (raw) {
      case '"':
      case '\\':
      case '\'':
      case '`':
      case '<':
      case '>':
      case '|':
      case '?':
      case '*':
      case '&':
      case '^':
      case '%':
      case '#':
      case '!':
      case '(':
      case ')':
      case '[':
      case ']':
      case '{':
      case '}':
      case ',':
      case ';':
      case '$':
      case '~':
      case '=':
      case '+':
        return false;
      default:
        break;
    }
  }
  return true;
}

}  // namespace cplan
