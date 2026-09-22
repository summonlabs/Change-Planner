#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "change_planner/core/digest.hpp"
#include "change_planner/core/status.hpp"
#include "change_planner/core/strong.hpp"

namespace cplan {

// Canonical binary encoding: a single deterministic byte representation used
// both for content digests and for persisted artifacts. Every value is written
// with an explicit width and every variable-length field is length-prefixed.
class CanonicalEncoder {
 public:
  CanonicalEncoder() = default;

  void put_bool(bool value);
  void put_u8(std::uint8_t value);
  void put_u32(std::uint32_t value);
  void put_u64(std::uint64_t value);
  void put_i64(std::int64_t value);
  void put_text(std::string_view value);
  void put_blob(std::span<const std::byte> value);
  void put_fixed(std::span<const std::byte> value);

  // Domain separation tag: distinguishes encodings that would otherwise share a
  // prefix shape.
  void put_tag(std::string_view tag);

  const std::vector<std::byte>& data() const noexcept { return buffer_; }
  std::size_t size() const noexcept { return buffer_.size(); }

  Digest digest() const noexcept;

 private:
  std::vector<std::byte> buffer_{};
};

struct CanonicalLimits {
  std::size_t max_total_bytes = 64u * 1024u * 1024u;
  std::size_t max_text_bytes = 1u * 1024u * 1024u;
  std::size_t max_blob_bytes = 8u * 1024u * 1024u;
  std::size_t max_elements = 1u * 1024u * 1024u;
};

// Bounds-checked reader. Failures are sticky: the first failure is retained and
// every subsequent read returns a default value without touching out-of-range
// memory.
class CanonicalDecoder {
 public:
  explicit CanonicalDecoder(std::span<const std::byte> data, CanonicalLimits limits = {});

  bool ok() const noexcept { return error_ == ErrorCode::ok; }
  ErrorCode error() const noexcept { return error_; }
  const std::string& error_message() const noexcept { return error_message_; }

  void fail(ErrorCode code, std::string message);

  bool get_bool();
  std::uint8_t get_u8();
  std::uint32_t get_u32();
  std::uint64_t get_u64();
  std::int64_t get_i64();
  std::string get_text();
  std::vector<std::byte> get_blob();
  std::vector<std::byte> get_fixed(std::size_t size);
  void get_tag(std::string_view expected);

  // Reads a bounded element count and verifies that the remaining payload could
  // physically hold that many elements, so a corrupt length cannot drive an
  // enormous allocation.
  std::uint32_t get_count(std::size_t minimum_bytes_per_element);

  std::size_t remaining() const noexcept { return data_.size() - position_; }
  std::size_t position() const noexcept { return position_; }
  bool at_end() const noexcept { return position_ == data_.size(); }

  std::span<const std::byte> rest() const noexcept { return data_.subspan(position_); }

 private:
  void require(std::size_t bytes, const char* what);

  std::span<const std::byte> data_;
  CanonicalLimits limits_;
  std::size_t position_{0};
  ErrorCode error_{ErrorCode::ok};
  std::string error_message_{};
};

// ---------------------------------------------------------------------------
// Typed codecs. Every encodable type provides a Codec<T> specialization, which
// keeps encoding and decoding symmetric and avoids overload ambiguity.
// ---------------------------------------------------------------------------

template <class T>
struct Codec;

template <>
struct Codec<bool> {
  static void encode(CanonicalEncoder& out, bool value) { out.put_bool(value); }
  static bool decode(CanonicalDecoder& in) { return in.get_bool(); }
};

template <>
struct Codec<std::uint8_t> {
  static void encode(CanonicalEncoder& out, std::uint8_t value) { out.put_u8(value); }
  static std::uint8_t decode(CanonicalDecoder& in) { return in.get_u8(); }
};

template <>
struct Codec<std::uint32_t> {
  static void encode(CanonicalEncoder& out, std::uint32_t value) { out.put_u32(value); }
  static std::uint32_t decode(CanonicalDecoder& in) { return in.get_u32(); }
};

template <>
struct Codec<std::uint64_t> {
  static void encode(CanonicalEncoder& out, std::uint64_t value) { out.put_u64(value); }
  static std::uint64_t decode(CanonicalDecoder& in) { return in.get_u64(); }
};

template <>
struct Codec<std::int64_t> {
  static void encode(CanonicalEncoder& out, std::int64_t value) { out.put_i64(value); }
  static std::int64_t decode(CanonicalDecoder& in) { return in.get_i64(); }
};

template <>
struct Codec<std::string> {
  static void encode(CanonicalEncoder& out, const std::string& value) { out.put_text(value); }
  static std::string decode(CanonicalDecoder& in) { return in.get_text(); }
};

template <>
struct Codec<Digest> {
  static void encode(CanonicalEncoder& out, const Digest& value) {
    out.put_fixed(std::span<const std::byte>(reinterpret_cast<const std::byte*>(value.data()),
                                             Digest::kSize));
  }
  static Digest decode(CanonicalDecoder& in) {
    const std::vector<std::byte> raw = in.get_fixed(Digest::kSize);
    if (!in.ok()) {
      return Digest{};
    }
    Digest::bytes_type bytes{};
    for (std::size_t i = 0; i < Digest::kSize; ++i) {
      bytes[i] = static_cast<std::uint8_t>(raw[i]);
    }
    return Digest::from_bytes(bytes);
  }
};

template <class Tag>
struct Codec<NameId<Tag>> {
  static void encode(CanonicalEncoder& out, const NameId<Tag>& value) { out.put_text(value.str()); }

