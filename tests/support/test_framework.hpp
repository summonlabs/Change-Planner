#pragma once

#include <exception>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

#include "change_planner/core/status.hpp"

namespace cplan_test {

// Uniform access to a Status whether the caller produced a Status or a
// Result<T>, so the assertion macros accept both.
inline const cplan::Status& to_status(const cplan::Status& status) { return status; }

template <class T>
const cplan::Status& to_status(const cplan::Result<T>& result) {
  return result.status();
}

// Thrown by CPLAN_REQUIRE to abort the current test case without aborting the
// process: a failing requirement is a diagnosed defect, not a crash.
class TestAbort final : public std::exception {
 public:
  const char* what() const noexcept override { return "test case aborted"; }
};

struct TestCase {
  std::string suite;
  std::string name;
  void (*body)();
};

std::vector<TestCase>& registry();
bool register_case(const char* suite, const char* name, void (*body)());

// Positional arguments passed to the test binary (for example the path of the
// CLI executable under test).
const std::vector<std::string>& positional_arguments();

void report_failure(const char* file, int line, const std::string& message);
void report_note(const std::string& message);

// Runs every registered case (honouring --filter=<substring> and --list) and
// returns the process exit code.
int run_all(int argc, char** argv);

struct Registrar {
  Registrar(const char* suite, const char* name, void (*body)()) {
    static_cast<void>(register_case(suite, name, body));
  }
};

template <class T>
std::string stringify(const T& value) {
  if constexpr (requires(std::ostream& stream, const T& candidate) { stream << candidate; }) {
    std::ostringstream out;
    out << value;
    return out.str();
  } else {
    return "<unprintable>";
  }
}

}  // namespace cplan_test

#define CPLAN_TEST(suite_name, case_name)                                            \
  static void cplan_test_body_##suite_name##_##case_name();                           \
  static const ::cplan_test::Registrar cplan_test_registrar_##suite_name##_##case_name( \
      #suite_name, #case_name, &cplan_test_body_##suite_name##_##case_name);          \
  static void cplan_test_body_##suite_name##_##case_name()

