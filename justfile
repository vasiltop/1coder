# 1code

# Recipe arguments reach the shell as "$@" rather than being interpolated, so
# things like --keys '<leader>v' survive instead of being read as redirections.
set positional-arguments

build_dir := "build"
release_dir := "build-release"

# Show the available recipes.
default:
    @just --list

# Configure and build (debug).
build:
    cmake -B {{build_dir}} -S . -DCMAKE_BUILD_TYPE=Debug
    cmake --build {{build_dir}} --config Debug -j

# Configure and build with optimisations.
# --config is what multi-config generators (Visual Studio) read; CMAKE_BUILD_TYPE
# is what single-config ones read. Passing both keeps every platform on Release.
release:
    cmake -B {{release_dir}} -S . -DCMAKE_BUILD_TYPE=Release
    cmake --build {{release_dir}} --config Release -j

# Run the headless test suite. Takes an optional name filter.
test filter="": build
    ./{{build_dir}}/editor_tests {{filter}}

# Open files in the editor.
run *files: build
    @./{{build_dir}}/editor "$@"

# Open files using the optimised binary.
run-release *files: release
    @./{{release_dir}}/editor "$@"

# Render one frame to a BMP with no display: just shot o.bmp f.c --keys '<C-w>v10j'
shot out *args: build
    #!/usr/bin/env bash
    set -euo pipefail
    out="$1"; shift
    SDL_VIDEODRIVER=dummy ./{{build_dir}}/editor "$@" --screenshot "$out"
    echo "wrote $out"

# Differential test against neovim. Optionally filter: just vimdiff dw
vimdiff filter="": build
    python3 tools/vimdiff.py -k "{{filter}}"

# A smaller neovim comparison, for a quick check.
vimdiff-quick: build
    python3 tools/vimdiff.py --quick

# Build from scratch, run the tests and check the core/app boundary.
ci:
    rm -rf {{build_dir}}
    @just build
    ./{{build_dir}}/editor_tests
    @just boundary
    # Release compiles out the arena assertions and NDEBUG has broken the build
    # before, so it is worth building both.
    cmake -B {{release_dir}} -S . -DCMAKE_BUILD_TYPE=Release
    cmake --build {{release_dir}} --config Release -j
    ./{{release_dir}}/editor_tests

# Fail if anything under core/ has picked up a dependency on SDL.
boundary:
    #!/usr/bin/env bash
    # The whole testing story rests on core/ staying graphics-free, so this is
    # worth checking rather than trusting.
    set -euo pipefail
    if grep -rn --include='*.cpp' --include='*.h' '#include *[<"]SDL' core/; then
        echo "core/ must not include SDL" >&2
        exit 1
    fi
    echo "core/ is free of SDL"

# Line counts per layer.
loc:
    @echo "core:  $(find core  -name '*.cpp' -o -name '*.h' | xargs cat | wc -l)"
    @echo "app:   $(find app   -name '*.cpp' -o -name '*.h' | xargs cat | wc -l)"
    @echo "tests: $(find tests -name '*.cpp' -o -name '*.h' | xargs cat | wc -l)"

# Generate compile_commands.json for clangd.
compdb:
    cmake -B {{build_dir}} -S . -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
    ln -sf {{build_dir}}/compile_commands.json compile_commands.json

# Remove build outputs.
clean:
    rm -rf {{build_dir}} {{release_dir}} compile_commands.json
