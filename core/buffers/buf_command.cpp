#include "editor/command.h"
#include "editor/editor.h"

// The `:` command window, implemented as a buffer.
//
// This is the test of whether "everything is a buffer" actually holds. The
// command window gets no special storage, no special editing code and no
// special undo -- it is a one-line buffer with a keymap and a hook.
//
// Crucially it is a *vim* buffer like any other. Escape drops to normal mode
// and every motion, operator and text object works on the line being typed:
// `dw`, `b`, `$`, `u` all behave exactly as they do in a file. The buffer
// claims only the three keys it genuinely needs, and those live in a
// buffer-local keymap that layers on top of whichever mode map is active
// rather than replacing it.
//
// A file explorer or a grep results pane is this same shape: a kind, a
// buffer-local keymap claiming <CR>, and a payload. Modal editing comes for
// free, because nothing here opts out of it.

namespace {

bool CommandLineKey(Editor *ed, Buffer *buffer, View *view, KeyChord chord) {
  // Backspacing at the start of an empty prompt dismisses it, which is what
  // people expect from a field they have just cleared. Everything else falls
  // through to the keymaps and behaves as ordinary editing.
  if (KeyChordEqual(chord, KeyChordKey(Key::Backspace)) && BufferSize(buffer) == 0) {
    CommandExec(ed, CommandId::command_line_cancel);
    return true;
  }
  return false;
}

}  // namespace

// Installs the command-window buffer. Called once during editor setup.
BufferHandle CommandLineBufferOpen(Editor *ed) {
  BufferHandle handle = BufferOpen(&ed->buffers, BufferKind::Command, Str8Lit("[command]"));
  Buffer *buffer = BufferFromHandle(&ed->buffers, handle);
  if (!buffer) return BufferHandleZero();

  // Newlines are dropped rather than splitting the prompt in two.
  buffer->flags |= BufferFlags::SingleLine;

  // Three keys, layered over whatever mode is current. Everything else --
  // motions, operators, counts, undo, insert mode -- comes from the ordinary
  // vim maps underneath. The parent set here is a placeholder; the input layer
  // reparents onto the active mode's map on every lookup.
  Keymap *keymap = KeymapAlloc(ed->arena, ed->insert_map);
  KeymapBind(keymap, "<CR>", CommandId::command_line_submit);
  KeymapBind(keymap, "<Tab>", CommandId::command_line_complete);
  KeymapBind(keymap, "<Esc>", CommandId::command_line_escape);

  buffer->hooks.keymap = keymap;
  buffer->hooks.on_key = CommandLineKey;

  return handle;
}