#define CPLAN_CHECK(condition)                                                       \
  do {                                                                               \
    if (!(condition)) {                                                              \
      ::cplan_test::report_failure(__FILE__, __LINE__, "check failed: " #condition); \
    }                                                                                \
  } while (false)

#define CPLAN_REQUIRE(condition)                                                          \
  do {                                                                                    \
    if (!(condition)) {                                                                   \
      ::cplan_test::report_failure(__FILE__, __LINE__, "requirement failed: " #condition); \
      throw ::cplan_test::TestAbort();                                                    \
    }                                                                                     \
  } while (false)

#define CPLAN_CHECK_EQ(lhs, rhs)                                                             \
  do {                                                                                       \
    const auto cplan_lhs_ = (lhs);                                                          \
    const auto cplan_rhs_ = (rhs);                                                          \
    if (!(cplan_lhs_ == cplan_rhs_)) {                                                       \
      ::cplan_test::report_failure(__FILE__, __LINE__,                                       \
                                   std::string("expected ") + #lhs + " == " + #rhs + " but " + \
                                       ::cplan_test::stringify(cplan_lhs_) + " != " +        \
                                       ::cplan_test::stringify(cplan_rhs_));                 \
    }                                                                                        \
  } while (false)

#define CPLAN_REQUIRE_EQ(lhs, rhs)                                                           \
  do {                                                                                       \
    const auto cplan_lhs_ = (lhs);                                                          \
    const auto cplan_rhs_ = (rhs);                                                          \
    if (!(cplan_lhs_ == cplan_rhs_)) {                                                       \
      ::cplan_test::report_failure(__FILE__, __LINE__,                                       \
                                   std::string("expected ") + #lhs + " == " + #rhs + " but " + \
                                       ::cplan_test::stringify(cplan_lhs_) + " != " +        \
                                       ::cplan_test::stringify(cplan_rhs_));                 \
      throw ::cplan_test::TestAbort();                                                       \
    }                                                                                        \
  } while (false)

#define CPLAN_CHECK_LT(lhs, rhs)                                                                 \
  do {                                                                                           \
    const auto cplan_lhs_ = (lhs);                                                              \
    const auto cplan_rhs_ = (rhs);                                                              \
    if (!(cplan_lhs_ < cplan_rhs_)) {                                                            \
      ::cplan_test::report_failure(__FILE__, __LINE__,                                           \
                                   std::string("expected ") + #lhs + " < " + #rhs + " but " +     \
                                       ::cplan_test::stringify(cplan_lhs_) + " >= " +            \
                                       ::cplan_test::stringify(cplan_rhs_));                     \
    }                                                                                            \
  } while (false)

#define CPLAN_CHECK_GT(lhs, rhs)                                                              \
  do {                                                                                        \
    const auto cplan_lhs_ = (lhs);                                                            \
    const auto cplan_rhs_ = (rhs);                                                            \
    if (!(cplan_lhs_ > cplan_rhs_)) {                                                         \
      ::cplan_test::report_failure(__FILE__, __LINE__,                                        \
                                   std::string("expected ") + #lhs + " > " + #rhs + " but " +  \
                                       ::cplan_test::stringify(cplan_lhs_) + " <= " +         \
                                       ::cplan_test::stringify(cplan_rhs_));                  \
    }                                                                                         \
  } while (false)

#define CPLAN_CHECK_GE(lhs, rhs)                                                              \
  do {                                                                                        \
    const auto cplan_lhs_ = (lhs);                                                            \
    const auto cplan_rhs_ = (rhs);                                                            \
    if (!(cplan_lhs_ >= cplan_rhs_)) {                                                        \
      ::cplan_test::report_failure(__FILE__, __LINE__,                                        \
                                   std::string("expected ") + #lhs + " >= " + #rhs + " but " + \
                                       ::cplan_test::stringify(cplan_lhs_) + " < " +          \
                                       ::cplan_test::stringify(cplan_rhs_));                  \
    }                                                                                         \
  } while (false)

#define CPLAN_CHECK_LE(lhs, rhs)                                                              \
  do {                                                                                        \
    const auto cplan_lhs_ = (lhs);                                                            \
    const auto cplan_rhs_ = (rhs);                                                            \
    if (!(cplan_lhs_ <= cplan_rhs_)) {                                                        \
      ::cplan_test::report_failure(__FILE__, __LINE__,                                        \
                                   std::string("expected ") + #lhs + " <= " + #rhs + " but " + \
                                       ::cplan_test::stringify(cplan_lhs_) + " > " +          \
                                       ::cplan_test::stringify(cplan_rhs_));                  \
    }                                                                                         \
  } while (false)

#define CPLAN_CHECK_OK(expression)                                                              \
  do {                                                                                          \
    const auto& cplan_result_ = (expression);                                                   \
    if (!cplan_result_.ok()) {                                                                  \
      ::cplan_test::report_failure(__FILE__, __LINE__,                                          \
                                   std::string("expected success from " #expression " but got ") + \
                                       ::cplan_test::to_status(cplan_result_).to_string());     \
    }                                                                                           \
  } while (false)

#define CPLAN_REQUIRE_OK(expression)                                                            \
  do {                                                                                          \
    const auto& cplan_result_ = (expression);                                                   \
    if (!cplan_result_.ok()) {                                                                  \
      ::cplan_test::report_failure(__FILE__, __LINE__,                                          \
                                   std::string("expected success from " #expression " but got ") + \
                                       ::cplan_test::to_status(cplan_result_).to_string());     \
      throw ::cplan_test::TestAbort();                                                          \
    }                                                                                           \
  } while (false)

#define CPLAN_CHECK_ERROR(expression, expected_code)                                          \
  do {                                                                                        \
    const auto& cplan_result_ = (expression);                                                 \
    if (cplan_result_.ok()) {                                                                 \
      ::cplan_test::report_failure(__FILE__, __LINE__,                                        \
                                   std::string("expected failure from " #expression));        \
    } else if (::cplan_test::to_status(cplan_result_).code() != (expected_code)) {            \
      ::cplan_test::report_failure(                                                           \
          __FILE__, __LINE__,                                                                 \
          std::string("expected error code ") + ::cplan::to_string(expected_code) +           \
              " from " #expression " but got " +                                              \
              ::cplan_test::to_status(cplan_result_).to_string());                            \
    }                                                                                         \
  } while (false)
