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

// The system clipboard, reached through function pointers so that core stays
// free of any platform dependency. The app installs SDL's implementation; a
// test can install a fake one and exercise "+y and "+p with no window.
struct ClipboardHooks {
  String8 (*read)(Arena *arena);
  void (*write)(String8 text);
};

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

  // `"` selects the register the next yank, delete or paste should use, so the
  // chord after it names a register rather than running a command. Insert
  // mode's <C-r> uses the same wait but runs a command once the name arrives.
  bool awaiting_register;
  CommandId register_follow_up;

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
  // register that a bare p pastes from; '+' and '*' are routed to the system
  // clipboard instead of being stored here.
  Register registers[kRegisterCount];
  // One arena per register, allocated on first use and cleared on each write.
  // A single shared arena would grow without bound over a long session, since
  // overwriting a register cannot free what the previous value took.
  Arena *register_arenas[kRegisterCount];

  ClipboardHooks clipboard;

  String8 cwd;
  String8 status_message;
  Arena *status_arena;  // cleared per message, for the same reason

  // Font size in pixels. The app owns the atlas, so it watches this and
  // rebuilds when it changes -- which is how zoom stays out of the core.
  f32 font_size;
  bool font_size_changed;

  // The command window: a buffer like any other, plus its own view. It needs a
  // view of its own because it has a cursor and a scroll position that must not
  // disturb the panel underneath it.
  BufferHandle command_buffer;
  View *command_view;
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
// Where keystrokes go: the command window when it is open, otherwise the
// focused panel. Commands still act on the focused panel, so a command typed
// into the window affects the text underneath it.
[[nodiscard]] View *EditorInputView(Editor *ed);
[[nodiscard]] Buffer *EditorInputBuffer(Editor *ed);
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

// Reading or writing '+' or '*' goes to the system clipboard when the app has
// installed the hooks, and is a no-op otherwise.
void EditorSetRegister(Editor *ed, u8 name, String8 text, bool linewise);
[[nodiscard]] Register EditorGetRegister(Editor *ed, u8 name);
[[nodiscard]] inline bool RegisterIsClipboard(u8 name) { return name == '+' || name == '*'; }

// Slot 0 is the unnamed register, which vim spells `""`. Mapping the quote onto
// it means `""p` and <C-r>" reach the same place a bare `p` does.
inline constexpr u8 kRegisterUnnamed = 0;
inline constexpr u8 kRegisterYank = '0';  // vim's "0: the last yank, never a delete

[[nodiscard]] inline u8 RegisterNormalise(u32 name) {
  if (name == 0 || name == '"') return kRegisterUnnamed;
  return (u8)name;
}

// ---------------------------------------------------------------------------
// Font size
// ---------------------------------------------------------------------------

inline constexpr f32 kFontSizeDefault = 16.0f;
inline constexpr f32 kFontSizeMin = 6.0f;
inline constexpr f32 kFontSizeMax = 72.0f;

void EditorSetFontSize(Editor *ed, f32 size);

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
