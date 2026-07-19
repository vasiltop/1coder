#pragma once

#include "command/command_id.h"
#include "editor/editor.h"

// Command dispatch.
//
// Identity (the id and its name) lives in core/command; this is where an id
// becomes a call. Keybindings and the `:` prompt both arrive here, so there is
// exactly one path from "the user asked for X" to "X happened".

struct CommandArgs {
  Editor *ed;
  View *view;
  Buffer *buffer;

  u64 count;      // resolved count, always >= 1
  bool has_count; // whether the user actually typed one

  String8 text;   // argument text from the `:` line, empty for keybindings
  KeyChord chord; // the chord that triggered it, for f/t and similar
};

using CommandProc = void (*)(CommandArgs *args);

struct Command {
  String8 name;
  String8 desc;
  CommandProc proc;
};

[[nodiscard]] const Command *CommandFromId(CommandId id);

// Runs a command. Resolves the focused view and buffer, and applies the
// pending vim count, so callers do not each reimplement that.
void CommandExec(Editor *ed, CommandId id, String8 text = String8{nullptr, 0},
                 KeyChord chord = KeyChord{});

// Parses and runs a `:` line such as "edit src/main.cpp" or "w". Returns false
// with a status message set when the command is unknown.
bool CommandExecLine(Editor *ed, String8 line);
