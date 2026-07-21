# Configuration

1coder ships with compiled-in defaults. An optional TOML file overlays them at
startup and again when you run `:config-reload`.

## Location

1. `$XDG_CONFIG_HOME/1coder/config.toml` if `XDG_CONFIG_HOME` is set
2. otherwise:
   - Unix / macOS: `~/.config/1coder/config.toml`
   - Windows: `%APPDATA%\1coder\config.toml` (typically `C:\Users\<you>\AppData\Roaming\1coder\config.toml`)

Missing file → defaults, no error.

Invalid file → previous (or default) settings stay applied. The status line
shows `config: errors — :config-error-log`; open the details with
`:config-error-log`.

## Example

A complete listing of the default bindings (plus commented theme / LSP / font
sections) lives in [`config/config.example.toml`](../config/config.example.toml).
Copy it to the path above and edit.

## Schema

### `[font]`

```toml
[font]
path = "/usr/share/fonts/TTF/JetBrainsMono-Regular.ttf"
face = ""          # face name inside a .ttc collection
size = 16.0
```

Precedence: built-in monospace discovery → `[font]` → `EDITOR_FONT` /
`EDITOR_FONT_FACE` / `EDITOR_FONT_SIZE` environment variables.

### `[theme]` and `[theme.syntax]`

Colours are `#RRGGBB` or `#RRGGBBAA`. Any omitted key keeps the compiled
default from `app/render/theme.cpp`.

```toml
[theme]
background = "#121212"
text = "#FAFBF6"

[theme.syntax]
keyword = "#C678DD"
string = "#98C379"
```

Field names match the `Theme` struct (`background`, `cursor`, `selection`, …).
Syntax keys are lowercased `TokenKind` names (`keyword`, `comment`, …).

### `[lsp.<language>]`

Preferred server for a language, tried before the built-in candidate list.

Languages: `cpp`, `rust`, `go`, `python`, `typescript` (also accepts `c` /
`javascript` as aliases).

```toml
[lsp.python]
command = "pyright-langserver"
args = ["--stdio"]
```

`command` may be a bare name (resolved on `PATH` and common user install dirs)
or an absolute path. If the preferred server is missing, the editor falls back
to the built-in candidates.

### `[bindings.<map>]`

Maps: `global`, `normal`, `insert`, `visual`, `pending` (operator-pending),
`place` (multi-cursor placement).

Keys are binding specs (`"gd"`, `"<leader>ff"`, `"<C-w>v"`). Values are command
names from `:commands`.

```toml
[bindings.normal]
"gd" = "definition"
"<leader>ff" = "find"
```

On reload, mode maps are cleared and defaults are reinstalled before your
overrides apply, so removing a line from the file restores the compiled
binding.

## Commands

| Command | Action |
|---------|--------|
| `:config-reload` | Re-read the config file |
| `:config-error-log` | Show the last load errors in a `[config-errors]` buffer |
