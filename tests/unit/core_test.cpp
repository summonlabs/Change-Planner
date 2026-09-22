#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "change_planner/core/canonical.hpp"
#include "change_planner/core/checked.hpp"
#include "change_planner/core/digest.hpp"
#include "change_planner/core/json.hpp"
#include "change_planner/core/strong.hpp"
#include "change_planner/core/utf8.hpp"
#include "tests/support/test_framework.hpp"

namespace {

struct DeviceTag {
  static constexpr const char* kind_name() { return "device"; }
};
struct LinkTag {
  static constexpr const char* kind_name() { return "link"; }
};
struct GenerationTag {
  static constexpr const char* kind_name() { return "generation"; }
};

using DeviceId = cplan::NameId<DeviceTag>;
using LinkId = cplan::NameId<LinkTag>;
using Generation = cplan::Scalar<GenerationTag, std::uint64_t>;

std::string bytes_to_string(const std::vector<std::byte>& data) {
  std::string out;
  out.reserve(data.size());
  for (const std::byte value : data) {
    out.push_back(static_cast<char>(value));
  }
  return out;
}

std::vector<std::byte> string_to_bytes(std::string_view text) {
  std::vector<std::byte> out;
  out.reserve(text.size());
  for (const char character : text) {
    out.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
  }
  return out;
}

}  // namespace

CPLAN_TEST(status, renders_code_and_message) {
  const cplan::Status ok = cplan::Status::success();
  CPLAN_CHECK(ok.ok());
  CPLAN_CHECK_EQ(ok.to_string(), std::string("ok"));

  const cplan::Status failure = cplan::Status::error(cplan::ErrorCode::stale_generation, "generation gap");
  CPLAN_CHECK(!failure.ok());
  CPLAN_CHECK_EQ(failure.code(), cplan::ErrorCode::stale_generation);
  CPLAN_CHECK_EQ(failure.to_string(), std::string("stale_generation: generation gap"));
  CPLAN_CHECK_EQ(std::string(cplan::to_string(cplan::ErrorCode::integrity_failure)),
                 std::string("integrity_failure"));
}

CPLAN_TEST(status, result_carries_value_or_status) {
  cplan::Result<int> value(41);
  CPLAN_CHECK(value.ok());
  CPLAN_CHECK_EQ(value.value(), 41);
  CPLAN_CHECK(value.status().ok());

  cplan::Result<int> failure = cplan::Status::error(cplan::ErrorCode::not_found, "missing");
  CPLAN_CHECK(!failure.ok());
  CPLAN_CHECK_EQ(failure.status().code(), cplan::ErrorCode::not_found);
  CPLAN_CHECK_EQ(failure.value_or(7), 7);

  cplan::Result<void> void_ok;
  CPLAN_CHECK(void_ok.ok());
  cplan::Result<void> void_failure = cplan::Status::error(cplan::ErrorCode::cancelled, "stopped");
  CPLAN_CHECK(!void_failure.ok());
  CPLAN_CHECK_EQ(void_failure.status().code(), cplan::ErrorCode::cancelled);
}

CPLAN_TEST(strong_ids, accepts_safe_text_and_rejects_hostile_text) {
  CPLAN_CHECK(cplan::is_valid_identity_text("device-a"));
  CPLAN_CHECK(cplan::is_valid_identity_text("A0._:-/@"));
  CPLAN_CHECK(cplan::is_valid_identity_text(std::string(128, 'x')));
  CPLAN_CHECK(!cplan::is_valid_identity_text(""));
  CPLAN_CHECK(!cplan::is_valid_identity_text(std::string(129, 'x')));
  CPLAN_CHECK(!cplan::is_valid_identity_text("has space"));
  CPLAN_CHECK(!cplan::is_valid_identity_text("quote\"inside"));
  CPLAN_CHECK(!cplan::is_valid_identity_text("back\\slash"));
  CPLAN_CHECK(!cplan::is_valid_identity_text("tab\there"));
  CPLAN_CHECK(!cplan::is_valid_identity_text("new\nline"));
  CPLAN_CHECK(!cplan::is_valid_identity_text("semi;colon"));
  CPLAN_CHECK(!cplan::is_valid_identity_text("dollar$sign"));
  CPLAN_CHECK(!cplan::is_valid_identity_text("per%cent"));
  CPLAN_CHECK(!cplan::is_valid_identity_text("caf\xC3\xA9"));
}

