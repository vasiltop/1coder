#pragma once

#include "base/base_arena.h"
#include "base/base_string.h"
#include "base/base_types.h"

// Command *identity*: the canonical list of every action the editor can
// perform, what each one accepts, and the name<->id mapping.
//
// Identity is separated from dispatch on purpose. The keymap binds ids and
// needs nothing else, so it can sit below the editor; the table of function
// pointers that actually runs a command lives in editor/command.cpp, where the
// Editor, View and Buffer types exist.
//
// Keybindings and the command window both resolve to these ids, so there is one
// dispatch path regardless of how an action was invoked.
//
// ---------------------------------------------------------------------------
// Command line syntax
// ---------------------------------------------------------------------------
//
//     [range] name[!] [arguments]
//
//   range      %            the whole buffer
//              .            the current line
//              $            the last line
//              N            line N (1-based, as displayed)
//              N,M          lines N through M
//              .,+3         relative to the current line
//              '<,'>        the visual selection
//   name       a command name, or any unambiguous prefix of one ("spl" ->
//              split-vertical only if nothing else starts with "spl"), or one
//              of the short aliases below
//   !          the "forced" variant, where a command defines one -- q! discards
//              unsaved changes, w! writes a read-only buffer
//   arguments  whitespace separated; wrap in double quotes to include spaces,
//              backslash escapes a quote
//
// A command declares which of these it accepts, so the parser can reject
// nonsense ("cursor-left" with a range) and the command window knows what to
// complete. Adding a command is one line here plus a body in command.cpp -- and
// a missing body is a link error, not a silent gap.

// What a command's argument means, which is what completion keys off.
enum class CommandArg : u8 {
  None = 0,     // takes no argument
  Text,          // freeform
  Path,          // completes against the filesystem
  BufferName,    // completes against open buffers
  CommandName,   // completes against the command list
  COUNT
};

enum class CommandFlags : u16 {
  None = 0,
  Bang = 1 << 0,        // accepts a trailing "!"
  Range = 1 << 1,       // accepts a line range prefix
  RequiresArg = 1 << 2, // an argument is mandatory
  Hidden = 1 << 3,      // omitted from completion (keybinding-only actions)
  // Runs once on the primary cursor even when the view has several. The default
  // is to fan out, so that motions and operators compose with multi-cursor
  // without being told about it; this marks the commands for which "once per
  // cursor" is meaningless (undo, save) or actively wrong (`.` and macro
  // replay, whose inner chords already fan out on their own).
  Single = 1 << 4,
};
ENUM_FLAG_OPS(CommandFlags)

// Short spellings for the table below, so a COMMAND_LIST line stays readable.
inline constexpr CommandFlags CmdNone = CommandFlags::None;
inline constexpr CommandFlags CmdBang = CommandFlags::Bang;
inline constexpr CommandFlags CmdRange = CommandFlags::Range;
inline constexpr CommandFlags CmdArg = CommandFlags::RequiresArg;
inline constexpr CommandFlags CmdHidden = CommandFlags::Hidden;
inline constexpr CommandFlags CmdSingle = CommandFlags::Single;

