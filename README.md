# 1code

A modal text editor with vim keybindings. One binary, no config files, no plugin
manager, no scripting language.

If you know vim, you already know how to drive it.

## Install

Download from the [latest release](https://github.com/vasiltop/1code/releases/latest):

| Platform | File |
|---|---|
| Linux | `1code-<version>-linux-x86_64.tar.gz` |
| macOS (Apple Silicon) | `1code-<version>-macos-arm64.tar.gz` |
| Windows | `1code-<version>-windows-x86_64.zip` |

Unpack it and put `1code` somewhere on your `PATH`.

The macOS and Windows builds are unsigned, so the first launch needs a nudge:
on macOS, right-click → Open, or `xattr -d com.apple.quarantine 1code`; on
Windows, "More info" → "Run anyway".

## Start here

```sh
1code file.c
```

| | |
|---|---|
| `:w` `:q` | save, quit |
| `<space>` | leader |
| `<space>pf` | find a file |
| `<space>pg` | search the project as you type |
| `<space>e` | file explorer |
| `<space>v` | split vertically |
| `:` | command window |

Motions, operators, text objects, registers, macros, counts and the jump list
behave the way you expect. The [key reference](docs/keybindings.md) is the
complete list.

Two things worth knowing early, because they are not quite vim:

**The file explorer is editable text.** Rename a file with `cw`, delete one with
`dd`, create one with `o`. Then `:w`, which shows you the plan and asks before
touching anything.

**Everything is a buffer** — search results, the command line, the explorer. So
`dw` edits the command you are typing, `j` and `k` walk search results, and undo
works in all of it.

## Configuring it

There are no config files, and there is no plugin system. Fonts come from the
environment:

```sh
EDITOR_FONT_SIZE=16 1code file.c
```

Everything else — colours, keybindings, defaults — is a source change and a
rebuild. That is the deal this editor makes, and the [docs](docs/) explain what
to change.

## Docs

- [Keybindings](docs/keybindings.md) · [Command window](docs/commands.md) ·
  [Search](docs/search.md) · [Explorer](docs/explorer.md) ·
  [Fonts and colours](docs/rendering.md)
- [Architecture](docs/architecture.md) and [development](docs/development.md),
  if you want to build it or change it

## Building

```sh
git clone --recurse-submodules https://github.com/vasiltop/1code
cmake -B build -S . && cmake --build build -j
```

Needs CMake, a C++23 compiler, and SDL3. More in [development](docs/development.md).
