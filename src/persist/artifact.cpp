#include "change_planner/persist/artifact.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <thread>

#if defined(_MSC_VER)
#include <process.h>
#else
#include <unistd.h>
#endif

#include "change_planner/core/checked.hpp"
#include "change_planner/version.hpp"

namespace cplan {
namespace {

std::array<std::byte, 16> artifact_magic() {
  std::array<std::byte, 16> magic{};
  const std::string_view name(kArtifactFormatName);
  const std::size_t limit = std::min(name.size(), magic.size());
  for (std::size_t i = 0; i < limit; ++i) {
    magic[i] = static_cast<std::byte>(static_cast<unsigned char>(name[i]));
  }
  return magic;
}

void put_u32_le(std::vector<std::byte>& out, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    out.push_back(static_cast<std::byte>((value >> shift) & 0xFFu));
  }
}

void put_u64_le(std::vector<std::byte>& out, std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    out.push_back(static_cast<std::byte>((value >> shift) & 0xFFu));
  }
}

std::uint32_t read_u32_le(const std::byte* data) {
  std::uint32_t value = 0;
  for (int i = 0; i < 4; ++i) {
    value |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[i])) << (8 * i);
  }
  return value;
}

std::uint64_t read_u64_le(const std::byte* data) {
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(data[i])) << (8 * i);
  }
  return value;
}

Digest header_digest(std::span<const std::byte> header_without_digest,
                     std::span<const std::byte> payload) {
  Sha256 hasher;
  hasher.update(header_without_digest.data(), header_without_digest.size());
  hasher.update(payload.data(), payload.size());
  return hasher.finalize();
}

std::atomic<std::uint64_t> g_partial_counter{0};

// The temporary file name must be unique across processes as well as within one
// process: concurrent publishers of the same destination would otherwise collide
// on the same temporary file and race each other's rename.
std::uint64_t current_process_id() {
#if defined(_MSC_VER)
  return static_cast<std::uint64_t>(::_getpid());
#else
  return static_cast<std::uint64_t>(::getpid());
#endif
}

// Renaming over a destination that another process is momentarily reading or
// replacing can fail with a transient sharing violation on Windows. Publication
// retries a bounded number of times before reporting failure, so a concurrent
// publisher is not a spurious IO error.
std::error_code rename_with_retry(const std::string& from, const std::string& to) {
  constexpr int kAttempts = 25;
  std::error_code error;
  for (int attempt = 0; attempt < kAttempts; ++attempt) {
    std::filesystem::rename(from, to, error);
    if (!error) {
      return error;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2 * (attempt + 1)));
  }
  return error;
}

std::string partial_path_for(const std::string& path) {
  const std::uint64_t sequence = g_partial_counter.fetch_add(1, std::memory_order_relaxed);
  return path + ".partial-" + std::to_string(current_process_id()) + "-" +
         std::to_string(sequence);
}

}  // namespace

std::vector<std::byte> serialize_plan_artifact(const Plan& plan, std::uint64_t* payload_bytes) {
  CanonicalEncoder encoder;
  plan.encode(encoder);
  const std::vector<std::byte>& payload = encoder.data();

  std::vector<std::byte> out;
  out.reserve(kArtifactHeaderBytes + payload.size());
  const std::array<std::byte, 16> magic = artifact_magic();
  out.insert(out.end(), magic.begin(), magic.end());
  put_u32_le(out, kArtifactFormatVersion);
  put_u32_le(out, kPlanSchemaVersion);
  put_u64_le(out, static_cast<std::uint64_t>(payload.size()));
  const std::span<const std::byte> header_prefix(out.data(), out.size());
  const Digest digest = header_digest(header_prefix, payload);
  for (std::size_t i = 0; i < Digest::kSize; ++i) {
    out.push_back(static_cast<std::byte>(digest.data()[i]));
  }
  out.insert(out.end(), payload.begin(), payload.end());
  if (payload_bytes != nullptr) {
    *payload_bytes = static_cast<std::uint64_t>(payload.size());
  }
  return out;
}

Digest artifact_envelope_digest(std::span<const std::byte> bytes) {
  return Sha256::hash(bytes);
}