// X(identifier, "name", "description", argument kind, flags)
//
// Editing primitives are Hidden: they are bound to keys and make little sense
// typed at a prompt, but they remain callable by name so a future macro or a
// binding can reach them.
#define COMMAND_LIST                                                                                                                 \
  /* ---- motions ---- */                                                                                                            \
  X(cursor_left,           "cursor-left",           "Move cursor left",                CommandArg::None, CmdHidden)                  \
  X(cursor_right,          "cursor-right",          "Move cursor right",               CommandArg::None, CmdHidden)                  \
  X(cursor_up,             "cursor-up",             "Move cursor up one line",         CommandArg::None, CmdHidden)                  \
  X(cursor_down,           "cursor-down",           "Move cursor down one line",       CommandArg::None, CmdHidden)                  \
  X(word_forward,          "word-forward",          "Move to next word start",         CommandArg::None, CmdHidden)                  \
  X(word_backward,         "word-backward",         "Move to previous word start",     CommandArg::None, CmdHidden)                  \
  X(word_end,              "word-end",              "Move to next word end",           CommandArg::None, CmdHidden)                  \
  X(word_forward_big,      "word-forward-big",      "Move to next WORD start",         CommandArg::None, CmdHidden)                  \
  X(word_backward_big,     "word-backward-big",     "Move to previous WORD start",     CommandArg::None, CmdHidden)                  \
  X(word_end_big,          "word-end-big",          "Move to next WORD end",           CommandArg::None, CmdHidden)                  \
  X(line_start,            "line-start",            "Move to start of line",           CommandArg::None, CmdHidden)                  \
  X(line_first_non_blank,  "line-first-non-blank",  "Move to first non-blank",         CommandArg::None, CmdHidden)                  \
  X(line_first_non_blank_linewise, "line-first-non-blank-linewise", "First non-blank, linewise", CommandArg::None, CmdHidden)        \
  X(line_end,              "line-end",              "Move to end of line",             CommandArg::None, CmdHidden)                  \
  X(file_start,            "file-start",            "Move to start of file",           CommandArg::None, CmdHidden)                  \
  X(file_end,              "file-end",              "Move to end of file",             CommandArg::None, CmdHidden)                  \
  X(paragraph_forward,     "paragraph-forward",     "Move to next blank line",         CommandArg::None, CmdHidden)                  \
  X(paragraph_backward,    "paragraph-backward",    "Move to previous blank line",     CommandArg::None, CmdHidden)                  \
  X(find_char_forward,     "find-char-forward",     "Find character forward",          CommandArg::None, CmdHidden)                  \
  X(find_char_backward,    "find-char-backward",    "Find character backward",         CommandArg::None, CmdHidden)                  \
  X(till_char_forward,     "till-char-forward",     "Move before next character",      CommandArg::None, CmdHidden)                  \
  X(till_char_backward,    "till-char-backward",    "Move after previous character",   CommandArg::None, CmdHidden)                  \
  X(matching_bracket,      "matching-bracket",      "Jump to matching bracket",        CommandArg::None, CmdHidden)                  \
  X(jump_older,            "jump-older",            "Jump to older position",          CommandArg::None, CmdHidden|CmdSingle)        \
  X(jump_newer,            "jump-newer",            "Jump to newer position",          CommandArg::None, CmdHidden|CmdSingle)        \
  /* ---- mode changes ---- */                                                                                                       \
  X(normal_mode,           "normal-mode",           "Return to normal mode",           CommandArg::None, CmdHidden|CmdSingle)        \
  X(insert_mode,           "insert-mode",           "Insert before cursor",            CommandArg::None, CmdHidden)                  \
  X(insert_line_start,     "insert-line-start",     "Insert at first non-blank",       CommandArg::None, CmdHidden)                  \
  X(append,                "append",                "Insert after cursor",             CommandArg::None, CmdHidden)                  \
  X(append_line_end,       "append-line-end",       "Insert at end of line",           CommandArg::None, CmdHidden)                  \
  X(open_line_below,       "open-line-below",       "Open a line below",               CommandArg::None, CmdHidden)                  \
  X(open_line_above,       "open-line-above",       "Open a line above",               CommandArg::None, CmdHidden)                  \
  X(visual_mode,           "visual-mode",           "Enter visual mode",               CommandArg::None, CmdHidden)                  \
  X(visual_line_mode,      "visual-line-mode",      "Enter linewise visual mode",      CommandArg::None, CmdHidden)                  \
  X(replace_mode,          "replace-mode",          "Enter replace mode",              CommandArg::None, CmdHidden)                  \
  X(select_register,       "select-register",       "Choose the register to use",      CommandArg::None, CmdHidden|CmdSingle)        \
  X(text_object_inner,     "text-object-inner",     "Select the inside of an object",  CommandArg::None, CmdHidden)                  \
  X(text_object_around,    "text-object-around",    "Select an object and its edges",  CommandArg::None, CmdHidden)                  \
  X(apply_text_object,     "apply-text-object",     "Resolve a pending text object",   CommandArg::None, CmdHidden)                  \
  X(macro_record,          "macro-record",          "Start or stop recording a macro", CommandArg::None, CmdHidden|CmdSingle)        \
  X(macro_start,           "macro-start",           "Begin recording into a register", CommandArg::None, CmdHidden|CmdSingle)        \
  X(macro_play,            "macro-play",            "Replay a macro",                  CommandArg::None, CmdHidden|CmdSingle)        \
  X(macro_replay,          "macro-replay",          "Replay the named macro",          CommandArg::None, CmdHidden|CmdSingle)        \
  X(macro_repeat,          "macro-repeat",          "Replay the last macro again",     CommandArg::None, CmdHidden|CmdSingle)        \
  X(insert_register,       "insert-register",       "Insert a register's contents",    CommandArg::None, CmdHidden)                  \
  X(insert_register_prompt, "insert-register-prompt", "Ask which register to insert",   CommandArg::None, CmdHidden|CmdSingle)       \
  /* ---- in-file search ---- */                                                                                                     \
  X(search_forward,        "search",                "Search forward in the file",      CommandArg::Text, CmdArg|CmdSingle)           \
  X(search_backward,       "search-backward",       "Search backward in the file",     CommandArg::Text, CmdArg|CmdSingle)           \
  X(search_next,           "search-next",           "Jump to the next match",          CommandArg::None, CmdSingle)                  \
  X(search_prev,           "search-prev",           "Jump to the previous match",      CommandArg::None, CmdSingle)                  \
  X(search_word_forward,   "search-word",           "Search for the word under the cursor", CommandArg::None, CmdSingle)             \
  X(search_word_backward,  "search-word-backward",  "Search back for the word under the cursor", CommandArg::None, CmdSingle)        \
  X(search_clear,          "nohlsearch",            "Stop highlighting search matches", CommandArg::None, CmdSingle)                 \
  X(zoom_in,               "zoom-in",               "Increase the font size",          CommandArg::None, CmdSingle)                  \
  X(zoom_out,              "zoom-out",              "Decrease the font size",          CommandArg::None, CmdSingle)                  \
  X(zoom_reset,            "zoom-reset",            "Restore the default font size",   CommandArg::None, CmdSingle)                  \
  /* ---- operators ---- */                                                                                                          \
  X(operator_delete,       "operator-delete",       "Delete over a motion",            CommandArg::None, CmdHidden)                  \
  X(operator_change,       "operator-change",       "Change over a motion",            CommandArg::None, CmdHidden)                  \
  X(operator_yank,         "operator-yank",         "Yank over a motion",              CommandArg::None, CmdHidden)                  \
  X(operator_indent,       "operator-indent",       "Indent over a motion",            CommandArg::None, CmdHidden)                  \
  X(operator_dedent,       "operator-dedent",       "Dedent over a motion",            CommandArg::None, CmdHidden)                  \
  /* ---- edits ---- */                                                                                                              \
  X(delete_char,           "delete-char",           "Delete character under cursor",   CommandArg::None, CmdHidden)                  \
  X(delete_char_before,    "delete-char-before",    "Delete character before cursor",  CommandArg::None, CmdHidden)                  \
  X(replace_char,          "replace-char",          "Replace character under cursor",  CommandArg::None, CmdHidden)                  \
  X(toggle_case,           "toggle-case",           "Toggle case of character",        CommandArg::None, CmdHidden)                  \
  X(delete_line,           "delete",                "Delete lines",                    CommandArg::None, CmdRange)                   \
  X(delete_to_line_end,    "delete-to-line-end",    "Delete to end of line",           CommandArg::None, CmdHidden)                  \
  X(change_line,           "change-line",           "Change current line",             CommandArg::None, CmdHidden)                  \
  X(change_to_line_end,    "change-to-line-end",    "Change to end of line",           CommandArg::None, CmdHidden)                  \
  X(yank_line,             "yank",                  "Yank lines",                      CommandArg::None, CmdRange)                   \
  X(paste_after,           "put",                   "Paste after cursor",              CommandArg::None, CmdNone)                    \
  X(paste_before,          "put-before",            "Paste before cursor",             CommandArg::None, CmdNone)                    \
  X(join_lines,            "join",                  "Join lines",                      CommandArg::None, CmdRange)                   \
  X(indent_line,           "indent",                "Indent lines",                    CommandArg::None, CmdRange)                   \
  X(dedent_line,           "dedent",                "Dedent lines",                    CommandArg::None, CmdRange)                   \
  X(undo,                  "undo",                  "Undo last change",                CommandArg::None, CmdSingle)                  \
  X(redo,                  "redo",                  "Redo last undone change",         CommandArg::None, CmdSingle)                  \
  X(repeat,                "repeat",                "Repeat last change",              CommandArg::None, CmdHidden|CmdSingle)        \
  /* ---- insert-mode input ---- */                                                                                                  \
  X(insert_char,           "insert-char",           "Insert the typed character",      CommandArg::None, CmdHidden)                  \
  X(insert_newline,        "insert-newline",        "Insert a line break",             CommandArg::None, CmdHidden)                  \
  X(insert_tab,            "insert-tab",            "Insert a tab",                    CommandArg::None, CmdHidden)                  \
  X(backspace,             "backspace",             "Delete backwards",                CommandArg::None, CmdHidden)                  \
  X(delete_word_before,    "delete-word-before",    "Delete the word before the cursor", CommandArg::None, CmdHidden)                \
  /* ---- scrolling ---- */                                                                                                          \
  X(scroll_half_page_down, "scroll-half-page-down", "Scroll down half a screen",       CommandArg::None, CmdHidden|CmdSingle)        \
  X(scroll_half_page_up,   "scroll-half-page-up",   "Scroll up half a screen",         CommandArg::None, CmdHidden|CmdSingle)        \
  X(scroll_line_down,      "scroll-line-down",      "Scroll down one line",            CommandArg::None, CmdHidden|CmdSingle)        \
  X(scroll_line_up,        "scroll-line-up",        "Scroll up one line",              CommandArg::None, CmdHidden|CmdSingle)        \
  X(center_line,           "center-line",           "Centre cursor line",              CommandArg::None, CmdHidden|CmdSingle)        \
  X(goto_line,             "goto",                  "Go to a line",                    CommandArg::None, CmdRange|CmdSingle)         \
  /* ---- windows ---- */                                                                                                            \
  X(split_vertical,        "split-vertical",        "Split window vertically",         CommandArg::Path, CmdSingle)                  \
  X(split_horizontal,      "split-horizontal",      "Split window horizontally",       CommandArg::Path, CmdSingle)                  \
  X(focus_left,            "focus-left",            "Focus window to the left",        CommandArg::None, CmdHidden|CmdSingle)        \
  X(focus_right,           "focus-right",           "Focus window to the right",       CommandArg::None, CmdHidden|CmdSingle)        \
  X(focus_up,              "focus-up",              "Focus window above",              CommandArg::None, CmdHidden|CmdSingle)        \
  X(focus_down,            "focus-down",            "Focus window below",              CommandArg::None, CmdHidden|CmdSingle)        \
  X(close_window,          "close",                 "Close the focused window",        CommandArg::None, CmdBang|CmdSingle)          \
  X(only_window,           "only",                  "Close all other windows",         CommandArg::None, CmdSingle)                  \
  /* ---- buffers and files ---- */                                                                                                  \
  X(buffer_next,           "buffer-next",           "Switch to next buffer",           CommandArg::None, CmdSingle)                  \
  X(buffer_prev,           "buffer-prev",           "Switch to previous buffer",       CommandArg::None, CmdSingle)                  \
  X(buffer_switch,         "buffer",                "Switch to a named buffer",  CommandArg::BufferName, CmdArg|CmdSingle)           \
  X(edit_file,             "edit",                  "Open a file",                     CommandArg::Path, CmdBang|CmdArg|CmdSingle)   \
  X(revert,                "revert",                "Reload the file from disk, discarding unsaved changes", CommandArg::None, CmdSingle) \
  X(write_file,            "write",                 "Write the buffer",                CommandArg::Path, CmdBang|CmdRange|CmdSingle) \
  X(write_quit,            "write-quit",            "Write and close the window",      CommandArg::Path, CmdBang|CmdSingle)          \
  X(quit,                  "quit",                  "Close the focused window",        CommandArg::None, CmdBang|CmdSingle)          \
  X(quit_all,              "quit-all",              "Exit the editor",                 CommandArg::None, CmdBang|CmdSingle)          \
  X(set_cwd,               "set-cwd",               "Set the working directory",       CommandArg::Path, CmdSingle)                  \
  /* ---- search ---- */                                                                                                             \
  X(grep,                  "grep",                  "Search the project",              CommandArg::Text, CmdArg|CmdSingle)           \
  X(find_file,             "find",                  "Fuzzy-find a file",               CommandArg::Text, CmdSingle)                  \
  X(live_grep,             "live-grep",             "Search the project as you type",  CommandArg::Text, CmdSingle)                  \
  X(result_open,           "result-open",           "Open the result under the cursor", CommandArg::None, CmdHidden|CmdSingle)       \
  /* ---- explorer ---- */                                                                                                           \
  X(explorer_parent,       "explorer",              "Open the containing directory",   CommandArg::Path, CmdSingle)                  \
  X(explorer_open,         "explorer-open",         "Open the entry under the cursor", CommandArg::None, CmdHidden|CmdSingle)        \
  X(explorer_apply,        "explorer-apply",        "Apply the pending listing edits", CommandArg::None, CmdHidden|CmdSingle)        \
  /* ---- language server ---- */                                                                                                    \
  X(lsp_format,            "format",                "Format the current file",         CommandArg::None, CmdSingle)                  \
  X(lsp_implementation,    "implementation",        "Jump to implementation",          CommandArg::None, CmdSingle)                  \
  X(lsp_definition,        "definition",            "Jump to definition",              CommandArg::None, CmdSingle)                  \
  X(lsp_declaration,       "declaration",           "Jump to declaration",             CommandArg::None, CmdSingle)                  \
  X(lsp_type_definition,   "type-definition",       "Jump to type definition",         CommandArg::None, CmdSingle)                  \
  X(lsp_rename,            "rename",                "Rename symbol under cursor",      CommandArg::None, CmdSingle)                  \
  X(lsp_completion,        "completion",            "Request manual completion",       CommandArg::None, CmdSingle)                  \
  X(lsp_hover,             "hover",                 "Show hover information",          CommandArg::None, CmdSingle)                  \
  X(lsp_diagnostic_float,  "diagnostic-float",      "Show diagnostics at cursor",      CommandArg::None, CmdSingle)                  \
  /* ---- compile ---- */                                                                                                            \
  X(compile,               "compile",               "Run a shell command in the compile buffer", CommandArg::Text, CmdSingle)        \
  X(recompile,             "recompile",             "Rerun the last compile command",  CommandArg::None, CmdSingle)                  \
  X(next_error,            "next-error",            "Jump to the next compile error",  CommandArg::None, CmdSingle)                  \
  X(prev_error,            "prev-error",            "Jump to the previous compile error", CommandArg::None, CmdSingle)               \
  /* ---- git ---- */                                                                                                                \
  X(git,                   "git",                   "Open the git status buffer",      CommandArg::None, CmdSingle)                  \
  X(git_refresh,           "git-refresh",           "Reload the git status buffer",    CommandArg::None, CmdHidden|CmdSingle)        \
  X(git_stage,             "git-stage",             "Stage the file or hunk under the cursor", CommandArg::None, CmdHidden|CmdSingle) \
  X(git_unstage,           "git-unstage",           "Unstage the file or hunk under the cursor", CommandArg::None, CmdHidden|CmdSingle) \
  X(git_discard,           "git-discard",           "Discard unstaged changes under the cursor", CommandArg::None, CmdHidden|CmdSingle) \
  X(git_discard_apply,     "git-discard-apply",     "Apply a confirmed discard",       CommandArg::None, CmdHidden|CmdSingle)        \
  X(git_toggle,            "git-toggle",            "Expand or collapse hunks under the cursor", CommandArg::None, CmdHidden|CmdSingle) \
  X(git_diff,              "git-diff",              "Show the diff under the cursor",  CommandArg::None, CmdHidden|CmdSingle)        \
  X(git_open,              "git-open",              "Open the file or commit under the cursor", CommandArg::None, CmdHidden|CmdSingle) \
  X(git_log,               "git-log",               "Open the git log buffer",         CommandArg::None, CmdSingle)                  \
  X(git_commit,            "git-commit",            "Commit staged changes",           CommandArg::Text, CmdSingle)                  \
  X(git_pull,              "git-pull",              "Pull from the remote",            CommandArg::Text, CmdSingle)                  \
  X(git_push,              "git-push",              "Push to the remote",              CommandArg::Text, CmdSingle)                  \
  X(git_arg_rebase,        "git-arg-rebase",        "Toggle pull --rebase",            CommandArg::None, CmdHidden|CmdSingle)        \
  X(git_arg_autostash,     "git-arg-autostash",     "Toggle pull --autostash",         CommandArg::None, CmdHidden|CmdSingle)        \
  X(git_arg_ff_only,       "git-arg-ff-only",       "Toggle pull --ff-only",           CommandArg::None, CmdHidden|CmdSingle)        \
  X(git_arg_set_upstream,  "git-arg-set-upstream",  "Toggle push -u",                  CommandArg::None, CmdHidden|CmdSingle)        \
  /* ---- display ---- */                                                                                                            \
  X(line_numbers_absolute, "number",                "Show absolute line numbers",      CommandArg::None, CmdSingle)                  \
  X(line_numbers_relative, "relativenumber",        "Show relative line numbers",      CommandArg::None, CmdSingle)                  \
  X(line_numbers_off,      "nonumber",              "Hide line numbers",               CommandArg::None, CmdSingle)                  \
  /* ---- meta ---- */                                                                                                               \
  X(list_commands,         "commands",              "List every command",              CommandArg::None, CmdSingle)                  \
  X(list_buffers,          "buffers",               "Pick an open buffer",             CommandArg::None, CmdSingle)                  \
  X(list_bindings,         "bindings",              "List key bindings",               CommandArg::None, CmdSingle)                  \
  X(config_reload,         "config-reload",         "Reload the user config file",     CommandArg::None, CmdSingle)                  \
  X(config_error_log,      "config-error-log",      "Show config load errors",         CommandArg::None, CmdSingle)                  \
  /* ---- cursors ---- */                                                                                                           \
  X(cursor_place,          "cursor-place",          "Start placing extra cursors",     CommandArg::None, CmdSingle)                  \
  X(cursor_place_mark,     "cursor-place-mark",     "Mark this position as a cursor",  CommandArg::None, CmdHidden|CmdSingle)        \
  X(cursor_place_mark_line_end, "cursor-place-mark-line-end", "Mark the end of this line", CommandArg::None, CmdHidden|CmdSingle)  \
  X(cursor_place_confirm,  "cursor-place-confirm",  "Make the marked positions live",  CommandArg::None, CmdHidden|CmdSingle)        \
  X(cursor_place_cancel,   "cursor-place-cancel",   "Discard the marked positions",    CommandArg::None, CmdHidden|CmdSingle)        \
  /* ---- command window ---- */                                                                                                     \
  X(command_line_open,     "command-line-open",     "Open the command window",         CommandArg::None, CmdHidden|CmdSingle)        \
  X(command_line_submit,   "command-line-submit",   "Run the typed command",           CommandArg::None, CmdHidden|CmdSingle)        \
  X(command_line_cancel,   "command-line-cancel",   "Dismiss the command window",      CommandArg::None, CmdHidden|CmdSingle)        \
  X(command_line_escape,   "command-line-escape",   "Leave insert mode, then dismiss", CommandArg::None, CmdHidden|CmdSingle)        \
  X(command_line_complete, "command-line-complete", "Complete the current word",       CommandArg::None, CmdHidden|CmdSingle)

