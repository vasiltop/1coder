#pragma once

#include "base/base_string.h"
#include "base/base_types.h"

// Minimal self-registering test harness. No external dependency, and it links
// only editor_core, so the whole editing core is exercised without a window.

struct TestCase {
  const char *name;
  const char *file;
  void (*fn)();
  TestCase *next;
};

void TestRegister(TestCase *test);
void TestFail(const char *file, int line, const char *fmt, ...) PrintfFormat(3, 4);

// Each test body becomes a free function; the registrar object exists only to
// run TestRegister before main.
#define TEST(name)                                                             \
  static void TestFn_##name();                                                 \
  static TestCase TestCase_##name = {#name, __FILE__, TestFn_##name, nullptr}; \
  static struct TestReg_##name {                                               \
    TestReg_##name() { TestRegister(&TestCase_##name); }                       \
  } TestRegInstance_##name;                                                    \
  static void TestFn_##name()

#define CHECK(cond)                                                            \
  Stmt(if (!(cond)) { TestFail(__FILE__, __LINE__, "CHECK failed: %s", #cond); })

#define CHECK_EQ(a, b)                                                         \
  Stmt({                                                                       \
    auto check_a_ = (a);                                                       \
    auto check_b_ = (b);                                                       \
    if (!((decltype(check_b_))check_a_ == check_b_)) {                         \
      TestFail(__FILE__, __LINE__, "CHECK_EQ failed: %s == %s  (%lld vs %lld)",\
               #a, #b, (long long)check_a_, (long long)check_b_);              \
    }                                                                          \
  })

#define CHECK_STR(a, b)                                                        \
  Stmt({                                                                       \
    String8 check_a_ = (a);                                                    \
    String8 check_b_ = (b);                                                    \
    if (!Str8Match(check_a_, check_b_)) {                                      \
      TestFail(__FILE__, __LINE__, "CHECK_STR failed: %s == %s\n     got: '%.*s'\n  wanted: '%.*s'", \
               #a, #b, (int)check_a_.size, (char *)check_a_.str,               \
               (int)check_b_.size, (char *)check_b_.str);                      \
    }                                                                          \
  })
