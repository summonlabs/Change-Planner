#include "change_planner/core/canonical.hpp"

#include <cstdint>

#include "change_planner/core/checked.hpp"
#include "change_planner/core/utf8.hpp"

namespace cplan {

void CanonicalEncoder::put_bool(bool value) {
  put_u8(value ? static_cast<std::uint8_t>(1) : static_cast<std::uint8_t>(0));
}

void CanonicalEncoder::put_u8(std::uint8_t value) {
  buffer_.push_back(static_cast<std::byte>(value));
}

void CanonicalEncoder::put_u32(std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    buffer_.push_back(static_cast<std::byte>((value >> shift) & 0xFFu));
  }
}

void CanonicalEncoder::put_u64(std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    buffer_.push_back(static_cast<std::byte>((value >> shift) & 0xFFu));
  }
}

void CanonicalEncoder::put_i64(std::int64_t value) {
  put_u64(static_cast<std::uint64_t>(value));
}

void CanonicalEncoder::put_fixed(std::span<const std::byte> value) {
  buffer_.insert(buffer_.end(), value.begin(), value.end());
}

void CanonicalEncoder::put_blob(std::span<const std::byte> value) {
  put_u32(static_cast<std::uint32_t>(value.size()));
  put_fixed(value);
}

void CanonicalEncoder::put_text(std::string_view value) {
  put_u32(static_cast<std::uint32_t>(value.size()));
  for (const char character : value) {
    buffer_.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
  }
}

void CanonicalEncoder::put_tag(std::string_view tag) { put_text(tag); }

Digest CanonicalEncoder::digest() const noexcept {
  return Sha256::hash(std::span<const std::byte>(buffer_.data(), buffer_.size()));
}

CanonicalDecoder::CanonicalDecoder(std::span<const std::byte> data, CanonicalLimits limits)
    : data_(data), limits_(limits) {
  if (data_.size() > limits_.max_total_bytes) {
    fail(ErrorCode::size_limit, "canonical payload exceeds the configured size limit");
  }
}

void CanonicalDecoder::fail(ErrorCode code, std::string message) {
  if (error_ == ErrorCode::ok) {
    error_ = code;
    error_message_ = std::move(message);
  }
}

void CanonicalDecoder::require(std::size_t bytes, const char* what) {
  if (!ok()) {
    return;
  }
  if (bytes > remaining()) {
    fail(ErrorCode::truncated, std::string("truncated canonical payload while reading ") + what);
  }
}

bool CanonicalDecoder::get_bool() { return get_u8() != 0; }

std::uint8_t CanonicalDecoder::get_u8() {
  require(1, "u8");
  if (!ok()) {
    return 0;
  }
  const auto value = static_cast<std::uint8_t>(data_[position_]);
  ++position_;
  return value;
}

std::uint32_t CanonicalDecoder::get_u32() {
  require(4, "u32");
  if (!ok()) {
    return 0;
  }
  std::uint32_t value = 0;
  for (int i = 0; i < 4; ++i) {
    value |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(data_[position_ + static_cast<std::size_t>(i)]))
             << (8 * i);
  }
  position_ += 4;
  return value;
}

std::uint64_t CanonicalDecoder::get_u64() {
  require(8, "u64");
  if (!ok()) {
    return 0;
  }
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(data_[position_ + static_cast<std::size_t>(i)]))
             << (8 * i);
  }
  position_ += 8;
  return value;
}

std::int64_t CanonicalDecoder::get_i64() { return static_cast<std::int64_t>(get_u64()); }

std::string CanonicalDecoder::get_text() {
  const std::uint32_t length = get_u32();
  if (!ok()) {
    return {};
  }
  if (length > limits_.max_text_bytes) {
    fail(ErrorCode::size_limit, "canonical text field exceeds the configured size limit");
    return {};
  }
  require(length, "text");
  if (!ok()) {
    return {};
  }
  std::string value(reinterpret_cast<const char*>(data_.data() + position_), length);
  position_ += length;
  if (!is_valid_utf8(value)) {
    fail(ErrorCode::malformed_input, "canonical text field is not valid UTF-8");
    return {};
  }
  return value;
}

std::vector<std::byte> CanonicalDecoder::get_blob() {
  const std::uint32_t length = get_u32();
  if (!ok()) {
    return {};
  }
  if (length > limits_.max_blob_bytes) {
    fail(ErrorCode::size_limit, "canonical blob field exceeds the configured size limit");
    return {};
  }
  return get_fixed(length);
}

std::vector<std::byte> CanonicalDecoder::get_fixed(std::size_t size) {
  require(size, "fixed field");
  if (!ok()) {
    return {};
  }
  std::vector<std::byte> value(data_.begin() + static_cast<std::ptrdiff_t>(position_),
                               data_.begin() + static_cast<std::ptrdiff_t>(position_ + size));
  position_ += size;
  return value;
}

void CanonicalDecoder::get_tag(std::string_view expected) {
  const std::string actual = get_text();
  if (!ok()) {
    return;
  }
  if (actual != expected) {
    fail(ErrorCode::malformed_input,
         "canonical payload tag mismatch: expected '" + std::string(expected) + "' but found '" +
             actual + "'");
  }
}

std::uint32_t CanonicalDecoder::get_count(std::size_t minimum_bytes_per_element) {
  const std::uint32_t count = get_u32();
  if (!ok()) {
    return 0;
  }
  if (count > limits_.max_elements) {
    fail(ErrorCode::size_limit, "declared element count exceeds the configured limit");
    return 0;
  }
  const auto required = checked_mul_size(count, minimum_bytes_per_element);
  if (!required.ok()) {
    fail(required.status().code(), required.status().message());
    return 0;
  }
  if (required.value() > remaining()) {
    fail(ErrorCode::truncated, "declared element count cannot fit in the remaining payload");
    return 0;
  }
  return count;
}

}  // namespace cplan