Result<PlanArtifact> parse_plan_artifact(std::span<const std::byte> bytes,
                                         const ArtifactLimits& limits) {
  if (bytes.size() > limits.max_bytes) {
    return Status::error(ErrorCode::size_limit, "artifact exceeds the configured size limit");
  }
  if (bytes.size() < kArtifactHeaderBytes) {
    return Status::error(ErrorCode::truncated,
                         "artifact is shorter than the minimum envelope size");
  }
  const std::array<std::byte, 16> magic = artifact_magic();
  if (!std::equal(magic.begin(), magic.end(), bytes.begin())) {
    return Status::error(ErrorCode::malformed_input, "artifact magic does not match " +
                                                        std::string(kArtifactFormatName));
  }
  const std::uint32_t format_version = read_u32_le(bytes.data() + 16);
  const std::uint32_t schema_version = read_u32_le(bytes.data() + 20);
  const std::uint64_t payload_length = read_u64_le(bytes.data() + 24);
  if (format_version != kArtifactFormatVersion) {
    return Status::error(ErrorCode::unsupported_version,
                         "artifact format version " + std::to_string(format_version) +
                             " is not supported by this build (expects " +
                             std::to_string(kArtifactFormatVersion) + ")");
  }
  if (schema_version != kPlanSchemaVersion) {
    return Status::error(ErrorCode::unsupported_version,
                         "plan schema version " + std::to_string(schema_version) +
                             " is not supported by this build (expects " +
                             std::to_string(kPlanSchemaVersion) + ")");
  }
  const auto payload_size = checked_size_from_u64(payload_length);
  if (!payload_size.ok()) {
    return payload_size.status();
  }
  if (payload_size.value() != bytes.size() - kArtifactHeaderBytes) {
    return Status::error(ErrorCode::truncated,
                         "artifact payload length does not match the file size (declared " +
                             std::to_string(payload_length) + ", available " +
                             std::to_string(bytes.size() - kArtifactHeaderBytes) + ")");
  }
  const std::span<const std::byte> header_prefix(bytes.data(), kArtifactHeaderBytes - Digest::kSize);
  const std::span<const std::byte> stored_digest(bytes.data() + kArtifactHeaderBytes - Digest::kSize,
                                                 Digest::kSize);
  const std::span<const std::byte> payload(bytes.data() + kArtifactHeaderBytes, payload_size.value());
  const Digest computed = header_digest(header_prefix, payload);
  Digest::bytes_type stored_bytes{};
  for (std::size_t i = 0; i < Digest::kSize; ++i) {
    stored_bytes[i] = static_cast<std::uint8_t>(stored_digest[i]);
  }
  if (!(computed == Digest::from_bytes(stored_bytes))) {
    return Status::error(ErrorCode::integrity_failure,
                         "artifact digest mismatch: the payload or header was modified");
  }

  CanonicalDecoder decoder(payload, limits.canonical);
  Plan plan = Plan::decode(decoder);
  if (!decoder.ok()) {
    return Status::error(decoder.error(), decoder.error_message());
  }
  if (!decoder.at_end()) {
    return Status::error(ErrorCode::malformed_input,
                         "artifact payload contains trailing bytes after the plan document");
  }
  const Status structure = plan.verify_structure();
  if (!structure.ok()) {
    return Status::error(structure.code(),
                         "decoded plan is not structurally valid: " + structure.message());
  }
  // Canonical form check: re-encoding must reproduce the payload byte for byte.
  // A payload that decodes but re-encodes differently is a non-canonical (and
  // therefore untrusted) document.
  CanonicalEncoder reencoder;
  plan.encode(reencoder);
  const std::vector<std::byte>& reencoded = reencoder.data();
  if (reencoded.size() != payload.size() ||
      !std::equal(reencoded.begin(), reencoded.end(), payload.begin())) {
    return Status::error(ErrorCode::integrity_failure,
                         "artifact payload is not the canonical encoding of the decoded plan");
  }

  PlanArtifact artifact;
  artifact.plan = std::move(plan);
  artifact.payload_digest = computed;
  artifact.envelope_digest = artifact_envelope_digest(bytes);
  artifact.format_version = format_version;
  artifact.schema_version = schema_version;
  artifact.payload_bytes = payload_length;
  artifact.integrity_verified = true;
  return artifact;
}

