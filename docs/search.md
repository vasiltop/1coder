# Search

`<leader>pg` searches the project as you type, `<leader>pf` fuzzy-finds a file,
and `<leader>pb` fuzzy-picks an open buffer — where the neovim config puts them.
By name: `:live-grep`, `:find`, `:buffers`, plus `:grep <pattern>` for a fixed list.

None of them adds a UI concept, which was the point of the buffer system:

- **A results list** is a read-only buffer whose keymap claims `<CR>`, with the
  matches hanging off `user_data`. `<CR>` opens the file at that line.
- **A picker** is the same thing with an editable query on its first line. Its
  `on_edit` hook rewrites everything below as you type, so it opens in insert
  mode and narrows live. Result lines are not editable — `<Esc>` drops to
  normal mode to navigate them with `j`/`k`, and `<CR>` opens one.

Live search reads the tree once into memory and rescans those bytes per
keystroke; re-reading the project on every character would not be usable. The
corpus is capped, and reopening reuses it.

Everything else — scrolling, splits, vim motions, undo — is inherited, because
these are ordinary buffers. Matching is literal with vim's 'smartcase'; there
are no regular expressions yet.

Version control directories, build output and object files are skipped by the
walk.
