#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX  // windows.h min/max macros must not leak into the standard library
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "change_planner/cli/io.hpp"
#include "change_planner/core/json.hpp"
#include "change_planner/persist/artifact.hpp"
#include "change_planner/plan/verify.hpp"
#include "tests/support/fixtures.hpp"
#include "tests/support/test_framework.hpp"

using namespace cplan_test;

#ifndef CPLAN_EXAMPLE_DATA_DIR
#define CPLAN_EXAMPLE_DATA_DIR "examples/data"
#endif

namespace {

struct ProcessResult {
  bool started{false};
  int exit_code{-1};
  std::string output;
};

std::string data_path(const std::string& name) {
  return std::string(CPLAN_EXAMPLE_DATA_DIR) + "/" + name;
}

std::string quote(const std::string& value) { return "\"" + value + "\""; }

#ifdef _WIN32

class ChildProcess {
 public:
  bool start(const std::string& executable, const std::vector<std::string>& arguments) {
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;

    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    if (!CreatePipe(&read_end, &write_end, &attributes, 0)) {
      return false;
    }
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

    std::string command = quote(executable);
    for (const std::string& argument : arguments) {
      command += " " + quote(argument);
    }

    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = write_end;
    startup.hStdError = write_end;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION info{};
    std::vector<char> mutable_command(command.begin(), command.end());
    mutable_command.push_back('\0');
    const BOOL created = CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info);
    CloseHandle(write_end);
    if (!created) {
      CloseHandle(read_end);
      return false;
    }
    process_ = info.hProcess;
    thread_ = info.hThread;
    pipe_ = read_end;
    return true;
  }

