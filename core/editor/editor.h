#pragma once

#include "base/base_arena.h"
#include "editor/buffer_registry.h"
#include "editor/filetype.h"
#include "editor/panel.h"
#include "editor/view.h"
#include "input/keymap.h"
#include "vim/vim_state.h"

// Top-level editor state. Holds no window, no renderer and no SDL handles --
// those live in the app layer and only ever read from here. That is what lets a
// test construct an Editor, feed it key chords and assert on the result.

inline constexpr u64 kMaxRecordedChords = 128;

// What the gutter counts. Relative shows each line's distance from the cursor,
// with 0 on the cursor's own line, which is what makes a jump count something
// you read rather than work out.
//
// There is no config file, so this is a source-level setting: change
// kLineNumberModeDefault and rebuild. :number, :relativenumber and :nonumber
// switch it while running.
enum class LineNumberMode : u8 { Off, Absolute, Relative };

inline constexpr LineNumberMode kLineNumberModeDefault = LineNumberMode::Relative;
// A floor rather than a fixed size, as vim's 'numberwidth' is: the gutter grows
// for a buffer with more lines than this many digits.
inline constexpr i32 kLineNumberMinDigits = 3;

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

  // `i`/`a` wait for the character naming the object -- `iw`, `a"`, `i(`.
  bool awaiting_text_object;
  bool text_object_inner;

  // A buffer asked a yes/no question on the command line and owns the next
  // keystroke. Only `y` proceeds; every other key cancels, so a mistyped
  // confirmation can never be the destructive answer.
  bool awaiting_confirm;
  CommandId confirm_command;
  BufferHandle confirm_buffer;

  // Macro recording. Chords are stored as a binding spec in a register, so a
  // macro is just text: it can be pasted, edited and yanked back like any
  // other register, exactly as vim does it.
  bool recording_macro;
  u8 macro_register;
  KeyChord macro_chords[kMaxRecordedChords];
  u64 macro_count;
  u8 last_macro_register;
  bool replaying_macro;
  // A count on `@` is typed before the register name, so it has to be held
  // across the wait for that name.
  u64 macro_count_pending;

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

  // What <CR> on a path opens. Populated in EditorInit.
  FiletypeRegistry filetypes;

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
  // What the prompt is for: ':' runs a command, '/' and '?' search. The
  // character is also what the renderer draws in front of the typed text.
  u8 command_line_prompt;

  // In-file search. The pattern is editor-wide rather than per-view, as vim's
  // is: `/` in one window sets what `n` finds in every other.
  String8 search_pattern;
  Arena *search_arena;  // cleared per pattern, like the register arenas
  bool search_forward;
  bool search_highlight;  // matches are painted until :noh

  // Which numbers the gutter shows. Editor-wide rather than per-view: with no
  // :set there would be no way to give one window a different answer.
  LineNumberMode line_number_mode;

  // Where the cursor sat when the search prompt opened. Incremental search
  // moves the cursor as the pattern is typed, so cancelling has to put it back.
  View *search_origin_view;
  u64 search_origin;
  u64 search_origin_scroll;

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

// A panel's rect includes its status line and its gutter; text is drawn in this
// sub-rect. Subtracting the gutter here rather than in the renderer is what
// keeps horizontal scrolling honest -- ViewScrollToCursor is fed this width.
[[nodiscard]] RectS32 EditorPanelTextRect(Editor *ed, const Panel *panel);
[[nodiscard]] i32 EditorPanelTextWidth(Editor *ed, const Panel *panel);
[[nodiscard]] i32 EditorPanelTextHeight(Editor *ed, const Panel *panel);

// Columns the gutter occupies in this panel, 0 when line numbers are off. Wide
// enough for the buffer's largest line number, floored at kLineNumberMinDigits,
// plus one blank column separating the numbers from the text.
[[nodiscard]] i32 EditorGutterWidth(Editor *ed, const Panel *panel);

// The number `line` displays: its distance from the cursor when relative (0 on
// the cursor's own line), otherwise the 1-based line number.
[[nodiscard]] u64 EditorLineNumberLabel(const Editor *ed, const View *view,
                                        const Buffer *buffer, u64 line);

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

// ---------------------------------------------------------------------------
// Jump list (<C-o> / <C-i>)
// ---------------------------------------------------------------------------

// Records the view's current buffer and cursor as a jump origin. No-op for the
// command window. Call before a jumpy motion that will actually move.
void EditorPushJump(Editor *ed, View *view);

// Switches to `entry`'s buffer (without resetting the view the way
// EditorShowBuffer does) and places the cursor at its offset.
void EditorJumpTo(Editor *ed, View *view, JumpEntry entry);

// Walk the per-view jump list `count` steps. Returns false when nothing moved.
[[nodiscard]] bool EditorJumpOlder(Editor *ed, View *view, u64 count);
[[nodiscard]] bool EditorJumpNewer(Editor *ed, View *view, u64 count);

// ---------------------------------------------------------------------------
// Confirmation
// ---------------------------------------------------------------------------

// Hands the next keystroke to a yes/no question. `y` runs `command` with
// `buffer` focused; anything else cancels. The caller sets the status message
// asking the question -- this only arranges for the answer.
void EditorAwaitConfirm(Editor *ed, CommandId command, BufferHandle buffer);

// ---------------------------------------------------------------------------
// Reacting to files moving underneath us
// ---------------------------------------------------------------------------

// After a rename on disk, points every open buffer at or under `old_path` to
// the matching location under `new_path`. Text is untouched, so unsaved edits
// survive a rename rather than being written back to a path that is gone.
// Matching on the directory prefix too is what makes renaming a folder carry
// the buffers inside it.
void EditorRetargetBufferPaths(Editor *ed, String8 old_path, String8 new_path);

// After a delete, marks any buffer at or under `path` dirty and leaves its text
// alone, so the content is still recoverable with `:w`. Closing the buffer
// would be the one outcome that loses work with no way back.
void EditorOrphanBufferPaths(Editor *ed, String8 path);

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