CPLAN_TEST(strong_ids, parse_and_compare_within_kind) {
  const auto parsed = DeviceId::parse("device-a");
  CPLAN_REQUIRE_OK(parsed);
  CPLAN_CHECK_EQ(parsed.value().str(), std::string("device-a"));
  CPLAN_CHECK_EQ(parsed.value().to_string(), std::string("device-a"));

  const auto other = DeviceId::parse("device-b");
  CPLAN_REQUIRE_OK(other);
  CPLAN_CHECK(parsed.value() < other.value());
  CPLAN_CHECK(parsed.value() != other.value());

  const auto too_long = DeviceId::parse(std::string(200, 'a'));
  CPLAN_CHECK_ERROR(too_long, cplan::ErrorCode::invalid_argument);

  const LinkId link = LinkId::from_validated("link-a");
  CPLAN_CHECK_EQ(link.str(), std::string("link-a"));
  CPLAN_CHECK(link.hash() == LinkId::from_validated("link-a").hash());
}

CPLAN_TEST(strong_scalars, ordering_and_checked_increment) {
  const Generation zero;
  const Generation one(1);
  CPLAN_CHECK(zero < one);
  CPLAN_CHECK_EQ(zero.value(), 0u);

  auto next = cplan::checked_increment(one);
  CPLAN_REQUIRE_OK(next);
  CPLAN_CHECK_EQ(next.value().value(), 2u);

  const Generation maximum(std::numeric_limits<std::uint64_t>::max());
  auto exhausted = cplan::checked_increment(maximum);
  CPLAN_CHECK_ERROR(exhausted, cplan::ErrorCode::limit_exceeded);
}

CPLAN_TEST(checked_math, detects_overflow) {
  CPLAN_CHECK_EQ(cplan::checked_add_u64(2, 3).value(), 5u);
  CPLAN_CHECK_ERROR(cplan::checked_add_u64(std::numeric_limits<std::uint64_t>::max(), 1),
                    cplan::ErrorCode::size_limit);
  CPLAN_CHECK_ERROR(cplan::checked_sub_u64(1, 2), cplan::ErrorCode::size_limit);
  CPLAN_CHECK_EQ(cplan::checked_sub_u64(5, 2).value(), 3u);
  CPLAN_CHECK_ERROR(cplan::checked_mul_u64(std::numeric_limits<std::uint64_t>::max(), 2),
                    cplan::ErrorCode::size_limit);
  CPLAN_CHECK_EQ(cplan::checked_mul_u64(0, std::numeric_limits<std::uint64_t>::max()).value(), 0u);
  CPLAN_CHECK_EQ(cplan::checked_factorial(0).value(), 1u);
  CPLAN_CHECK_EQ(cplan::checked_factorial(5).value(), 120u);
  CPLAN_CHECK_ERROR(cplan::checked_factorial(21), cplan::ErrorCode::size_limit);
  CPLAN_CHECK_EQ(cplan::checked_size_from_u64(9).value(), static_cast<std::size_t>(9));
}

