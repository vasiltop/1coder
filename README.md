# 1coder

A modal text editor with vim keybindings. One binary, no plugin manager, no
scripting language. Optional user config lives in a TOML file.

Native LSP support brings code navigation (jump to definition, type, implementation),
completions, diagnostics, formatting and rename. Language servers are auto-detected
from `PATH`, and can be overridden in config.

![1coder editing its own source, with a live grep for "test" narrowing in a
vertical split alongside](docs/screenshot-live-grep.png)

## Install

Download from the [latest release](https://github.com/vasiltop/1coder/releases/latest):
Unpack it and put `1coder` somewhere on your `PATH`.

## Configuring it

Sensible defaults are compiled in. To change bindings, theme colours, preferred
language servers, or the font, create:

```
~/.config/1coder/config.toml
```

On Windows: `%APPDATA%\1coder\config.toml`.  
(`$XDG_CONFIG_HOME/1coder/config.toml` when that variable is set.)

A full example with every default binding is in
[`config/config.example.toml`](config/config.example.toml). Copy it and keep only
what you want to change.

- `:config-reload` — re-read the file without restarting
- `:config-error-log` — open parse / validation errors (the status line only
  shows a short hint when something is wrong)

See [docs/config.md](docs/config.md) for the schema.

There is no plugin system and no scripting language.

## Building

```sh
git clone --recurse-submodules https://github.com/vasiltop/1coder
cmake -B build -S . && cmake --build build -j
```
