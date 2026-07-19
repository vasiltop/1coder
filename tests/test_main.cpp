#include "test.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace {

// Zero-initialized before any registrar runs, so registration order is safe.
TestCase *g_tests = nullptr;
TestCase *g_tests_last = nullptr;
u32 g_current_failures = 0;

}  // namespace

void TestRegister(TestCase *test) {
  if (g_tests_last) {
    g_tests_last->next = test;
    g_tests_last = test;
  } else {
    g_tests = g_tests_last = test;
  }
}

void TestFail(const char *file, int line, const char *fmt, ...) {
  g_current_failures += 1;
  fprintf(stderr, "  %s:%d: ", file, line);
  va_list args;
  va_start(args, fmt);
  vfprintf(stderr, fmt, args);
  va_end(args);
  fprintf(stderr, "\n");
}

int main(int argc, char **argv) {
  // Optional substring filter: ./editor_tests gap_buffer
  const char *filter = (argc > 1) ? argv[1] : nullptr;

  u32 run = 0, passed = 0, failed = 0, skipped = 0;

  for (TestCase *t = g_tests; t; t = t->next) {
    if (filter && !strstr(t->name, filter)) {
      skipped += 1;
      continue;
    }

    g_current_failures = 0;
    run += 1;
    t->fn();

    if (g_current_failures == 0) {
      passed += 1;
    } else {
      failed += 1;
      fprintf(stderr, "FAIL %s (%s)\n", t->name, t->file);
    }
  }

  printf("\n%u run, %u passed, %u failed", run, passed, failed);
  if (skipped) printf(", %u filtered out", skipped);
  printf("\n");

  return failed ? 1 : 0;
}
