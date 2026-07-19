#include "editor/command.h"
#include "editor/editor.h"

// The `:` command line, implemented as a buffer.
//
// This is the test of whether "everything is a buffer" actually holds. The
// command line gets no special storage, no special editing code and no special
// undo -- it is a one-line buffer with three hooks. Everything a file buffer
// can do, it does; everything it needs beyond that is expressed here.
//
// A file explorer or a git client would be this same shape: a kind, a hooks
// table, and a payload.

namespace {

// Enter runs the line. The command-line buffer sets BufferFlags::SingleLine, so
// insert_newline routes here instead of inserting a break.
void CommandLineSubmit(Editor *ed, Buffer *buffer, View *view, String8 line) {
  ed->command_line_active = false;

  TempArena scratch = ScratchBegin();
  String8 copy = PushStr8Copy(scratch.arena, line);
  BufferSetText(ed, buffer, String8{nullptr, 0});
  (void)CommandExecLine(ed, copy);
  ScratchEnd(scratch);
}

bool CommandLineKey(Editor *ed, Buffer *buffer, View *view, KeyChord chord) {
  // Escape dismisses without running, whatever the keymaps say.
  if (KeyChordEqual(chord, KeyChordKey(Key::Escape))) {
    ed->command_line_active = false;
    BufferSetText(ed, buffer, String8{nullptr, 0});
    return true;
  }

  // Backspace on an empty line dismisses too, which is what people expect from
  // a prompt they have just cleared.
  if (KeyChordEqual(chord, KeyChordKey(Key::Backspace)) && BufferSize(buffer) == 0) {
    ed->command_line_active = false;
    return true;
  }

  // Tab completes rather than inserting whitespace.
  if (KeyChordEqual(chord, KeyChordKey(Key::Tab))) {
    CommandExec(ed, CommandId::command_line_complete);
    return true;
  }

  // Enter submits. Handled here rather than through insert-newline because the
  // hook has the command buffer in hand, whereas a command would resolve to the
  // focused buffer -- the one the typed command is about to act on.
  if (KeyChordEqual(chord, KeyChordKey(Key::Return))) {
    TempArena scratch = ScratchBegin();
    String8 line = BufferTextAll(scratch.arena, buffer);
    CommandLineSubmit(ed, buffer, view, line);
    ScratchEnd(scratch);
    return true;
  }

  return false;  // everything else is ordinary text editing
}

}  // namespace

// Installs the command-line buffer. Called once during editor setup.
BufferHandle CommandLineBufferOpen(Editor *ed) {
  BufferHandle handle = BufferOpen(&ed->buffers, BufferKind::Command, Str8Lit("[command]"));
  Buffer *buffer = BufferFromHandle(&ed->buffers, handle);
  if (!buffer) return BufferHandleZero();

  // No modal editing, and a line break means "run this" rather than "insert".
  buffer->flags |= BufferFlags::NoVim | BufferFlags::SingleLine;

  buffer->hooks.on_submit = CommandLineSubmit;
  buffer->hooks.on_key = CommandLineKey;

  return handle;
}
