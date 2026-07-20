#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#  include <fcntl.h>
#  include <io.h>
#  include <windows.h>
#else
#  include <time.h>
#endif

namespace {

void SetBinaryMode() {
#if defined(_WIN32)
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
  _setmode(_fileno(stderr), _O_BINARY);
#endif
}

int EchoMode() {
  fputs("fake_lsp_server: stderr ready\n", stderr);
  fflush(stderr);

  unsigned char buffer[4096];
  for (;;) {
    size_t got = fread(buffer, 1, sizeof(buffer), stdin);
    if (got > 0) {
      if (fwrite(buffer, 1, got, stdout) != got) return 1;
      fflush(stdout);
    }
    if (got < sizeof(buffer)) {
      if (ferror(stdin)) return 1;
      if (feof(stdin)) return 0;
    }
  }
}

int SleepMode() {
#if defined(_WIN32)
  for (;;) Sleep(1000);
#else
  struct timespec delay = {1, 0};
  for (;;) nanosleep(&delay, nullptr);
#endif
}

}  // namespace

int main(int argc, char **argv) {
  SetBinaryMode();

  if (argc > 1 && strcmp(argv[1], "--echo") == 0) return EchoMode();
  if (argc > 1 && strcmp(argv[1], "--sleep") == 0) return SleepMode();

  fputs("fake_lsp_server: expected --echo or --sleep\n", stderr);
  return 2;
}