  static NameId<Tag> decode(CanonicalDecoder& in) {
    std::string text = in.get_text();
    if (!in.ok()) {
      return NameId<Tag>{};
    }
    // An empty field is the canonical representation of an absent optional
    // reference (for example the parent plan of an initial plan).
    if (text.empty()) {
      return NameId<Tag>{};
    }
    if (!is_valid_identity_text(text)) {
      in.fail(ErrorCode::malformed_input, std::string("identity text rejected for ") +
                                              std::string(Tag::kind_name()) + " during decode");
      return NameId<Tag>{};
    }
    return NameId<Tag>::from_validated(std::move(text));
  }
};

template <class Tag, class Rep>
struct Codec<Scalar<Tag, Rep>> {
  static void encode(CanonicalEncoder& out, Scalar<Tag, Rep> value) {
    static_assert(sizeof(Rep) <= sizeof(std::uint64_t), "Scalar representation too wide to encode");
    if constexpr (std::is_signed_v<Rep>) {
      out.put_i64(static_cast<std::int64_t>(value.value()));
    } else {
      out.put_u64(static_cast<std::uint64_t>(value.value()));
    }
  }

  static Scalar<Tag, Rep> decode(CanonicalDecoder& in) {
    if constexpr (std::is_signed_v<Rep>) {
      const std::int64_t raw = in.get_i64();
      if (!in.ok()) {
        return Scalar<Tag, Rep>{};
      }
      if (raw < static_cast<std::int64_t>((std::numeric_limits<Rep>::min)()) ||
          raw > static_cast<std::int64_t>((std::numeric_limits<Rep>::max)())) {
        in.fail(ErrorCode::malformed_input,
                std::string("scalar out of range for ") + std::string(Tag::kind_name()));
        return Scalar<Tag, Rep>{};
      }
      return Scalar<Tag, Rep>(static_cast<Rep>(raw));
    } else {
      const std::uint64_t raw = in.get_u64();
      if (!in.ok()) {
        return Scalar<Tag, Rep>{};
      }
      if (raw > static_cast<std::uint64_t>((std::numeric_limits<Rep>::max)())) {
        in.fail(ErrorCode::malformed_input,
                std::string("scalar out of range for ") + std::string(Tag::kind_name()));
        return Scalar<Tag, Rep>{};
      }
      return Scalar<Tag, Rep>(static_cast<Rep>(raw));
    }
  }
};

template <class T>
void encode_value(CanonicalEncoder& out, const T& value) {
  Codec<T>::encode(out, value);
}

template <class T>
T decode_value(CanonicalDecoder& in) {
  return Codec<T>::decode(in);
}

// Sequence encoders for domain types: the element codec is supplied explicitly
// so that no implicit overload resolution decides how an entity is written.
template <class T, class EncodeFn>
void encode_list(CanonicalEncoder& out, const std::vector<T>& values, EncodeFn encode_one) {
  out.put_u32(static_cast<std::uint32_t>(values.size()));
  for (const T& value : values) {
    encode_one(out, value);
  }
}

template <class T, class DecodeFn>
std::vector<T> decode_list(CanonicalDecoder& in, std::size_t minimum_bytes_per_element,
                           DecodeFn decode_one) {
  std::vector<T> values;
  const std::uint32_t count = in.get_count(minimum_bytes_per_element);
  if (!in.ok()) {
    return values;
  }
  values.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    values.push_back(decode_one(in));
    if (!in.ok()) {
      return {};
    }
  }
  return values;
}

template <class T>
void encode_sequence(CanonicalEncoder& out, const std::vector<T>& values) {
  out.put_u32(static_cast<std::uint32_t>(values.size()));
  for (const T& value : values) {
    Codec<T>::encode(out, value);
  }
}

template <class T>
std::vector<T> decode_sequence(CanonicalDecoder& in, std::size_t minimum_bytes_per_element) {
  std::vector<T> values;
  const std::uint32_t count = in.get_count(minimum_bytes_per_element);
  if (!in.ok()) {
    return values;
  }
  values.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    values.push_back(Codec<T>::decode(in));
    if (!in.ok()) {
      return {};
    }
  }
  return values;
}

}  // namespace cplan