  bool wait(int* exit_code, std::string* output) {
    if (process_ == nullptr) {
      return false;
    }
    // Drain the pipe before waiting: the child blocks if it fills the buffer.
    char buffer[4096];
    DWORD read = 0;
    while (ReadFile(pipe_, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
      output->append(buffer, read);
    }
    WaitForSingleObject(process_, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(process_, &code);
    CloseHandle(pipe_);
    CloseHandle(thread_);
    CloseHandle(process_);
    pipe_ = nullptr;
    process_ = nullptr;
    *exit_code = static_cast<int>(code);
    return true;
  }

  void kill() {
    if (process_ != nullptr) {
      TerminateProcess(process_, 9);
      WaitForSingleObject(process_, INFINITE);
      CloseHandle(pipe_);
      CloseHandle(thread_);
      CloseHandle(process_);
      pipe_ = nullptr;
      process_ = nullptr;
    }
  }

  ~ChildProcess() { kill(); }

 private:
  HANDLE process_{nullptr};
  HANDLE thread_{nullptr};
  HANDLE pipe_{nullptr};
};

#endif

ProcessResult run_process(const std::string& executable, const std::vector<std::string>& arguments) {
  ProcessResult result;
#ifdef _WIN32
  ChildProcess child;
  if (!child.start(executable, arguments)) {
    return result;
  }
  result.started = true;
  child.wait(&result.exit_code, &result.output);
#else
  static_cast<void>(executable);
  static_cast<void>(arguments);
#endif
  return result;
}

std::string cli_path() {
  const std::vector<std::string>& arguments = positional_arguments();
  return arguments.empty() ? std::string() : arguments[0];
}

std::string helper_path() {
  const std::vector<std::string>& arguments = positional_arguments();
  return arguments.size() < 2 ? std::string() : arguments[1];
}

bool wait_for_file(const std::string& path, std::uint32_t iterations) {
  for (std::uint32_t attempt = 0; attempt < iterations; ++attempt) {
    if (std::filesystem::exists(path)) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

}  // namespace

CPLAN_TEST(process, cli_plans_and_publishes_a_verifiable_artifact) {
  const std::string cli = cli_path();
  CPLAN_REQUIRE(!cli.empty());
  const std::string directory = make_scratch_directory("process-plan");
  const std::string artifact = directory + "/plan.cplan";

  const ProcessResult result =
      run_process(cli, {"plan", "--input", data_path("ladder-request.json"), "--out", artifact,
                        "--verify", "--json"});
  CPLAN_REQUIRE(result.started);
  CPLAN_CHECK_EQ(result.exit_code, 0);

  // The JSON printed by the process is a real document, not a text blob.
  const auto document = cplan::parse_json(result.output);
  CPLAN_REQUIRE_OK(document);
  CPLAN_CHECK(document.value().is_object());
  const cplan::JsonValue* plan = document.value().find("plan");
  CPLAN_REQUIRE(plan != nullptr);
  CPLAN_CHECK(plan->find("planId") != nullptr);
  const cplan::JsonValue* verification = document.value().find("verification");
  CPLAN_REQUIRE(verification != nullptr);
  CPLAN_CHECK(verification->find("ok")->as_bool());

  auto loaded = cplan::read_plan_artifact(artifact);
  CPLAN_REQUIRE_OK(loaded);
  CPLAN_CHECK(loaded.value().integrity_verified);

  // An independent process can inspect and validate what another one published.
  const ProcessResult inspected = run_process(cli, {"inspect", "--plan", artifact, "--json"});
  CPLAN_REQUIRE(inspected.started);
  CPLAN_CHECK_EQ(inspected.exit_code, 0);
  const auto inspected_document = cplan::parse_json(inspected.output);
  CPLAN_REQUIRE_OK(inspected_document);

  const ProcessResult validated =
      run_process(cli, {"validate", "--plan", artifact, "--input",
                        data_path("ladder-request.json"), "--exhaustive"});
  CPLAN_REQUIRE(validated.started);
  CPLAN_CHECK_EQ(validated.exit_code, 0);
  CPLAN_CHECK(validated.output.find("valid") != std::string::npos);
  remove_scratch_directory(directory);
}

CPLAN_TEST(process, cli_reports_refusals_with_a_usable_exit_code) {
  const std::string cli = cli_path();
  CPLAN_REQUIRE(!cli.empty());
  const std::string directory = make_scratch_directory("process-refuse");

  // The same request planned against a stale generation must be refused.
  auto document = cplan::cli::read_json_file(data_path("ladder-request.json"));
  CPLAN_REQUIRE_OK(document);
  cplan::JsonValue::object_type members = document.value().members();
  for (auto& member : members) {
    if (member.first == "target") {
      cplan::JsonValue::object_type target = member.second.members();
      for (auto& field : target) {
        if (field.first == "basedOnGeneration") {
          field.second = cplan::JsonValue::make_uint(999);
        }
      }
      member.second = cplan::JsonValue::make_object(std::move(target));
    }
  }
  const std::string stale_path = directory + "/stale-request.json";
  {
    std::ofstream stream(stale_path, std::ios::binary | std::ios::trunc);
    stream << cplan::write_json(cplan::JsonValue::make_object(std::move(members)),
                                cplan::JsonStyle::pretty);
  }

  const ProcessResult refused = run_process(cli, {"plan", "--input", stale_path});
  CPLAN_REQUIRE(refused.started);
  CPLAN_CHECK_EQ(refused.exit_code, 1);
  CPLAN_CHECK(refused.output.find("stale") != std::string::npos);

  const ProcessResult missing = run_process(cli, {"plan", "--input", directory + "/absent.json"});
  CPLAN_REQUIRE(missing.started);
  CPLAN_CHECK_EQ(missing.exit_code, 4);

  const ProcessResult unusable = run_process(cli, {"explain", "--plan", directory + "/absent"});
  CPLAN_REQUIRE(unusable.started);
  CPLAN_CHECK_EQ(unusable.exit_code, 4);
  remove_scratch_directory(directory);
}

CPLAN_TEST(process, concurrent_writers_publish_atomically) {
  const std::string cli = cli_path();
  CPLAN_REQUIRE(!cli.empty());
  const std::string directory = make_scratch_directory("process-concurrent");
  const std::string artifact = directory + "/shared.cplan";

  constexpr std::size_t kWriters = 6;
  std::vector<ChildProcess> writers(kWriters);
  for (std::size_t index = 0; index < kWriters; ++index) {
    CPLAN_REQUIRE(writers[index].start(
        cli, {"plan", "--input", data_path("ladder-request.json"), "--out", artifact}));
  }
  std::vector<std::string> outputs(kWriters);
  for (std::size_t index = 0; index < kWriters; ++index) {
    int exit_code = -1;
    CPLAN_REQUIRE(writers[index].wait(&exit_code, &outputs[index]));
    CPLAN_CHECK_EQ(exit_code, 0);
  }

  // The destination is always a complete artifact: writers use an atomic rename.
  auto loaded = cplan::read_plan_artifact(artifact);
  CPLAN_REQUIRE_OK(loaded);
  CPLAN_CHECK(loaded.value().integrity_verified);

  const auto scan = cplan::scan_plan_directory(directory);
  CPLAN_REQUIRE_OK(scan);
  CPLAN_CHECK_EQ(scan.value().unreadable_count, static_cast<std::size_t>(0));
  CPLAN_CHECK_EQ(scan.value().readable_count, static_cast<std::size_t>(1));
  remove_scratch_directory(directory);
}

CPLAN_TEST(process, killed_writer_never_leaves_a_half_written_artifact) {
  const std::string helper = helper_path();
  CPLAN_REQUIRE(!helper.empty());
  const std::string directory = make_scratch_directory("process-kill");
  const std::string artifact = directory + "/crashed.cplan";

  ChildProcess child;
  CPLAN_REQUIRE(child.start(helper, {"hold", artifact}));
  CPLAN_REQUIRE(wait_for_file(artifact + ".ready", 2000));
  child.kill();  // real process kill, not a simulated one

  // The destination was never created: a killed writer cannot publish a partial
  // plan.
  CPLAN_CHECK(!std::filesystem::exists(artifact));
  auto missing = cplan::read_plan_artifact(artifact);
  CPLAN_CHECK_ERROR(missing, cplan::ErrorCode::not_found);

  // The orphaned partial file is reported, never interpreted as a plan.
  const auto scan = cplan::scan_plan_directory(directory);
  CPLAN_REQUIRE_OK(scan);
  CPLAN_CHECK_EQ(scan.value().readable_count, static_cast<std::size_t>(0));
  CPLAN_CHECK_GT(scan.value().unreadable_count, 0u);
  bool orphan_reported = false;
  for (const cplan::ArtifactScanEntry& entry : scan.value().entries) {
    if (!entry.readable && entry.error.find("incomplete publication") != std::string::npos) {
      orphan_reported = true;
    }
  }
  CPLAN_CHECK(orphan_reported);

  // A later, healthy publication succeeds in the same directory.
  CPLAN_REQUIRE(!helper.empty());
  const ProcessResult written =
      run_process(helper, {"write", data_path("ladder-request.json"), artifact});
  CPLAN_REQUIRE(written.started);
  CPLAN_CHECK_EQ(written.exit_code, 0);
  auto recovered = cplan::read_plan_artifact(artifact);
  CPLAN_REQUIRE_OK(recovered);
  remove_scratch_directory(directory);
}

CPLAN_TEST(process, torn_writes_are_rejected_by_independent_readers) {
  const std::string cli = cli_path();
  const std::string helper = helper_path();
  CPLAN_REQUIRE(!cli.empty());
  CPLAN_REQUIRE(!helper.empty());
  const std::string directory = make_scratch_directory("process-torn");
  const std::string artifact = directory + "/torn.cplan";

  // A non-atomic writer publishes a truncated document and dies.
  const ProcessResult torn = run_process(helper, {"torn", artifact});
  CPLAN_REQUIRE(torn.started);
  CPLAN_CHECK_EQ(torn.exit_code, 3);

  auto loaded = cplan::read_plan_artifact(artifact);
  CPLAN_CHECK(!loaded.ok());
  const ProcessResult inspected = run_process(cli, {"inspect", "--plan", artifact});
  CPLAN_REQUIRE(inspected.started);
  CPLAN_CHECK_EQ(inspected.exit_code, 4);

  const ProcessResult scanned = run_process(cli, {"scan", "--directory", directory, "--json"});
  CPLAN_REQUIRE(scanned.started);
  CPLAN_CHECK_EQ(scanned.exit_code, 1);
  const auto document = cplan::parse_json(scanned.output);
  CPLAN_REQUIRE_OK(document);
  CPLAN_CHECK_EQ(document.value().find("unreadable")->as_uint64(), 1u);
  remove_scratch_directory(directory);
}
