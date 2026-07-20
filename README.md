# 1coder

A modal text editor with vim keybindings. One binary, no config files, no plugin
manager, no scripting language.

Native LSP support brings code navigation (jump to definition, type, implementation),
completions, diagnostics, formatting and rename. Language servers are auto-detected
from `PATH`.

![1coder editing its own source, with a live grep for "test" narrowing in a
vertical split alongside](docs/screenshot-live-grep.png)

## Install

Download from the [latest release](https://github.com/vasiltop/1coder/releases/latest):
Unpack it and put `1coder` somewhere on your `PATH`.

## Configuring it

There are no config files, and there is no plugin system. To configure the editor you must rebuild it from source.

## Building

```sh
git clone --recurse-submodules https://github.com/vasiltop/1coder
cmake -B build -S . && cmake --build build -j
```
