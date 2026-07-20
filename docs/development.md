# Development

Needs CMake, a C++23 compiler, and SDL3. Builds on Linux, macOS and Windows.

```sh
git clone --recurse-submodules https://github.com/vasiltop/1coder
cmake -B build -S . && cmake --build build -j

./build/editor path/to/file      # run
./build/editor_tests             # 343 tests, needs no display
```

SDL3 comes from the `third_party/SDL` submodule; without it CMake falls back to
a system SDL3, and without that it builds the tests alone. `stb_truetype` is
vendored. Nothing else is required.

With [just](https://github.com/casey/just), `just` on its own lists everything:

```sh
just build                 # debug
just release               # optimised
just test [filter]         # headless test suite, optional name filter
just run file.c            # build and open
just ci                    # clean build + tests + boundary check
just boundary              # fail if core/ has picked up an SDL dependency
just shot o.bmp f.c --keys '<C-w>v10j'   # render one frame, no display needed
just compdb                # compile_commands.json for clangd
```

## Testing

The test binary links `editor_core` only — no SDL, no display — which is what
the `core/` and `app/` split is for. `just boundary` fails the build if anything
under `core/` picks up an SDL include, and CI runs it.

Tests that need real files use the fixtures in `tests/test_tempdir.h`. They go
through the os layer rather than shelling out, so they run on every platform.

`--screenshot <path>` renders one frame to a BMP and exits; `--keys <spec>`
replays a binding spec first. Together they capture the editor in any state
without a display:

```sh
just shot out.bmp file.c --keys '<C-w>v10jVj'
# or, without just:
SDL_VIDEODRIVER=dummy ./build/editor file.c --keys '<C-w>v10jVj' --screenshot out.bmp
```

`just vimdiff [filter]` runs the editor and a clean headless Neovim through the
same non-remapped interactive keystrokes, then compares exact text and cursor
position. `just vimdiff-quick` runs the smaller smoke corpus; neither command
skips known differences.

## CI and releases

`.github/workflows/ci.yml` builds and tests on Linux, macOS and Windows for
every push and pull request. It checks out without submodules, so it builds the
tests rather than the editor — that is deliberate, and keeps PRs fast.

`.github/workflows/release.yml` runs on a `v*` tag. It builds with submodules on
all three platforms, **runs the test suite against the release binary**, packages
an archive per platform, and publishes them together. If any platform fails, no
release is published — the tests are a gate, not a formality.

Tag only commits that CI has already gone green on. A tag that fails to build
cannot be retried into existence; it has to be moved or bumped.

## Platform notes

Windows builds with MSVC. Its generator is multi-config, so the justfile passes
both `CMAKE_BUILD_TYPE` and `--config`, and CMake is told to put binaries in the
build directory itself rather than in `build/Debug/`. The CRT is linked
statically so the shipped `.exe` needs no redistributable.

Symlinks cannot be created by an unprivileged Windows process without Developer
Mode, so the tests that need them skip rather than fail.

## Not yet implemented

Tree-sitter highlighting (the `TokenArray` seam and the theme's colour table are
in place and unused), a git client, marks, visual block mode (the `VimMode` case
exists as a seam), regular expressions in search, and image rendering beyond the
placeholder buffer.
