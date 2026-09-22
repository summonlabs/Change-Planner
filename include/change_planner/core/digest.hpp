#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "change_planner/core/status.hpp"

namespace cplan {

// SHA-256 content digest. Used for plan content identity, input binding,
// artifact integrity and deterministic tie-breaking.
class Digest {
 public:
  static constexpr std::size_t kSize = 32;
  using bytes_type = std::array<std::uint8_t, kSize>;

  constexpr Digest() noexcept = default;

  static Digest from_bytes(const bytes_type& bytes) noexcept {
    Digest digest;
    digest.bytes_ = bytes;
    return digest;
  }

  static Result<Digest> from_hex(std::string_view hex);

  const bytes_type& bytes() const noexcept { return bytes_; }
  const std::uint8_t* data() const noexcept { return bytes_.data(); }

  std::string hex() const;
  // Short stable form for human-facing diagnostics and step identity text.
  std::string short_hex() const;

  bool is_zero() const noexcept;

  friend bool operator==(const Digest&, const Digest&) = default;
  friend std::strong_ordering operator<=>(const Digest& lhs, const Digest& rhs) noexcept {
    for (std::size_t i = 0; i < kSize; ++i) {
      if (lhs.bytes_[i] != rhs.bytes_[i]) {
        return lhs.bytes_[i] < rhs.bytes_[i] ? std::strong_ordering::less
                                             : std::strong_ordering::greater;
      }
    }
    return std::strong_ordering::equal;
  }

  std::size_t hash() const noexcept;

 private:
  bytes_type bytes_{};
};

// Streaming SHA-256 (FIPS 180-4).
class Sha256 {
 public:
  Sha256() noexcept;

  Sha256(const Sha256&) = delete;
  Sha256& operator=(const Sha256&) = delete;

  void update(const void* data, std::size_t size) noexcept;
  void update(std::span<const std::byte> data) noexcept { update(data.data(), data.size()); }
  void update(std::string_view text) noexcept { update(text.data(), text.size()); }

  // Finalizes the digest. The object must not be updated afterwards.
  [[nodiscard]] Digest finalize() noexcept;

  static Digest hash(std::string_view text) noexcept;
  static Digest hash(std::span<const std::byte> data) noexcept;

 private:
  void process_block(const std::uint8_t* block) noexcept;

  std::uint32_t state_[8];
  std::uint64_t total_bytes_;
  std::uint8_t buffer_[64];
  std::size_t buffer_length_;
};

}  // namespace cplan

namespace std {

template <>
struct hash<::cplan::Digest> {
  std::size_t operator()(const ::cplan::Digest& digest) const noexcept { return digest.hash(); }
};

}  // namespace std
