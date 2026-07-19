#pragma once

#include "base/base_arena.h"
#include "base/base_string.h"
#include "base/base_types.h"

// Command *identity*: the canonical list of every action the editor can
// perform, and the name<->id mapping.
//
// Identity is separated from dispatch on purpose. The keymap binds ids and
// needs nothing else, so it can sit below the editor; the table of function
// pointers that actually runs a command lives in editor/command.cpp, where the
// Editor, View and Buffer types exist. Adding a command is a single line here
// plus its body there -- and the linker catches a missing body.
//
// Keybindings and the `:` command line both resolve to these ids, so there is
// exactly one dispatch path regardless of how an action was invoked.

// X(identifier, "name typed at the : prompt", "description")
#define COMMAND_LIST                                                                        \
  /* ---- motions ---- */                                                                   \
  X(cursor_left,             "cursor-left",             "Move cursor left")                 \
  X(cursor_right,            "cursor-right",            "Move cursor right")                \
  X(cursor_up,               "cursor-up",               "Move cursor up one line")          \
  X(cursor_down,             "cursor-down",             "Move cursor down one line")        \
  X(word_forward,            "word-forward",            "Move to next word start")          \
  X(word_backward,           "word-backward",           "Move to previous word start")      \
  X(word_end,                "word-end",                "Move to next word end")            \
  X(word_forward_big,        "word-forward-big",        "Move to next WORD start")          \
  X(word_backward_big,       "word-backward-big",       "Move to previous WORD start")      \
  X(word_end_big,            "word-end-big",            "Move to next WORD end")            \
  X(line_start,              "line-start",              "Move to start of line")            \
  X(line_first_non_blank,    "line-first-non-blank",    "Move to first non-blank of line")  \
  X(line_end,                "line-end",                "Move to end of line")              \
  X(file_start,              "file-start",              "Move to start of file")            \
  X(file_end,                "file-end",                "Move to end of file")              \
  X(paragraph_forward,       "paragraph-forward",       "Move to next blank line")          \
  X(paragraph_backward,      "paragraph-backward",      "Move to previous blank line")      \
  X(find_char_forward,       "find-char-forward",       "Find character forward on line")   \
  X(find_char_backward,      "find-char-backward",      "Find character backward on line")  \
  X(till_char_forward,       "till-char-forward",       "Move before next character")       \
  X(till_char_backward,      "till-char-backward",      "Move after previous character")    \
  X(matching_bracket,        "matching-bracket",        "Jump to matching bracket")         \
  /* ---- mode changes ---- */                                                              \
  X(normal_mode,             "normal-mode",             "Return to normal mode")            \
  X(insert_mode,             "insert-mode",             "Insert before cursor")             \
  X(insert_line_start,       "insert-line-start",       "Insert at first non-blank")        \
  X(append,                  "append",                  "Insert after cursor")              \
  X(append_line_end,         "append-line-end",         "Insert at end of line")            \
  X(open_line_below,         "open-line-below",         "Open a line below and insert")     \
  X(open_line_above,         "open-line-above",         "Open a line above and insert")     \
  X(visual_mode,             "visual-mode",             "Enter characterwise visual mode")  \
  X(visual_line_mode,        "visual-line-mode",        "Enter linewise visual mode")       \
  X(replace_mode,            "replace-mode",            "Enter replace mode")               \
  /* ---- operators ---- */                                                                 \
  X(operator_delete,         "operator-delete",         "Delete over a motion")             \
  X(operator_change,         "operator-change",         "Change over a motion")             \
  X(operator_yank,           "operator-yank",           "Yank over a motion")               \
  X(operator_indent,         "operator-indent",         "Indent over a motion")             \
  X(operator_dedent,         "operator-dedent",         "Dedent over a motion")             \
  /* ---- edits ---- */                                                                     \
  X(delete_char,             "delete-char",             "Delete character under cursor")    \
  X(delete_char_before,      "delete-char-before",      "Delete character before cursor")   \
  X(delete_line,             "delete-line",             "Delete current line")              \
  X(delete_to_line_end,      "delete-to-line-end",      "Delete to end of line")            \
  X(change_line,             "change-line",             "Change current line")              \
  X(change_to_line_end,      "change-to-line-end",      "Change to end of line")            \
  X(yank_line,               "yank-line",               "Yank current line")                \
  X(paste_after,             "paste-after",             "Paste after cursor")               \
  X(paste_before,            "paste-before",            "Paste before cursor")              \
  X(join_lines,              "join-lines",              "Join line below onto this one")    \
  X(indent_line,             "indent-line",             "Indent current line")              \
  X(dedent_line,             "dedent-line",             "Dedent current line")              \
  X(undo,                    "undo",                    "Undo last change")                 \
  X(redo,                    "redo",                    "Redo last undone change")          \
  X(repeat,                  "repeat",                  "Repeat last change")               \
  /* ---- insert-mode input ---- */                                                         \
  X(insert_newline,          "insert-newline",          "Insert a line break")              \
  X(insert_tab,              "insert-tab",              "Insert a tab")                     \
  X(backspace,               "backspace",               "Delete backwards")                 \
  /* ---- scrolling ---- */                                                                 \
  X(scroll_half_page_down,   "scroll-half-page-down",   "Scroll down half a screen")        \
  X(scroll_half_page_up,     "scroll-half-page-up",     "Scroll up half a screen")          \
  X(scroll_line_down,        "scroll-line-down",        "Scroll down one line")             \
  X(scroll_line_up,          "scroll-line-up",          "Scroll up one line")               \
  X(center_line,             "center-line",             "Centre cursor line on screen")     \
  /* ---- windows ---- */                                                                   \
  X(split_vertical,          "split-vertical",          "Split window vertically")          \
  X(split_horizontal,        "split-horizontal",        "Split window horizontally")        \
  X(focus_left,              "focus-left",              "Focus window to the left")         \
  X(focus_right,             "focus-right",             "Focus window to the right")        \
  X(focus_up,                "focus-up",                "Focus window above")               \
  X(focus_down,              "focus-down",              "Focus window below")               \
  X(close_window,            "close-window",            "Close the focused window")         \
  X(only_window,             "only-window",             "Close all other windows")          \
  /* ---- buffers and files ---- */                                                         \
  X(buffer_next,             "buffer-next",             "Switch to next buffer")            \
  X(buffer_prev,             "buffer-prev",             "Switch to previous buffer")        \
  X(edit_file,               "edit",                    "Open a file")                      \
  X(write_file,              "write",                   "Write the current buffer")         \
  X(write_quit,              "write-quit",              "Write and close the window")       \
  X(quit,                    "quit",                    "Close the focused window")         \
  X(quit_all,                "quit-all",                "Exit the editor")                  \
  /* ---- command line ---- */                                                              \
  X(command_line_open,       "command-line-open",       "Open the command line")            \
  X(command_line_submit,     "command-line-submit",     "Run the typed command")            \
  X(command_line_cancel,     "command-line-cancel",     "Dismiss the command line")

enum class CommandId : u16 {
  None = 0,
#define X(id, name, desc) id,
  COMMAND_LIST
#undef X
  COUNT
};

// The name typed at the `:` prompt.
[[nodiscard]] String8 CommandName(CommandId id);
[[nodiscard]] String8 CommandDescription(CommandId id);
// Returns CommandId::None when no command has that name.
[[nodiscard]] CommandId CommandIdFromName(String8 name);

// Prefix search over command names, for command-line completion.
struct CommandIdList {
  CommandId *ids;
  u64 count;
};
[[nodiscard]] CommandIdList CommandIdsWithPrefix(Arena *arena, String8 prefix);
