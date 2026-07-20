# The command window

Opened with `:`. Syntax:

```
[range] name[!] [arguments]
```

- **range** — `%` whole buffer · `.` current line · `$` last line · `N` ·
  `N,M` · `.,+3` · `'<,'>` the visual selection
- **name** — a full name, an unambiguous prefix (`und` → `undo`), or a short
  alias (`w` `q` `wq` `qa` `e` `vs` `sp` `bn` …)
- **!** — the forced variant, where a command defines one (`q!` discards unsaved
  changes)
- **arguments** — whitespace separated; `"quote them"` to include spaces

Tab completes: command names while typing the name, then whatever the command's
declared argument kind calls for — filesystem paths, buffer names, or command
names. `:commands` lists everything with its shape.

The window is an ordinary buffer, so every motion and operator works inside it.
It opens in insert mode; `<Esc>` drops to normal mode, `<Esc>` again abandons.

## Adding a command

One line in `COMMAND_LIST` (`core/command/command_id.h`) declaring its name,
description, argument kind and flags:

```c
X(reload_file, "reload", "Reread the file from disk", CommandArg::None, CmdBang)
```

and a body in `core/editor/command.cpp`:

```cpp
static void Cmd_reload_file(CommandArgs *a) { /* ... */ }
```

The macro generates the enum, the name/id hash and the dispatch table. A missing
body is a link error, not a silent gap. It is immediately reachable from the
command window, from completion, and from any keybinding.

Because keybindings and the command window resolve to the same `CommandId` and
run the same body, `:1d` and `dd` cannot drift apart.
