#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "change_planner/core/canonical.hpp"
#include "change_planner/plan/plan.hpp"
#include "change_planner/plan/refusal.hpp"

namespace cplan {

// Artifact envelope layout (little endian):
//   magic[16] | format_version u32 | schema_version u32 | payload_length u64 |
//   payload_digest[32] | payload
// The digest covers magic, both versions, the payload length and the payload, so
// header tampering is detected as well as payload tampering.
inline constexpr std::size_t kArtifactHeaderBytes = 16 + 4 + 4 + 8 + Digest::kSize;

struct ArtifactLimits {
  std::size_t max_bytes = 32u * 1024u * 1024u;
  CanonicalLimits canonical{};
};

// A persisted plan never becomes fresh because it could be deserialized: the
// reader reports integrity and leaves validity to plan invalidation.
struct PlanArtifact {
  Plan plan;
  Digest payload_digest;
  Digest envelope_digest;
  std::uint32_t format_version{0};
  std::uint32_t schema_version{0};
  std::uint64_t payload_bytes{0};
  std::string source_path;
  bool integrity_verified{false};
};

[[nodiscard]] std::vector<std::byte> serialize_plan_artifact(
    const Plan& plan, std::uint64_t* payload_bytes = nullptr);

[[nodiscard]] Result<PlanArtifact> parse_plan_artifact(std::span<const std::byte> bytes,
                                                       const ArtifactLimits& limits = {});

// Atomic publication: the artifact is written to a sibling temporary file,
// flushed, and renamed over the destination, so a partially written artifact is
// never observable at the destination path.
[[nodiscard]] Result<void> write_plan_artifact(const std::string& path, const Plan& plan);

[[nodiscard]] Result<PlanArtifact> read_plan_artifact(const std::string& path,
                                                      const ArtifactLimits& limits = {});

// Reading a plan never implies accepting it: freshness must be established
// against current authoritative state before execution.
[[nodiscard]] Digest artifact_envelope_digest(std::span<const std::byte> bytes);

struct ArtifactScanEntry {
  std::string path;
  bool readable{false};
  PlanId plan_id;
  PlanGeneration generation;
  Digest payload_digest;
  std::string error;
};

struct ArtifactScan {
  std::vector<ArtifactScanEntry> entries;
  std::size_t readable_count{0};
  std::size_t unreadable_count{0};
};

// Scans a directory for artifacts. Corrupt or unreadable files are reported, not
// skipped: silent acceptance of damaged state is never an option.
[[nodiscard]] Result<ArtifactScan> scan_plan_directory(const std::string& directory);

}  // namespace cplan
