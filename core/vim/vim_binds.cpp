#include "editor/editor.h"

// Default keybindings.
//
// This is the whole configuration surface: there is no scripting language, so
// changing a binding means editing this file. Everything here goes through the
// same KeymapBind used by any other code, so nothing about these is privileged.
//
// These mirror the user's neovim config (~/.config/nvim/lua/config/keymaps.lua)
// for every feature that exists here. Its mappings that depend on plugins or
// LSP -- the file explorer, mini.pick, compile-mode, dap, neogit, search
// highlighting -- have no counterpart yet and are left out rather than
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

  // ---- global: work in every mode ----
  KeymapBind(global, "<C-w>v", CommandId::split_vertical);
  KeymapBind(global, "<C-w>s", CommandId::split_horizontal);
  KeymapBind(global, "<C-w>h", CommandId::focus_left);
  KeymapBind(global, "<C-w>j", CommandId::focus_down);
  KeymapBind(global, "<C-w>k", CommandId::focus_up);
  KeymapBind(global, "<C-w>l", CommandId::focus_right);
  KeymapBind(global, "<C-w>c", CommandId::close_window);
  KeymapBind(global, "<C-w>o", CommandId::only_window);

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

  KeymapBind(normal, "v", CommandId::visual_mode);
  KeymapBind(normal, "V", CommandId::visual_line_mode);

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

  KeymapBind(normal, "<C-d>", CommandId::scroll_half_page_down);
  KeymapBind(normal, "<C-u>", CommandId::scroll_half_page_up);
  KeymapBind(normal, "<C-e>", CommandId::scroll_line_down);
  KeymapBind(normal, "<C-y>", CommandId::scroll_line_up);
  KeymapBind(normal, "zz", CommandId::center_line);

  KeymapBind(normal, ":", CommandId::command_line_open);
  KeymapBind(normal, "<Esc>", CommandId::normal_mode);

  // ---- window navigation ----
  // Bare ctrl-hjkl to move focus, alongside the <C-w> forms above, which is how
  // the neovim config layers its mappings over the built-in ones.
  KeymapBind(normal, "<C-h>", CommandId::focus_left);
  KeymapBind(normal, "<C-j>", CommandId::focus_down);
  KeymapBind(normal, "<C-k>", CommandId::focus_up);
  KeymapBind(normal, "<C-l>", CommandId::focus_right);

  // ---- leader bindings ----
  // <leader>h and <leader>v mirror :split and :vsplit, so h is the horizontal
  // divider (stacked windows) and v the vertical one (side by side).
  KeymapBind(normal, "<leader>h", CommandId::split_horizontal);
  KeymapBind(normal, "<leader>v", CommandId::split_vertical);

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
  KeymapBind(insert, "<Left>", CommandId::cursor_left);
  KeymapBind(insert, "<Down>", CommandId::cursor_down);
  KeymapBind(insert, "<Up>", CommandId::cursor_up);
  KeymapBind(insert, "<Right>", CommandId::cursor_right);
}
