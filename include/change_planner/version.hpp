#pragma once

#include <cstdint>

namespace cplan {

inline constexpr int kVersionMajor = 1;
inline constexpr int kVersionMinor = 0;
inline constexpr int kVersionPatch = 0;
inline constexpr const char* kVersionString = "1.0.0";
inline constexpr const char* kProductName = "Change Planner";
inline constexpr const char* kVendorName = "Summon Software Labs";

// Persisted artifact envelope identity. Bumping kArtifactFormatVersion is a
// breaking change: readers refuse to interpret older envelopes.
inline constexpr const char* kArtifactFormatName = "CPLAN-ARTIFACT";
inline constexpr std::uint32_t kArtifactFormatVersion = 1;

// Canonical payload schema identity for the plan document itself.
inline constexpr const char* kPlanSchemaName = "cplan.plan";
inline constexpr std::uint32_t kPlanSchemaVersion = 1;

// Canonical payload schema identity for the planning request document.
inline constexpr const char* kRequestSchemaName = "cplan.request";
inline constexpr std::uint32_t kRequestSchemaVersion = 1;

}  // namespace cplan
