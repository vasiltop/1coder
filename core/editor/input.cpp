#include "editor/command.h"
#include "editor/editor.h"
#include "vim/vim_motions.h"

// Key input: turning chords into commands.
//
// Resolution order per press, highest priority first:
//   1. the buffer's on_key hook -- a full override, used by the command line
//   2. the buffer-local keymap
//   3. the keymap for the current vim mode
//   4. the global keymap
// Levels 2-4 are a keymap parent chain, so the lookup is one uniform walk.

namespace {

// Commands that need the *next* keystroke as an argument, rather than as a
// binding: f, F, t, T.
[[nodiscard]] bool CommandWantsCharacter(CommandId id) {
  return id == CommandId::find_char_forward || id == CommandId::find_char_backward ||
         id == CommandId::till_char_forward || id == CommandId::till_char_backward;
}

[[nodiscard]] bool ChordIsPrintable(KeyChord chord) {
  // Control and command combinations are bindings, not text.
  if (HasAny(chord.mods, KeyMod::Ctrl | KeyMod::Alt | KeyMod::Super)) return false;
  if (!KeyChordIsChar(chord)) return false;
  // Reject C0 controls; tab and newline arrive as their own bindings.
  return chord.codepoint >= 0x20 && chord.codepoint != 0x7F;
}

void ClearPending(InputState *input) {
  input->pending = nullptr;
  input->pending_map = nullptr;
  input->pending_chord_count = 0;
}

void PushPendingChord(InputState *input, KeyChord chord) {
  if (input->pending_chord_count < kMaxChordSequence) {
    input->pending_chords[input->pending_chord_count] = chord;
    input->pending_chord_count += 1;
  }
}

void RecordChord(InputState *input, KeyChord chord) {
  if (!input->recording || input->replaying) return;
  if (input->record_count >= kMaxRecordedChords) return;
  input->record[input->record_count] = chord;
  input->record_count += 1;
}

// Inserts a typed character. Not a command, because there is one of these per
// printable codepoint and binding them all would be absurd.
void InsertText(Editor *ed, View *view, Buffer *buffer, u32 codepoint) {
  if (BufferIsReadOnly(buffer)) return;

  u8 encoded[4];
  u32 length = Utf8Encode(encoded, codepoint);
  String8 text = String8{encoded, length};

  if (view->vim.mode == VimMode::Replace) {
    // Replace mode overwrites the character under the cursor instead of
    // pushing it along, but still stops at the end of the line.
    u64 line_end = BufferLineEnd(buffer, BufferLineFromOffset(buffer, view->cursor));
    u64 end = (view->cursor < line_end) ? BufferNextCodepoint(buffer, view->cursor)
                                        : view->cursor;
    BufferReplace(ed, buffer, RangeU64{view->cursor, end}, text, view->cursor,
                  view->cursor + length);
  } else {
    BufferInsert(ed, buffer, view->cursor, text, view->cursor, view->cursor + length);
  }

  ViewSetCursor(view, buffer, view->cursor + length);
}

}  // namespace

Keymap *EditorActiveKeymap(Editor *ed) {
  View *view = EditorFocusedView(ed);
  Buffer *buffer = EditorBufferForView(ed, view);
  if (!view) return ed->global_map;

  // A buffer-local map layers above whichever mode map applies.
  if (buffer && buffer->hooks.keymap) return buffer->hooks.keymap;

  // Buffers that opt out of modal editing always use the insert map, so typing
  // into the command line behaves like a text field.
  if (buffer && HasFlag(buffer->flags, BufferFlags::NoVim)) return ed->insert_map;

  switch (view->vim.mode) {
    case VimMode::Insert:
    case VimMode::Replace:
      return ed->insert_map;
    case VimMode::Visual:
    case VimMode::VisualLine:
    case VimMode::VisualBlock:
      return ed->visual_map;
    case VimMode::OperatorPending:
      return ed->operator_pending_map;
    default:
      return ed->normal_map;
  }
}