enum class CommandId : u16 {
  None = 0,
#define X(id, name, desc, arg, flags) id,
  COMMAND_LIST
#undef X
  COUNT
};

// What a command is called and what it accepts. Dispatch is separate; see
// editor/command.h.
struct CommandSpec {
  String8 name;
  String8 desc;
  CommandArg arg;
  CommandFlags flags;
};

[[nodiscard]] const CommandSpec *CommandSpecFromId(CommandId id);
[[nodiscard]] String8 CommandName(CommandId id);
[[nodiscard]] String8 CommandDescription(CommandId id);

// Exact name match only. Returns None when there is no such command.
[[nodiscard]] CommandId CommandIdFromName(String8 name);

// Full resolution as the command window does it: exact name, then a short
// alias, then an unambiguous prefix. Ambiguous prefixes resolve to None, so
// "b" does not silently pick one of several commands starting with it.
[[nodiscard]] CommandId CommandIdResolve(String8 name);

// Names that would be reachable by extending `prefix`, for completion. Hidden
// commands are omitted unless `include_hidden`.
struct CommandIdList {
  CommandId *ids;
  u64 count;
};
[[nodiscard]] CommandIdList CommandIdsWithPrefix(Arena *arena, String8 prefix,
                                                 bool include_hidden = false);
