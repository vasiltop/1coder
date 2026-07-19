#pragma once

#include "command/command_id.h"
#include "editor/editor.h"

// Command dispatch.
//
// Identity (the id, its name, what it accepts) lives in core/command; this is
// where an id becomes a call. Keybindings and the command window both arrive
// here, so there is exactly one path from "the user asked for X" to "X
// happened".

struct CommandArgs {
  Editor *ed;
  View *view;
  Buffer *buffer;

  u64 count;       // resolved vim count, always >= 1
  bool has_count;  // whether the user actually typed one

  String8 text;    // everything after the name, trimmed; empty for keybindings
  String8 arg;     // the first argument, unquoted
  bool bang;       // the "!" variant was requested

  // Line range, 0-based and inclusive, valid only when has_range. Commands that
  // do not declare CmdRange never see one.
  bool has_range;
  u64 line_first;
  u64 line_last;

  KeyChord chord;  // the chord that triggered it, for f/t and similar
};

using CommandProc = void (*)(CommandArgs *args);

struct Command {
  // The id rather than a CommandSpec pointer: resolving the spec here would
  // mean reading another translation unit's table during static
  // initialisation, whose order is unspecified.
  CommandId id;
  CommandProc proc;
};

[[nodiscard]] const Command *CommandFromId(CommandId id);

// Runs a command directly, as a keybinding does.
void CommandExec(Editor *ed, CommandId id, String8 text = String8{nullptr, 0},
                 KeyChord chord = KeyChord{});
// Runs a command with fully specified arguments, as the command window does.
void CommandExecArgs(Editor *ed, CommandId id, CommandArgs *args);

// ---------------------------------------------------------------------------
// Command line parsing
// ---------------------------------------------------------------------------

// The outcome of parsing a line, kept separate from running it so the command
// window can show errors and completions without side effects.
enum class CommandParseStatus : u8 {
  Ok = 0,
  Empty,           // nothing but whitespace
  UnknownCommand,
  AmbiguousPrefix,
  BadRange,
  UnexpectedBang,  // command does not define a forced variant
  UnexpectedRange,
  MissingArgument,
};

struct CommandParse {
  CommandParseStatus status;
  CommandId id;

  String8 name;  // the name as typed
  String8 text;
  String8 arg;
  bool bang;

  bool has_range;
  u64 line_first;
  u64 line_last;
};

// Parses without running. `buffer` supplies the current line and line count so
// that ".", "$" and "%" can be resolved; pass null to skip range resolution.
[[nodiscard]] CommandParse CommandParseLine(Arena *arena, String8 line, const Buffer *buffer,
                                            const View *view);

[[nodiscard]] String8 CommandParseStatusMessage(Arena *arena, const CommandParse *parse);

// Parses and runs. Returns false with a status message set on failure.
bool CommandExecLine(Editor *ed, String8 line);

// ---------------------------------------------------------------------------
// Completion
// ---------------------------------------------------------------------------

struct CommandCompletion {
  String8 *items;
  u64 count;
  // The span of `line` the items would replace, so the caller can splice one in
  // without re-deriving where the current word began.
  u64 replace_start;
  u64 replace_end;
};

// Completions for the cursor position at the end of `line`. Completes the
// command name while still typing it, and afterwards whatever the command's
// declared argument kind calls for.
[[nodiscard]] CommandCompletion CommandCompletionsFor(Arena *arena, Editor *ed, String8 line);