Result<void> write_plan_artifact(const std::string& path, const Plan& plan) {
  const Status structure = plan.verify_structure();
  if (!structure.ok()) {
    return Status::error(structure.code(),
                         "refusing to persist an invalid plan: " + structure.message());
  }
  const std::vector<std::byte> bytes = serialize_plan_artifact(plan);
  const std::string partial = partial_path_for(path);
  {
    std::ofstream stream(partial, std::ios::binary | std::ios::trunc);
    if (!stream.is_open()) {
      return Status::error(ErrorCode::not_found, "cannot open '" + partial + "' for writing");
    }
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    stream.flush();
    if (!stream.good()) {
      stream.close();
      std::error_code ignored;
      std::filesystem::remove(partial, ignored);
      return Status::error(ErrorCode::internal_invariant,
                           "failed to write the complete artifact to '" + partial + "'");
    }
  }
  const std::error_code error = rename_with_retry(partial, path);
  if (error) {
    std::error_code ignored;
    std::filesystem::remove(partial, ignored);
    return Status::error(ErrorCode::internal_invariant,
                         "atomic rename of '" + partial + "' to '" + path +
                             "' failed: " + error.message());
  }
  return Status::success();
}

Result<PlanArtifact> read_plan_artifact(const std::string& path, const ArtifactLimits& limits) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error) {
    return Status::error(ErrorCode::not_found, "cannot stat '" + path + "': " + error.message());
  }
  if (size > limits.max_bytes) {
    return Status::error(ErrorCode::size_limit,
                         "artifact '" + path + "' exceeds the configured size limit");
  }
  std::ifstream stream(path, std::ios::binary);
  if (!stream.is_open()) {
    return Status::error(ErrorCode::not_found, "cannot open '" + path + "' for reading");
  }
  std::vector<std::byte> bytes(static_cast<std::size_t>(size));
  if (!bytes.empty()) {
    stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (stream.gcount() != static_cast<std::streamsize>(bytes.size())) {
      return Status::error(ErrorCode::truncated,
                           "artifact '" + path + "' shrank while it was being read");
    }
  }
  auto artifact = parse_plan_artifact(bytes, limits);
  if (!artifact.ok()) {
    return artifact.status();
  }
  artifact.value().source_path = path;
  return artifact;
}

Result<ArtifactScan> scan_plan_directory(const std::string& directory) {
  std::error_code error;
  if (!std::filesystem::is_directory(directory, error)) {
    return Status::error(ErrorCode::not_found, "'" + directory + "' is not a readable directory");
  }
  std::vector<std::string> paths;
  for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
    if (error) {
      return Status::error(ErrorCode::not_found,
                           "cannot enumerate '" + directory + "': " + error.message());
    }
    if (!entry.is_regular_file()) {
      continue;
    }
    paths.push_back(entry.path().string());
  }
  std::sort(paths.begin(), paths.end());

  ArtifactScan scan;
  for (const std::string& path : paths) {
    ArtifactScanEntry result;
    result.path = path;
    const std::string filename = std::filesystem::path(path).filename().string();
    if (filename.find(".partial-") != std::string::npos) {
      // An orphaned partial publication: a crashed or killed writer. It is never
      // interpreted as a plan, and it is never hidden either.
      result.readable = false;
      result.error = "incomplete publication: orphaned partial file (a writer did not finish)";
      ++scan.unreadable_count;
      scan.entries.push_back(std::move(result));
      continue;
    }
    auto artifact = read_plan_artifact(path);
    if (artifact.ok()) {
      result.readable = true;
      result.plan_id = artifact.value().plan.id;
      result.generation = artifact.value().plan.generation;
      result.payload_digest = artifact.value().payload_digest;
      ++scan.readable_count;
    } else {
      result.readable = false;
      result.error = artifact.status().to_string();
      ++scan.unreadable_count;
    }
    scan.entries.push_back(std::move(result));
  }
  return scan;
}

}  // namespace cplan