CPLAN_TEST(digest, matches_nist_vectors) {
  CPLAN_CHECK_EQ(cplan::Sha256::hash("").hex(),
                 std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  CPLAN_CHECK_EQ(cplan::Sha256::hash("abc").hex(),
                 std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  CPLAN_CHECK_EQ(
      cplan::Sha256::hash("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq").hex(),
      std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
  CPLAN_CHECK_EQ(
      cplan::Sha256::hash(
          "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu")
          .hex(),
      std::string("cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1"));

  std::string million_a(1000000, 'a');
  CPLAN_CHECK_EQ(cplan::Sha256::hash(million_a).hex(),
                 std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

CPLAN_TEST(digest, streaming_equals_oneshot_across_block_boundaries) {
  for (std::size_t length = 0; length <= 200; ++length) {
    std::string text;
    for (std::size_t i = 0; i < length; ++i) {
      text.push_back(static_cast<char>('a' + (i % 26)));
    }
    const cplan::Digest expected = cplan::Sha256::hash(text);
    for (std::size_t chunk = 1; chunk <= 17; chunk += 4) {
      cplan::Sha256 hasher;
      std::size_t offset = 0;
      while (offset < text.size()) {
        const std::size_t amount = std::min(chunk, text.size() - offset);
        hasher.update(text.data() + offset, amount);
        offset += amount;
      }
      CPLAN_CHECK_EQ(hasher.finalize(), expected);
    }
  }
}

CPLAN_TEST(digest, hex_round_trip_and_validation) {
  const cplan::Digest digest = cplan::Sha256::hash("payload");
  const auto parsed = cplan::Digest::from_hex(digest.hex());
  CPLAN_REQUIRE_OK(parsed);
  CPLAN_CHECK_EQ(parsed.value(), digest);
  CPLAN_CHECK_EQ(parsed.value().short_hex(), digest.hex().substr(0, 16));
  CPLAN_CHECK(!digest.is_zero());
  CPLAN_CHECK(cplan::Digest{}.is_zero());

  CPLAN_CHECK_ERROR(cplan::Digest::from_hex("00"), cplan::ErrorCode::malformed_input);
  CPLAN_CHECK_ERROR(cplan::Digest::from_hex(std::string(64, 'z')), cplan::ErrorCode::malformed_input);
}

CPLAN_TEST(canonical_codec, round_trips_scalars_text_and_tags) {
  cplan::CanonicalEncoder encoder;
  encoder.put_tag("cplan.test.v1");
  encoder.put_bool(true);
  encoder.put_bool(false);
  encoder.put_u8(0x7Fu);
  encoder.put_u32(0xDEADBEEFu);
  encoder.put_u64(0x0123456789ABCDEFull);
  encoder.put_i64(-42);
  encoder.put_text("hello world");
  encoder.put_blob(string_to_bytes(std::string("blob")));
  encoder.put_fixed(string_to_bytes(std::string("fixed")));

  const std::vector<std::byte> payload = encoder.data();
  cplan::CanonicalDecoder decoder(payload);
  decoder.get_tag("cplan.test.v1");
  CPLAN_REQUIRE_OK(decoder.ok() ? cplan::Status::success()
                                : cplan::Status::error(decoder.error(), decoder.error_message()));
  CPLAN_CHECK_EQ(decoder.get_bool(), true);
  CPLAN_CHECK_EQ(decoder.get_bool(), false);
  CPLAN_CHECK_EQ(decoder.get_u8(), static_cast<std::uint8_t>(0x7Fu));
  CPLAN_CHECK_EQ(decoder.get_u32(), 0xDEADBEEFu);
  CPLAN_CHECK_EQ(decoder.get_u64(), 0x0123456789ABCDEFull);
  CPLAN_CHECK_EQ(decoder.get_i64(), static_cast<std::int64_t>(-42));
  CPLAN_CHECK_EQ(decoder.get_text(), std::string("hello world"));
  CPLAN_CHECK_EQ(bytes_to_string(decoder.get_blob()), std::string("blob"));
  CPLAN_CHECK_EQ(bytes_to_string(decoder.get_fixed(5)), std::string("fixed"));
  CPLAN_CHECK(decoder.ok());
  CPLAN_CHECK(decoder.at_end());
  CPLAN_CHECK_EQ(encoder.digest(), encoder.digest());
}

CPLAN_TEST(canonical_codec, rejects_corrupt_and_truncated_payloads) {
  cplan::CanonicalEncoder encoder;
  encoder.put_text("0123456789");
  const std::vector<std::byte> payload = encoder.data();

  cplan::CanonicalDecoder truncated(std::span<const std::byte>(payload.data(), 3));
  static_cast<void>(truncated.get_text());
  CPLAN_CHECK(!truncated.ok());
  CPLAN_CHECK_EQ(truncated.error(), cplan::ErrorCode::truncated);

  cplan::CanonicalDecoder tag_mismatch(payload);
  tag_mismatch.get_tag("cplan.other.v1");
  CPLAN_CHECK_EQ(tag_mismatch.error(), cplan::ErrorCode::malformed_input);

  std::vector<std::byte> huge = string_to_bytes(std::string(4, '\0'));
  huge[0] = std::byte{0xFF};
  huge[1] = std::byte{0xFF};
  huge[2] = std::byte{0xFF};
  huge[3] = std::byte{0xFF};
  cplan::CanonicalDecoder oversized(huge);
  static_cast<void>(oversized.get_text());
  CPLAN_CHECK(!oversized.ok());
  CPLAN_CHECK(oversized.error() == cplan::ErrorCode::size_limit ||
              oversized.error() == cplan::ErrorCode::truncated);

  CPLAN_CHECK_EQ(cplan::CanonicalDecoder(string_to_bytes(std::string("x"))).get_count(4), 0u);
}

CPLAN_TEST(canonical_codec, typed_helpers_round_trip) {
  cplan::CanonicalEncoder encoder;
  cplan::encode_value(encoder, DeviceId::from_validated("device-7"));
  cplan::encode_value(encoder, Generation(9));
  cplan::encode_value(encoder, cplan::Sha256::hash("bound"));
  cplan::encode_value(encoder, std::string("policy-a"));

  cplan::CanonicalDecoder decoder(encoder.data());
  CPLAN_CHECK_EQ(cplan::decode_value<DeviceId>(decoder).str(), std::string("device-7"));
  CPLAN_CHECK_EQ(cplan::decode_value<Generation>(decoder).value(), 9u);
  CPLAN_CHECK_EQ(cplan::decode_value<cplan::Digest>(decoder), cplan::Sha256::hash("bound"));
  CPLAN_CHECK_EQ(cplan::decode_value<std::string>(decoder), std::string("policy-a"));
  CPLAN_CHECK(decoder.ok());

  cplan::CanonicalEncoder sequence_encoder;
  std::vector<DeviceId> devices{DeviceId::from_validated("a"), DeviceId::from_validated("b")};
  cplan::encode_sequence(sequence_encoder, devices);
  cplan::CanonicalDecoder sequence_decoder(sequence_encoder.data());
  const std::vector<DeviceId> decoded = cplan::decode_sequence<DeviceId>(sequence_decoder, 4);
  CPLAN_REQUIRE_OK(sequence_decoder.ok() ? cplan::Status::success()
                                         : cplan::Status::error(sequence_decoder.error(),
                                                                sequence_decoder.error_message()));
  CPLAN_CHECK_EQ(decoded.size(), static_cast<std::size_t>(2));
  CPLAN_CHECK_EQ(decoded[0].str(), std::string("a"));
  CPLAN_CHECK_EQ(decoded[1].str(), std::string("b"));

  // Identity text that survives the byte-level codec but fails domain validation
  // must be rejected on decode.
  cplan::CanonicalEncoder hostile;
  hostile.put_text("bad identity");
  cplan::CanonicalDecoder hostile_decoder(hostile.data());
  static_cast<void>(cplan::decode_value<DeviceId>(hostile_decoder));
  CPLAN_CHECK(!hostile_decoder.ok());
  CPLAN_CHECK_EQ(hostile_decoder.error(), cplan::ErrorCode::malformed_input);
}

CPLAN_TEST(json_parse, reads_canonical_documents) {
  const auto parsed = cplan::parse_json(
      R"({"b": [1, 2, {"nested": true}], "a": null, "c": "text", "d": -9223372036854775808, "e": 18446744073709551615, "f": 1.5e2})");
  CPLAN_REQUIRE_OK(parsed);
  CPLAN_CHECK(parsed.value().is_object());
  CPLAN_CHECK_EQ(parsed.value().size(), static_cast<std::size_t>(6));

  // Members are stored in canonical (sorted) order.
  const auto& members = parsed.value().members();
  CPLAN_CHECK_EQ(members[0].first, std::string("a"));
  CPLAN_CHECK_EQ(members[6 - 1].first, std::string("f"));

  const cplan::JsonValue* array = parsed.value().find("b");
  CPLAN_REQUIRE(array != nullptr);
  CPLAN_CHECK(array->is_array());
  CPLAN_CHECK_EQ(array->size(), static_cast<std::size_t>(3));
  CPLAN_CHECK_EQ(array->at(1).as_int64(), 2);
  CPLAN_CHECK(array->at(2).find("nested")->as_bool());

  CPLAN_CHECK(parsed.value().find("a")->is_null());
  CPLAN_CHECK_EQ(parsed.value().find("c")->as_string(), std::string("text"));
  CPLAN_CHECK_EQ(parsed.value().find("d")->as_int64(), std::numeric_limits<std::int64_t>::min());
  CPLAN_CHECK_EQ(parsed.value().find("e")->as_uint64(), std::numeric_limits<std::uint64_t>::max());
  CPLAN_CHECK_EQ(parsed.value().find("f")->as_double(), 150.0);

  // Round trip is byte-stable.
  const std::string written = cplan::write_json(parsed.value());
  const auto reparsed = cplan::parse_json(written);
  CPLAN_REQUIRE_OK(reparsed);
  CPLAN_CHECK_EQ(cplan::write_json(reparsed.value()), written);
}

CPLAN_TEST(json_parse, handles_escapes_and_unicode) {
  const auto parsed = cplan::parse_json(
      R"json({"esc": "a\nb\tc \"q\" back\\slash \/slash \u00e9 \ud83d\ude00"})json");
  CPLAN_REQUIRE_OK(parsed);
  const std::string& text = parsed.value().find("esc")->as_string();
  CPLAN_CHECK_EQ(text.find("a\nb"), static_cast<std::size_t>(0));
  CPLAN_CHECK(text.find("b\tc") != std::string::npos);
  CPLAN_CHECK(text.find("\"q\"") != std::string::npos);
  CPLAN_CHECK(text.find("back\\slash") != std::string::npos);
  CPLAN_CHECK(text.find("/slash") != std::string::npos);
  CPLAN_CHECK(text.find("\xC3\xA9") != std::string::npos);
  CPLAN_CHECK(text.find("\xF0\x9F\x98\x80") != std::string::npos);
}

CPLAN_TEST(json_parse, rejects_malformed_documents) {
  const char* cases[] = {
      "",
      "   ",
      "{",
      "}",
      "[1,",
      "{\"a\":1,}",
      "{\"a\" 1}",
      "{a:1}",
      "{\"a\":1}{\"b\":2}",
      "{\"a\":1} trailing",
      "{\"a\":1,\"a\":2}",
      "01",
      "1.",
      ".5",
      "+1",
      "1e",
      "-",
      "tru",
      "nulll",
      "\"unterminated",
      "\"bad escape \\q\"",
      "\"raw control \x01\"",
      "[1 2]",
      "18446744073709551616",
      "-9223372036854775809",
      "1e999",
  };
  for (const char* text : cases) {
    const auto parsed = cplan::parse_json(text);
    CPLAN_CHECK(!parsed.ok());
    if (!parsed.ok()) {
      CPLAN_CHECK_EQ(parsed.status().code(), cplan::ErrorCode::malformed_input);
    }
  }
}

CPLAN_TEST(json_parse, enforces_limits) {
  cplan::JsonLimits limits;
  limits.max_depth = 8;
  limits.max_bytes = 64;
  limits.max_string_bytes = 8;
  limits.max_elements = 4;

  const auto too_deep = cplan::parse_json("[[[[[[[[[[1]]]]]]]]]]", limits);
  CPLAN_CHECK_ERROR(too_deep, cplan::ErrorCode::malformed_input);

  const auto too_large = cplan::parse_json(std::string(200, ' '), limits);
  CPLAN_CHECK_ERROR(too_large, cplan::ErrorCode::size_limit);

  const auto long_string = cplan::parse_json("\"0123456789\"", limits);
  CPLAN_CHECK_ERROR(long_string, cplan::ErrorCode::malformed_input);

  const auto many_elements = cplan::parse_json("[1,2,3,4,5]", limits);
  CPLAN_CHECK_ERROR(many_elements, cplan::ErrorCode::malformed_input);

  const auto fits = cplan::parse_json("[1,2,3]", limits);
  CPLAN_REQUIRE_OK(fits);
  CPLAN_CHECK_EQ(fits.value().size(), static_cast<std::size_t>(3));
}

CPLAN_TEST(json_write, canonical_and_pretty_forms) {
  cplan::JsonValue::object_type members;
  members.emplace_back("z", cplan::JsonValue::make_int(-1));
  members.emplace_back("a", cplan::JsonValue::make_array({cplan::JsonValue::make_bool(true),
                                                          cplan::JsonValue::make_string("x")}));
  const cplan::JsonValue value = cplan::JsonValue::make_object(std::move(members));
  CPLAN_CHECK_EQ(cplan::write_json(value), std::string(R"({"a":[true,"x"],"z":-1})"));

  const std::string pretty = cplan::write_json(value, cplan::JsonStyle::pretty);
  CPLAN_CHECK(pretty.find("\n") != std::string::npos);
  CPLAN_CHECK_EQ(cplan::write_json(cplan::JsonValue::make_object({}), cplan::JsonStyle::pretty),
                 std::string("{}\n"));

  // Duplicate keys supplied programmatically keep the last value.
  cplan::JsonValue::object_type duplicates;
  duplicates.emplace_back("k", cplan::JsonValue::make_int(1));
  duplicates.emplace_back("k", cplan::JsonValue::make_int(2));
  CPLAN_CHECK_EQ(cplan::write_json(cplan::JsonValue::make_object(std::move(duplicates))),
                 std::string(R"({"k":2})"));
}

CPLAN_TEST(utf8, validates_and_encodes) {
  CPLAN_CHECK(cplan::is_valid_utf8("plain ascii"));
  CPLAN_CHECK(cplan::is_valid_utf8("\xC3\xA9"));
  CPLAN_CHECK(cplan::is_valid_utf8("\xF0\x9F\x98\x80"));
  CPLAN_CHECK(!cplan::is_valid_utf8("\xC3\x28"));
  CPLAN_CHECK(!cplan::is_valid_utf8("\xC0\x80"));
  CPLAN_CHECK(!cplan::is_valid_utf8("\xED\xA0\x80"));
  CPLAN_CHECK(!cplan::is_valid_utf8("\xF5\x80\x80\x80"));
  CPLAN_CHECK(!cplan::is_valid_utf8("\xE2\x82"));

  std::string encoded;
  CPLAN_CHECK(cplan::append_utf8(encoded, 0x1F600));
  CPLAN_CHECK_EQ(encoded, std::string("\xF0\x9F\x98\x80"));
  CPLAN_CHECK(!cplan::append_utf8(encoded, 0xD800));
  CPLAN_CHECK(!cplan::append_utf8(encoded, 0x110000));
}
