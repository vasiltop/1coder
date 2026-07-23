# Git client

A Magit-inspired status buffer. `<leader>gs` or `:git` opens `[git]` for the
repository containing the current file (or the editor's working directory).

The buffer's text *is* the status: a header, then Untracked / Unstaged / Staged
sections. Vim motions work as usual; the buffer-local map claims only the keys
below.

## Status keys

| Key | Action |
|-----|--------|
| `s` | Stage file or hunk under the cursor |
| `u` | Unstage file or hunk under the cursor |
| `x` | Discard unstaged/untracked (asks `[y/N]`) |
| `Tab` | On a file, expand/collapse it into hunks; on a hunk, fold just that hunk |
| `d` | Open a diff view for the file / whole tree |
| `Enter` | Open the file, or show a commit from the log |
| `l` | Open `[git-log]` |
| `c` | Prompt for `:git-commit ` |
| `R` | Refresh |
| `F` | Pull (honours Args) |
| `P` | Push (honours Args) |

## Argument toggles

Flags live on the status buffer and show on the `Args:` header line. Toggle
them, then run the action:

| Key | Flag | Used by |
|-----|------|---------|
| `r` | `--rebase` | pull |
| `a` | `--autostash` | pull |
| `f` | `--ff-only` | pull |
| `U` | `-u` | push |

Example: `r` then `F` runs `git pull --rebase`.

One-shot flags also work from the command window: `:git-pull --rebase`,
`:git-push -u`.

## Other commands

| Command | |
|---------|---|
| `:git` | Open status |
| `:git-log` | Open log |
| `:git-commit <msg>` | Commit staged changes |
| `:git-pull [flags]` | Pull |
| `:git-push [flags]` | Push |

Diff and log buffers are the same `BufferKind::Git` with a different view; they
open in a vertical split beside the current window when not already visible. In a
commit diff opened from the log, `Tab` folds the hunk under the cursor down to
its `@@` header (marked with a trailing ` ...`); `Tab` again unfolds it.
