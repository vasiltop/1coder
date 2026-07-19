#pragma once

#include "base/base_arena.h"
#include "editor/buffer_registry.h"
#include "editor/panel.h"
#include "editor/view.h"
#include "input/keymap.h"
#include "vim/vim_state.h"

// Top-level editor state. Holds no window, no renderer and no SDL handles --
// those live in the app layer and only ever read from here. That is what lets a
// test construct an Editor, feed it key chords and assert on the result.

inline constexpr u64 kMaxRecordedChords = 128;

// Where a partly-typed chord sequence has got to, plus the machinery for
// repeating the last change with `.`.
struct InputState {
  KeymapNode *pending;   // null when no sequence is in progress
  Keymap *pending_map;   // the map the sequence began in; it must finish there
  KeyChord pending_chords[kMaxChordSequence];
  u64 pending_chord_count;

  // f/F/t/T take the next keystroke as their target rather than as a binding,
  // so the command waits here for one chord.
  CommandId awaiting_char_command;

  // `.` replays the chords that produced the last change, which is enough to
  // reproduce operators, counts and inserted text without a separate
  // representation of "what changed".
  KeyChord record[kMaxRecordedChords];
  u64 record_count;
  bool recording;
  // Edit serial when recording began. The change may land on any chord in the
  // sequence -- the text edit of `iX<Esc>` happens on `X`, not on `<Esc>` --
  // so completion is judged against the start, not the previous chord.
  u64 record_serial;

  KeyChord last_change[kMaxRecordedChords];
  u64 last_change_count;

  bool replaying;  // guards against `.` recording itself
};

struct Editor {
  Arena *arena;

  BufferRegistry buffers;

  Panel *root_panel;
  Panel *focused_panel;

  // Keymaps, chained: buffer-local -> mode -> global.
  Keymap *global_map;
  Keymap *normal_map;
  Keymap *insert_map;
  Keymap *visual_map;
  Keymap *operator_pending_map;

  InputState input;

  // Yank and delete registers, indexed by ASCII name. Slot 0 is the unnamed
  // register that a bare p pastes from.
  Register registers[kRegisterCount];

  String8 cwd;
  String8 status_message;

  // The `:` line, which is a buffer like any other.
  BufferHandle command_buffer;
  bool command_line_active;

  RectS32 screen;  // whole window, in cells
  bool quit;
};

void EditorInit(Editor *ed, Arena *arena, RectS32 screen);
void EditorDestroy(Editor *ed);

// Recomputes every panel rect. Call after a split, a close or a window resize.
void EditorLayout(Editor *ed);
void EditorSetScreen(Editor *ed, RectS32 screen);

[[nodiscard]] View *EditorFocusedView(Editor *ed);
[[nodiscard]] Buffer *EditorFocusedBuffer(Editor *ed);
[[nodiscard]] Buffer *EditorBufferForView(Editor *ed, View *view);

// A panel's rect includes its status line; text is drawn in this sub-rect.
[[nodiscard]] RectS32 EditorPanelTextRect(const Editor *ed, const Panel *panel);
[[nodiscard]] i32 EditorPanelTextWidth(const Editor *ed, const Panel *panel);
[[nodiscard]] i32 EditorPanelTextHeight(const Editor *ed, const Panel *panel);

// Brings the focused view's cursor into sight. Called after anything that moves
// the cursor or changes the text.
void EditorScrollFocusedToCursor(Editor *ed);

// Splitting clones the focused view, so the new window opens on the same buffer
// at the same place, as vim does.
Panel *EditorSplit(Editor *ed, Axis2 axis);
void EditorClosePanel(Editor *ed, Panel *panel);
void EditorFocusPanel(Editor *ed, Panel *panel);
void EditorFocusDir(Editor *ed, Dir2 dir);

// Opens a file, reusing an existing buffer for the same path.
BufferHandle EditorOpenFile(Editor *ed, String8 path);
void EditorShowBuffer(Editor *ed, BufferHandle buffer);

void EditorSetStatus(Editor *ed, String8 message);
void EditorSetStatusF(Editor *ed, const char *fmt, ...) PrintfFormat(2, 3);

// ---------------------------------------------------------------------------
// Registers
// ---------------------------------------------------------------------------

void EditorSetRegister(Editor *ed, u8 name, String8 text, bool linewise);
[[nodiscard]] Register EditorGetRegister(Editor *ed, u8 name);

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

// The single entry point for key input. The platform layer calls it once per
// press; tests call it to replay a sequence.
void EditorProcessChord(Editor *ed, KeyChord chord);
// Convenience for tests and for `.`: feeds a binding spec chord by chord.
void EditorProcessSpec(Editor *ed, String8 spec);
void EditorProcessSpec(Editor *ed, const char *spec);

// The keymap consulted for the focused view's current mode.
[[nodiscard]] Keymap *EditorActiveKeymap(Editor *ed);

// Installs the default vim bindings. Called by EditorInit; exposed so a test
// can build an editor with a bare keymap instead.
void EditorInstallDefaultBindings(Editor *ed);
