# The file explorer

A directory as a buffer whose text *is* the listing, in the shape of oil.nvim.
Editing that text and writing it performs the filesystem operations:

| | |
|---|---|
| rename a file | `cw` |
| delete one | `dd` |
| create one | `o`, type a name |
| create a directory | `o`, type a name ending in `/` |
| move one | `dd`, then `p` in another directory's listing |
| apply it all | `:w` |

`:w` shows the plan and asks before touching the disk. Nothing happens until you
confirm.

`<leader>e` opens the explorer for the current file's directory, `-` goes up a
level, and `<CR>` opens whatever is under the cursor — a file in a buffer, a
directory in its own explorer. `:e <dir>` and `1coder <dir>` land there too.

`-` at the filesystem root does nothing rather than inventing a level above it.

## Why one buffer per directory

There is one buffer per directory rather than a single explorer that retargets.
A jump list entry stores a `BufferHandle` and an offset, so a shared buffer
would make every historical jump resolve against whatever directory happened to
be showing. Per-directory buffers also fall straight out of `BufferFromPath`'s
existing dedupe.

Because it is an ordinary buffer, undo, splits, search and every motion work in
it without the explorer knowing they exist.

## Images

Opening an image opens an image buffer rather than a wall of bytes. Rendering is
a placeholder for now — the buffer kind and the dispatch are in place.
