# 1coder docs

How the editor works, and why it is built the way it is.

For day-to-day use the [README](../README.md) is enough. This is for learning
the editor properly, or for changing it.

## Using it

- [Keybindings](keybindings.md) — the complete key reference
- [Language server protocol](lsp.md) — auto-detected servers, navigation, completion
- [The command window](commands.md) — `:` syntax, and how to add a command
- [Search](search.md) — live grep, file finder, buffer list
- [The file explorer](explorer.md) — directories as editable text
- [Fonts and colours](rendering.md) — what is configurable without a rebuild

## Changing it

- [Architecture](architecture.md) — start here
- [Development](development.md) — building, tests, screenshots, roadmap

## The short version

Procedural C++23 in the Handmade Hero / RAD Debugger style: plain structs and
free functions, arena allocation, no inheritance, no scripting language. All
configuration is source code.

The load-bearing decision is the line between `core/` and `app/` — everything
about editing is testable without opening a window. [Architecture](architecture.md)
explains what that buys and what it costs.
