# Language server protocol

1coder includes built-in LSP support with auto-detected language servers, available through keybindings and completions.

## Supported languages

The editor auto-detects the following language servers, searching `PATH` and then common user install dirs (`~/.local/share/nvim/mason/bin`, `~/.local/bin`, `~/.cargo/bin`):

- **C/C++**: `clangd`
- **Rust**: `rust-analyzer`
- **Go**: `gopls`
- **Python**: `basedpyright-langserver` (falls back to `pyright` if unavailable)
- **JavaScript/TypeScript**: `typescript-language-server`

Lua is not mapped to any language server.

The editor finds each server once per project root (detected from nearest marker: `.git`, `Cargo.toml`, `go.mod`, `pyproject.toml`, `package.json`, or `.` as fallback). One server instance runs per language per root and restarts once if it crashes.

## Commands and bindings

### Code navigation

- `gi` — jump to definition of identifier under cursor
- `gd` — jump to type definition
- `gD` — jump to implementation
- `gt` — jump to type of identifier under cursor (hover, then press `gt`)

### Editing

- `<leader>cf` — format buffer (leaves buffer marked dirty)
- `<leader>rn` — rename identifier (leaves buffer marked dirty, prompts in command window)
- `<leader>d` — list diagnostics for buffer in command window

### Completions

In normal mode:
- `<C-Space>` — show hover information for identifier under cursor

In insert mode:
- `<C-Space>` — manually trigger completions (else automatic on `.` / `->` / `:` for most languages)
- `Up` / `Down` or `Ctrl-N` / `Ctrl-P` — cycle through completions
- `Enter` — accept highlighted completion
- `Esc` — dismiss completions

## Diagnostics

Diagnostics are underlined in the buffer and marked in the gutter. Use `<leader>d` to list them for the current buffer.

## Server status and troubleshooting

If a server fails to start, the command window shows a status message. If a running server crashes, the editor attempts one automatic restart. A missing or persistently crashed server does not prevent editing.

Language servers are auto-detected by default. Prefer a specific binary with
`[lsp.<language>]` in `~/.config/1coder/config.toml` (see [configuration](config.md)).
Ensure servers are available on `PATH` (or give an absolute `command`).