void EditorProcessChord(Editor *ed, KeyChord chord) {
  if (!KeyChordValid(chord)) return;

  View *view = EditorFocusedView(ed);
  Buffer *buffer = EditorBufferForView(ed, view);
  if (!view || !buffer) return;

  InputState *input = &ed->input;

  // Begin recording at the start of each normal-mode command, so `.` has the
  // chords that produced whatever change follows.
  if (!input->replaying && !input->pending && view->vim.mode == VimMode::Normal) {
    input->recording = true;
    input->record_count = 0;
    input->record_serial = buffer->edit_serial;
  }
  RecordChord(input, chord);

  // 1. The buffer gets first refusal, which is how the command line and later
  //    the explorer behave unmodally without the core knowing about them.
  if (buffer->hooks.on_key && buffer->hooks.on_key(ed, buffer, view, chord)) {
    return;
  }

  // 2. A command waiting on a target character takes this chord as its
  //    argument, whatever it happens to be bound to.
  if (input->awaiting_char_command != CommandId::None) {
    CommandId id = input->awaiting_char_command;
    input->awaiting_char_command = CommandId::None;

    // Escape abandons the pending f/t rather than searching for it.
    if (KeyChordIsChar(chord)) CommandExec(ed, id, String8{nullptr, 0}, chord);
    return;
  }

  KeymapLookup lookup = {};

  if (input->pending) {
    // Mid-sequence: it must complete in the map it began in.
    lookup = KeymapStep(input->pending_map, input->pending, chord);

    if (!lookup.node) {
      // A dead end abandons the sequence rather than reinterpreting the chord,
      // so a mistyped `<C-w>x` does not silently do something else.
      ClearPending(input);
      return;
    }
  } else {
    // A digit before a command is a count, not a binding -- except '0', which
    // is the start-of-line motion unless a count is already under way.
    bool modal = !VimModeIsInsert(view->vim.mode) &&
                 !HasFlag(buffer->flags, BufferFlags::NoVim);
    if (modal && KeyChordIsChar(chord) && CharIsDigit((u8)chord.codepoint)) {
      u64 digit = chord.codepoint - '0';
      if (digit != 0 || view->vim.has_count) {
        view->vim.count = view->vim.count * 10 + digit;
        view->vim.has_count = true;
        return;
      }
    }

    lookup = KeymapStep(EditorActiveKeymap(ed), nullptr, chord);
  }

  if (lookup.node) {
    if (lookup.command != CommandId::None) {
      ClearPending(input);

      if (CommandWantsCharacter(lookup.command)) {
        // f/F/t/T need the next keystroke as their target, so record the
        // command and let the following chord supply it.
        input->awaiting_char_command = lookup.command;
        return;
      }

      CommandExec(ed, lookup.command, String8{nullptr, 0}, chord);
    } else {
      // A prefix: wait for more.
      input->pending = lookup.node;
      input->pending_map = lookup.found_in;
      PushPendingChord(input, chord);
      return;
    }
  } else if (VimModeIsInsert(view->vim.mode) || HasFlag(buffer->flags, BufferFlags::NoVim)) {
    // Unbound printable keys are text.
    if (ChordIsPrintable(chord)) InsertText(ed, view, buffer, chord.codepoint);
    ClearPending(input);
  } else {
    ClearPending(input);
  }

  // Finish a recording once the buffer has changed and we are back in normal
  // mode -- that span of chords is exactly what `.` should replay.
  if (input->recording && !input->replaying && view->vim.mode == VimMode::Normal &&
      !input->pending) {
    if (buffer->edit_serial != input->record_serial) {
      for (u64 i = 0; i < input->record_count; i += 1) {
        input->last_change[i] = input->record[i];
      }
      input->last_change_count = input->record_count;
    }
    input->recording = false;
    input->record_count = 0;
  }
}

void EditorProcessSpec(Editor *ed, String8 spec) {
  TempArena scratch = ScratchBegin1(ed->arena);
  KeyChordSequence seq = KeyChordParseSequence(scratch.arena, spec);
  for (u64 i = 0; i < seq.count; i += 1) EditorProcessChord(ed, seq.chords[i]);
  ScratchEnd(scratch);
}

void EditorProcessSpec(Editor *ed, const char *spec) {
  EditorProcessSpec(ed, Str8C(spec));
}
