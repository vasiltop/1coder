#include "editor/editor.h"

// Default keybindings.
//
// These are installed at startup and again on :config-reload before any
// overrides from ~/.config/1coder/config.toml are applied. Everything here
// goes through the same KeymapBind used by user config, so nothing about these
// is privileged. See config/config.example.toml for the full listing.
//
// These mirror the user's neovim config (~/.config/nvim/lua/config/keymaps.lua)
// for every feature that exists here. Its mappings that depend on plugins --
// dap, neogit -- have no counterpart yet and are left out rather than
// approximated.
//
// Note how `d` is bound only as an operator in the normal map, with the motions
// it can take living in the operator-pending map. That means `dw`, `d$`, `d2j`
// and `dip` all work without any of them being spelled out -- and `dd` is just
// `d` followed by `d` in the operator-pending map.

void EditorInstallDefaultBindings(Editor *ed) {
  Keymap *global = ed->global_map;
  Keymap *normal = ed->normal_map;
  Keymap *insert = ed->insert_map;
  Keymap *visual = ed->visual_map;
  Keymap *pending = ed->operator_pending_map;
  Keymap *place = ed->cursor_place_map;

  // ---- global: work in every mode ----
  KeymapBind(global, "<C-w>v", CommandId::split_vertical);
  KeymapBind(global, "<C-w>s", CommandId::split_horizontal);
  KeymapBind(global, "<C-w>h", CommandId::focus_left);
  KeymapBind(global, "<C-w>j", CommandId::focus_down);
  KeymapBind(global, "<C-w>k", CommandId::focus_up);
  KeymapBind(global, "<C-w>l", CommandId::focus_right);
  KeymapBind(global, "<C-w>c", CommandId::close_window);
  KeymapBind(global, "<C-w>o", CommandId::only_window);
  KeymapBind(global, "<C-f>", CommandId::only_window);

  // Zoom, in every mode. The usual window-manager spellings rather than
  // anything vim has an opinion about.
  KeymapBind(global, "<C-=>", CommandId::zoom_in);
  KeymapBind(global, "<C-+>", CommandId::zoom_in);
  KeymapBind(global, "<C-->", CommandId::zoom_out);
  KeymapBind(global, "<C-0>", CommandId::zoom_reset);

  // ---- motions, shared by normal, visual and operator-pending ----
  // Bound into each map rather than a shared parent so that operator-pending
  // can override individual entries (see `d`, `y`, `c` below).
  Keymap *motion_maps[] = {normal, visual, pending};
  for (u64 i = 0; i < ArrayCount(motion_maps); i += 1) {
    Keymap *map = motion_maps[i];

    KeymapBind(map, "h", CommandId::cursor_left);
    KeymapBind(map, "j", CommandId::cursor_down);
    KeymapBind(map, "k", CommandId::cursor_up);
    KeymapBind(map, "l", CommandId::cursor_right);
    KeymapBind(map, "<Left>", CommandId::cursor_left);
    KeymapBind(map, "<Down>", CommandId::cursor_down);
    KeymapBind(map, "<Up>", CommandId::cursor_up);
    KeymapBind(map, "<Right>", CommandId::cursor_right);

    KeymapBind(map, "w", CommandId::word_forward);
    KeymapBind(map, "b", CommandId::word_backward);
    KeymapBind(map, "e", CommandId::word_end);
    KeymapBind(map, "W", CommandId::word_forward_big);
    KeymapBind(map, "B", CommandId::word_backward_big);
    KeymapBind(map, "E", CommandId::word_end_big);

    KeymapBind(map, "0", CommandId::line_start);
    KeymapBind(map, "^", CommandId::line_first_non_blank);
    // `_` is `^` made linewise, so d_ takes the whole line as dd does.
    KeymapBind(map, "_", CommandId::line_first_non_blank_linewise);
    KeymapBind(map, "$", CommandId::line_end);

    KeymapBind(map, "gg", CommandId::file_start);
    KeymapBind(map, "G", CommandId::file_end);

    KeymapBind(map, "{", CommandId::paragraph_backward);
    KeymapBind(map, "}", CommandId::paragraph_forward);

    KeymapBind(map, "f", CommandId::find_char_forward);
    KeymapBind(map, "F", CommandId::find_char_backward);
    KeymapBind(map, "t", CommandId::till_char_forward);
    KeymapBind(map, "T", CommandId::till_char_backward);

    KeymapBind(map, "%", CommandId::matching_bracket);
  }

  // ---- normal mode ----
  KeymapBind(normal, "i", CommandId::insert_mode);
  KeymapBind(normal, "I", CommandId::insert_line_start);
  KeymapBind(normal, "a", CommandId::append);
  KeymapBind(normal, "A", CommandId::append_line_end);
  KeymapBind(normal, "o", CommandId::open_line_below);
  KeymapBind(normal, "O", CommandId::open_line_above);
  KeymapBind(normal, "R", CommandId::replace_mode);
  KeymapBind(normal, "r", CommandId::replace_char);
  KeymapBind(normal, "~", CommandId::toggle_case);

  KeymapBind(normal, "v", CommandId::visual_mode);
  KeymapBind(normal, "V", CommandId::visual_line_mode);

  // `"x` picks the register for the next yank, delete or paste. "+ and "* are
  // the system clipboard, so "+y and "+p exchange text with other programs.
  KeymapBind(normal, "\"", CommandId::select_register);
  KeymapBind(visual, "\"", CommandId::select_register);

  KeymapBind(normal, "d", CommandId::operator_delete);
  KeymapBind(normal, "c", CommandId::operator_change);
  KeymapBind(normal, "y", CommandId::operator_yank);
  KeymapBind(normal, ">", CommandId::operator_indent);
  KeymapBind(normal, "<", CommandId::operator_dedent);

  KeymapBind(normal, "x", CommandId::delete_char);
  KeymapBind(normal, "X", CommandId::delete_char_before);
  KeymapBind(normal, "D", CommandId::delete_to_line_end);
  KeymapBind(normal, "C", CommandId::change_to_line_end);
  KeymapBind(normal, "p", CommandId::paste_after);
  KeymapBind(normal, "P", CommandId::paste_before);
  KeymapBind(normal, "J", CommandId::join_lines);

  KeymapBind(normal, "u", CommandId::undo);
  KeymapBind(normal, "<C-r>", CommandId::redo);
  KeymapBind(normal, ".", CommandId::repeat);
  KeymapBind(normal, "gi", CommandId::lsp_implementation);
  KeymapBind(normal, "gd", CommandId::lsp_definition);
  KeymapBind(normal, "gD", CommandId::lsp_declaration);
  KeymapBind(normal, "gt", CommandId::lsp_type_definition);

  // Jump list: <C-o> older, <C-i> newer. Tab is bound too because this input
  // path keeps Tab and Ctrl-I as distinct chords, while vim treats them alike.
  KeymapBind(normal, "<C-o>", CommandId::jump_older);
  KeymapBind(normal, "<C-i>", CommandId::jump_newer);
  KeymapBind(normal, "<Tab>", CommandId::jump_newer);

  KeymapBind(normal, "<C-d>", CommandId::scroll_half_page_down);
  KeymapBind(normal, "<C-u>", CommandId::scroll_half_page_up);
  KeymapBind(normal, "<C-e>", CommandId::scroll_line_down);
  KeymapBind(normal, "<C-y>", CommandId::scroll_line_up);
  KeymapBind(normal, "zz", CommandId::center_line);

  // `-` opens the containing directory, so `-` and <CR> navigate the
  // filesystem as inverses of each other from anywhere in the editor.
  KeymapBind(normal, "-", CommandId::explorer_parent);

  KeymapBind(normal, ":", CommandId::command_line_open);
  KeymapBind(normal, "<Esc>", CommandId::normal_mode);

  // ---- window navigation ----
  // Bare ctrl-hjkl to move focus, alongside the <C-w> forms above, which is how
  // the neovim config layers its mappings over the built-in ones.
  KeymapBind(normal, "<C-h>", CommandId::focus_left);
  KeymapBind(normal, "<C-j>", CommandId::focus_down);
  KeymapBind(normal, "<C-k>", CommandId::focus_up);
  KeymapBind(normal, "<C-l>", CommandId::focus_right);
  KeymapBind(normal, "<C-Space>", CommandId::lsp_hover);

  // ---- leader bindings ----
  // <leader>h and <leader>v mirror :split and :vsplit, so h is the horizontal
  // divider (stacked windows) and v the vertical one (side by side).
  KeymapBind(normal, "<leader>h", CommandId::split_horizontal);
  KeymapBind(normal, "<leader>v", CommandId::split_vertical);

  // The neovim config's mini.pick bindings, pointed at the equivalents here.
  KeymapBind(normal, "<leader>pf", CommandId::find_file);
  // The neovim config's grep binding is the live one; the one-shot `:grep` is
  // still there for when a fixed list is what is wanted.
  KeymapBind(normal, "<leader>pg", CommandId::live_grep);
  KeymapBind(normal, "<leader>pb", CommandId::list_buffers);
  KeymapBind(normal, "<leader>pp", CommandId::find_git);
  KeymapBind(normal, "<leader>sc", CommandId::set_cwd);

  // The explorer for the containing directory. `-` does the same thing; this is
  // the discoverable spelling of it.
  KeymapBind(normal, "<leader>e", CommandId::explorer_parent);

  // Magit-style git status. Buffer-local keys (s/u/F/P/r/...) live on the
  // [git] buffer itself; this only opens it.
  KeymapBind(normal, "<leader>gs", CommandId::git);
  KeymapBind(normal, "<leader>cf", CommandId::lsp_format);
  KeymapBind(normal, "<leader>rn", CommandId::lsp_rename);
  KeymapBind(normal, "<leader>d", CommandId::lsp_diagnostic_float);

  // compile-mode.nvim's recompile and circular next/prev error bindings.
  KeymapBind(normal, "<leader>rc", CommandId::recompile);
  KeymapBind(normal, "<leader>ne", CommandId::next_error);
  KeymapBind(normal, "<leader>pe", CommandId::prev_error);

  // ---- multiple cursors ----
  // Placement is staged: <leader>mc starts it and marks the first position,
  // motions move between positions, <CR> makes the marks live and <Esc>
  // abandons them. The placement map is parented to the normal one, so every
  // motion bound above stays available for aiming.
  //
  // Marking repeats on a bare `c`: a mark is made once per position, and a
  // leader sequence each time would be tiring. It shadows the change operator,
  // but only for as long as placement is under way -- and changing text is not
  // what `c` is for while choosing where the cursors go.
  KeymapBind(normal, "<leader>mc", CommandId::cursor_place);
  KeymapBind(place, "c", CommandId::cursor_place_mark);
  // `A` marks the end of the line, borrowing vim's append-at-end-of-line
  // spelling. Normal mode cannot rest past the last character, so without this
  // there is no way to mark one cursor at the end of a line and another at the
  // start of the next -- which is much of the point of having several.
  KeymapBind(place, "A", CommandId::cursor_place_mark_line_end);
  KeymapBind(place, "<CR>", CommandId::cursor_place_confirm);
  KeymapBind(place, "<Esc>", CommandId::cursor_place_cancel);

  // ---- text objects ----
  // `i` and `a` are prefixes: the chord after them names the object. They apply
  // in operator-pending mode (`ciw`) and in visual mode (`vi(`), but mean
  // nothing on their own in normal mode, where `i` and `a` insert.
  KeymapBind(pending, "i", CommandId::text_object_inner);
  KeymapBind(pending, "a", CommandId::text_object_around);
  KeymapBind(visual, "i", CommandId::text_object_inner);
  KeymapBind(visual, "a", CommandId::text_object_around);

  // ---- in-file search ----
  // `n`/`N` and `*`/`#` also work in visual mode, where they extend the
  // selection to the match by moving the cursor end of it.
  KeymapBind(normal, "/", CommandId::search_forward);
  KeymapBind(normal, "?", CommandId::search_backward);
  KeymapBind(normal, "n", CommandId::search_next);
  KeymapBind(normal, "N", CommandId::search_prev);
  KeymapBind(normal, "*", CommandId::search_word_forward);
  KeymapBind(normal, "#", CommandId::search_word_backward);
  KeymapBind(visual, "n", CommandId::search_next);
  KeymapBind(visual, "N", CommandId::search_prev);
  KeymapBind(visual, "*", CommandId::search_word_forward);
  KeymapBind(visual, "#", CommandId::search_word_backward);
  // Matching the nvim config's `<leader>/ -> :nohlsearch`.
  KeymapBind(normal, "<leader>/", CommandId::search_clear);

  // ---- macros ----
  KeymapBind(normal, "q", CommandId::macro_record);
  // `@@` needs no binding of its own: `@` starts waiting for a register name,
  // and '@' is itself the name meaning "the last macro".
  KeymapBind(normal, "@", CommandId::macro_play);

  // ---- operator-pending ----
  // A doubled operator acts on whole lines: dd, yy, cc, >>, <<.
  KeymapBind(pending, "d", CommandId::delete_line);
  KeymapBind(pending, "y", CommandId::yank_line);
  KeymapBind(pending, "c", CommandId::change_line);
  KeymapBind(pending, ">", CommandId::indent_line);
  KeymapBind(pending, "<", CommandId::dedent_line);
  KeymapBind(pending, "<Esc>", CommandId::normal_mode);

  // ---- visual mode ----
  KeymapBind(visual, "d", CommandId::operator_delete);
  KeymapBind(visual, "x", CommandId::operator_delete);
  KeymapBind(visual, "c", CommandId::operator_change);
  KeymapBind(visual, "y", CommandId::operator_yank);
  KeymapBind(visual, ">", CommandId::operator_indent);
  KeymapBind(visual, "<", CommandId::operator_dedent);
  KeymapBind(visual, "J", CommandId::join_lines);
  KeymapBind(visual, "v", CommandId::visual_mode);
  KeymapBind(visual, "V", CommandId::visual_line_mode);
  KeymapBind(visual, "<Esc>", CommandId::normal_mode);
  KeymapBind(visual, ":", CommandId::command_line_open);

  // ---- insert mode ----
  // Sparse by design: everything unbound falls through to text insertion.
  KeymapBind(insert, "<Esc>", CommandId::normal_mode);
  KeymapBind(insert, "<CR>", CommandId::insert_newline);
  KeymapBind(insert, "<Tab>", CommandId::insert_tab);
  KeymapBind(insert, "<BS>", CommandId::backspace);
  // Word rubout: <C-w> is vim's own, <C-h> is what the neovim config maps, and
  // <C-BS> is what a window system sends for ctrl-backspace.
  KeymapBind(insert, "<C-w>", CommandId::delete_word_before);
  KeymapBind(insert, "<C-h>", CommandId::delete_word_before);
  KeymapBind(insert, "<C-BS>", CommandId::delete_word_before);
  // <C-r>{reg} inserts a register without leaving insert mode, as vim does.
  KeymapBind(insert, "<C-r>", CommandId::insert_register_prompt);
  KeymapBind(insert, "<C-Space>", CommandId::lsp_completion);
  KeymapBind(insert, "<Left>", CommandId::cursor_left);
  KeymapBind(insert, "<Down>", CommandId::cursor_down);
  KeymapBind(insert, "<Up>", CommandId::cursor_up);
  KeymapBind(insert, "<Right>", CommandId::cursor_right);
}
