#include "tests/support/test_framework.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace cplan_test {
namespace {

int g_current_failures = 0;
std::string g_current_case;
bool g_verbose = false;

}  // namespace

std::vector<TestCase>& registry() {
  static std::vector<TestCase> cases;
  return cases;
}

const std::vector<std::string>& positional_arguments() {
  static std::vector<std::string> arguments;
  return arguments;
}

bool register_case(const char* suite, const char* name, void (*body)()) {
  registry().push_back(TestCase{suite, name, body});
  return true;
}

void report_failure(const char* file, int line, const std::string& message) {
  ++g_current_failures;
  std::printf("    FAIL %s:%d [%s] %s\n", file, line, g_current_case.c_str(), message.c_str());
  std::fflush(stdout);
}

void report_note(const std::string& message) {
  if (g_verbose) {
    std::printf("    note [%s] %s\n", g_current_case.c_str(), message.c_str());
    std::fflush(stdout);
  }
}

int run_all(int argc, char** argv) {
  std::string filter;
  bool list_only = false;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument.rfind("--filter=", 0) == 0) {
      filter = argument.substr(9);
    } else if (argument == "--list") {
      list_only = true;
    } else if (argument == "--verbose") {
      g_verbose = true;
    } else if (argument.rfind("--", 0) == 0) {
      std::printf("unknown argument: %s\n", argument.c_str());
      return 2;
    } else {
      const_cast<std::vector<std::string>&>(positional_arguments()).push_back(argument);
    }
  }

  std::vector<TestCase> selected;
  for (const TestCase& test : registry()) {
    const std::string full = test.suite + "." + test.name;
    if (!filter.empty() && full.find(filter) == std::string::npos) {
      continue;
    }
    selected.push_back(test);
  }

  if (list_only) {
    for (const TestCase& test : selected) {
      std::printf("%s.%s\n", test.suite.c_str(), test.name.c_str());
    }
    return 0;
  }

  std::size_t passed = 0;
  std::size_t failed = 0;
  for (const TestCase& test : selected) {
    g_current_case = test.suite + "." + test.name;
    g_current_failures = 0;
    try {
      test.body();
    } catch (const TestAbort&) {
      // Failure already reported by the failing requirement.
    } catch (const std::exception& error) {
      report_failure("<framework>", 0, std::string("unhandled exception: ") + error.what());
    } catch (...) {
      report_failure("<framework>", 0, "unhandled non-standard exception");
    }
    if (g_current_failures == 0) {
      ++passed;
      std::printf("[ PASS ] %s\n", g_current_case.c_str());
    } else {
      ++failed;
      std::printf("[ FAIL ] %s (%d failing checks)\n", g_current_case.c_str(), g_current_failures);
    }
    std::fflush(stdout);
  }

  std::printf("\n%zu passed, %zu failed, %zu selected\n", passed, failed, selected.size());
  std::fflush(stdout);
  return failed == 0 ? 0 : 1;
}

}  // namespace cplan_test

int main(int argc, char** argv) { return ::cplan_test::run_all(argc, argv); }
