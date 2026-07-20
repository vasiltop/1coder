#include "test.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER)
#  include <crtdbg.h>
#endif

namespace {

// The MSVC debug CRT reports corruption and failed assertions through a modal
// dialog by default. There is nobody to dismiss it on a build server, so the
// run blocks until the job times out with nothing written to the log -- which
// is exactly how this suite has been failing on Windows. Route the reports to
// stderr so they are printed and the process dies promptly instead.
//
// EDITOR_TESTS_HEAPCHECK additionally validates the whole heap on every
// allocation, which turns a delayed STATUS_HEAP_CORRUPTION into a report
// naming the block that was overrun. It is very slow, so it stays opt-in.
void ConfigureCrtDiagnostics() {
#if defined(_MSC_VER)
  for (int report : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) {
    _CrtSetReportMode(report, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(report, _CRTDBG_FILE_STDERR);
  }
  if (getenv("EDITOR_TESTS_HEAPCHECK") != nullptr) {
    _CrtSetDbgFlag(_CrtSetDbgFlag(_CRTDBG_REPORT_FLAG) | _CRTDBG_ALLOC_MEM_DF |
                   _CRTDBG_CHECK_ALWAYS_DF);
  }
#endif
}

}  // namespace

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
  ConfigureCrtDiagnostics();

  // Optional substring filter: ./editor_tests gap_buffer
  const char *filter = (argc > 1) ? argv[1] : nullptr;
  // The summary only prints once every test has finished, and stdout is block
  // buffered into a pipe under CI, so a test that deadlocks leaves no trace of
  // which one it was. EDITOR_TESTS_TRACE names each test before running it.
  const bool trace = getenv("EDITOR_TESTS_TRACE") != nullptr;

  u32 run = 0, passed = 0, failed = 0, skipped = 0;

  for (TestCase *t = g_tests; t; t = t->next) {
    if (filter && !strstr(t->name, filter)) {
      skipped += 1;
      continue;
    }

    g_current_failures = 0;
    run += 1;
    if (trace) {
      fprintf(stderr, "[run] %s\n", t->name);
      fflush(stderr);
    }
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
