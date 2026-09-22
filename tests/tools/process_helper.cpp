// Process helper for the independent-process tests.
//
// Modes:
//   write <request.json> <artifact>   plan the request and publish the artifact
//   torn <artifact>                   write a truncated artifact to the target path
//   partial <artifact>                write only the sibling partial file, then exit
//   hold <artifact>                   write the sibling partial file, signal
//                                     readiness, then block until killed
//
// Every mode exits with a distinct status so the parent can assert on real
// process outcomes rather than on in-process mocks.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "change_planner/cli/io.hpp"
#include "change_planner/persist/artifact.hpp"
#include "change_planner/plan/generate.hpp"

namespace {

int plan_and_write(const std::string& request_path, const std::string& artifact_path) {
  auto document = cplan::cli::read_json_file(request_path);
  if (!document.ok()) {
    std::fprintf(stderr, "read: %s\n", document.status().to_string().c_str());
    return 10;
  }
  auto request = cplan::cli::parse_plan_request(document.value());
  if (!request.ok()) {
    std::fprintf(stderr, "parse: %s\n", request.status().to_string().c_str());
    return 11;
  }
  const cplan::PlanResult result = cplan::generate_plan(request.value());
  if (!result.ok()) {
    std::fprintf(stderr, "refused: %s\n", result.refusal().explain().c_str());
    return 12;
  }
  const cplan::Status written = cplan::write_plan_artifact(artifact_path, result.plan());
  if (!written.ok()) {
    std::fprintf(stderr, "write: %s\n", written.to_string().c_str());
    return 13;
  }
  std::printf("%s\n", result.plan().content_digest().hex().c_str());
  return 0;
}

std::vector<std::byte> read_all(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  std::vector<std::byte> bytes;
  char buffer[4096];
  while (stream.read(buffer, sizeof(buffer)) || stream.gcount() > 0) {
    const std::streamsize count = stream.gcount();
    for (std::streamsize i = 0; i < count; ++i) {
      bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(buffer[i])));
    }
  }
  return bytes;
}

void write_all(const std::string& path, const std::vector<std::byte>& bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  stream.flush();
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: process_helper <mode> ...\n");
    return 2;
  }
  const std::string mode = argv[1];
  if (mode == "write") {
    if (argc != 4) {
      std::fprintf(stderr, "write requires <request> <artifact>\n");
      return 2;
    }
    return plan_and_write(argv[2], argv[3]);
  }
  if (mode == "torn") {
    if (argc != 3) {
      return 2;
    }
    // A writer that is not atomic publishes a truncated document at the target
    // path. Readers must reject it.
    const std::string path = argv[2];
    std::vector<std::byte> bytes(200, std::byte{0x41});
    write_all(path, bytes);
    return 3;
  }
  if (mode == "partial" || mode == "hold") {
    if (argc != 3) {
      return 2;
    }
    const std::string path = argv[2];
    const std::string partial = path + ".partial-helper";
    std::vector<std::byte> bytes(120, std::byte{0x42});
    write_all(partial, bytes);
    if (mode == "partial") {
      return 4;  // process dies before the rename
    }
    // Signal readiness for the parent, then block until the parent kills us.
    write_all(path + ".ready", {std::byte{'r'}});
    while (true) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }
  std::fprintf(stderr, "unknown mode: %s\n", mode.c_str());
  return 2;
}
