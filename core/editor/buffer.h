#pragma once

#include "base/base_arena.h"
#include "base/base_string.h"
#include "base/base_types.h"
#include "input/keymap.h"
#include "input/keys.h"
#include "text/gap_buffer.h"
#include "text/line_index.h"
#include "text/syntax.h"
#include "text/token.h"
#include "text/undo.h"

struct Editor;
struct View;
struct Buffer;

// Everything the editor displays is a buffer: files, the `:` command line, and
// later grep results, a file explorer and a git client. They differ only in
// their kind, their hooks and the payload hanging off `user_data` -- never in
// how text is stored, edited, undone or drawn.
//
// Adding a new kind means one file under core/buffers/ providing a BufferHooks
// table. Nothing in the core needs to learn about it.

enum class BufferKind : u8 {
  File = 0,   // ordinary text file
  Scratch,    // unbacked text
  Command,    // the `:` line
  Grep,       // search results
  FileList,   // fuzzy file finder
  Explorer,   // directory tree
  Image,      // an image file, described by its header
  Git,        // git status / diff
  COUNT
};

enum class BufferFlags : u32 {
  None = 0,
  Dirty = 1 << 0,      // has unsaved changes
  ReadOnly = 1 << 1,   // rejects every edit
  SingleLine = 1 << 2, // newlines are dropped; the command window is one line
};
ENUM_FLAG_OPS(BufferFlags)

// Stale-safe reference. Buffers can be closed while something still refers to
// them, so views and commands store handles and resolve them per use rather
// than holding a pointer that could dangle.
struct BufferHandle {
  u64 index;
  u64 generation;
};

[[nodiscard]] inline bool BufferHandleEqual(BufferHandle a, BufferHandle b) {
  return a.index == b.index && a.generation == b.generation;
}
[[nodiscard]] inline BufferHandle BufferHandleZero() { return BufferHandle{0, 0}; }

// The extensibility seam. Every field is optional; a plain text file leaves all
// of them null.
struct BufferHooks {
  // Focus moved to a view showing this buffer.
  void (*on_activate)(Editor *ed, Buffer *buffer, View *view);
  // First chance at a key press. Returning true consumes it, bypassing vim --
  // this is what lets the command line and the explorer behave unmodally.
  bool (*on_key)(Editor *ed, Buffer *buffer, View *view, KeyChord chord);
  // <CR> on a line. Where a grep hit jumps to its file, or a `:` line runs.
  void (*on_submit)(Editor *ed, Buffer *buffer, View *view, String8 line);
  // Text changed. Where a syntax provider marks its tree dirty.
  void (*on_edit)(Editor *ed, Buffer *buffer, RangeU64 old_range, u64 new_len);
  // `:w` on this buffer. Returning true means the buffer wrote itself and no
  // text should go to disk -- how the explorer turns an edited listing into
  // filesystem operations instead of writing the listing over a directory.
  bool (*on_write)(Editor *ed, Buffer *buffer, View *view);
  void (*on_close)(Editor *ed, Buffer *buffer);
  // Bindings that apply only while this buffer is focused. These layer *above*
  // whichever vim mode map is active rather than replacing it, so a buffer can
  // claim a few keys -- <CR> to run a command, or to open a search hit -- while
  // every motion, operator and mode change keeps working as usual.
  Keymap *keymap;
};

struct Buffer {
  BufferHandle handle;
  BufferKind kind;
  BufferFlags flags;

  String8 name;  // shown in the status line
  String8 path;  // empty for buffers with no file

  // Whether the file ends with a newline. It is kept as a flag rather than as
  // buffer content because the bytes "a\n" are ambiguous -- one line with a
  // terminator, or two lines the second of which is empty. Vim resolves this
  // the same way, with 'endofline'. Storing the terminator as content instead
  // would add a phantom last line, shifting every line number and sending G and
  // j somewhere that does not exist.
  bool final_newline;

  GapBuffer text;
  LineIndex lines;
  UndoStack undo;
  TokenArray tokens;
  SyntaxCache syntax;

  // Bumped on every edit. Lets consumers -- dot-repeat recording, syntax
  // providers -- notice a change without diffing the text.
  u64 edit_serial;

  BufferHooks hooks;
  void *user_data;

  // The gap buffer, line index and undo stack each require sole ownership of
  // an arena so they can grow in place, hence one apiece.
  Arena *arena;
  Arena *text_arena;
  Arena *index_arena;
  Arena *undo_record_arena;
  Arena *undo_text_arena;
};

void BufferInit(Buffer *buffer, BufferKind kind, String8 name);
void BufferDestroy(Buffer *buffer);

[[nodiscard]] inline u64 BufferSize(const Buffer *buffer) { return GapBufferSize(&buffer->text); }
[[nodiscard]] inline u64 BufferLineCount(const Buffer *buffer) { return LineCount(&buffer->lines); }
[[nodiscard]] inline bool BufferIsDirty(const Buffer *buffer) {
  return HasFlag(buffer->flags, BufferFlags::Dirty);
}
[[nodiscard]] inline bool BufferIsReadOnly(const Buffer *buffer) {
  return HasFlag(buffer->flags, BufferFlags::ReadOnly);
}

// ---------------------------------------------------------------------------
// Mutation
//
// BufferReplace is the only way text changes. Insert and delete are thin
// wrappers over it. Routing everything through one primitive is what keeps the
// line index, undo history, dirty flag and hooks consistent by construction
// rather than by remembering to update them.
// ---------------------------------------------------------------------------

void BufferReplace(Editor *ed, Buffer *buffer, RangeU64 range, String8 new_text,
                   u64 cursor_before, u64 cursor_after);

inline void BufferInsert(Editor *ed, Buffer *buffer, u64 pos, String8 text,
                         u64 cursor_before, u64 cursor_after) {
  BufferReplace(ed, buffer, RangeU64{pos, pos}, text, cursor_before, cursor_after);
}

inline void BufferDelete(Editor *ed, Buffer *buffer, RangeU64 range,
                         u64 cursor_before, u64 cursor_after) {
  BufferReplace(ed, buffer, range, String8{nullptr, 0}, cursor_before, cursor_after);
}

// Replaces all text without recording undo, for loading a file into a buffer.
void BufferSetText(Editor *ed, Buffer *buffer, String8 text);

// Undo/redo. Returns the offset the cursor should move to, or the buffer size
// when there was nothing to do; `moved` reports whether anything happened.
u64 BufferUndo(Editor *ed, Buffer *buffer, bool *moved);
u64 BufferRedo(Editor *ed, Buffer *buffer, bool *moved);

// Groups every edit until the matching end into one undo step, so an entire
// insert-mode session or a multi-part command undoes at once.
void BufferBeginEditGroup(Buffer *buffer);
void BufferEndEditGroup(Buffer *buffer);

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

[[nodiscard]] inline u8 BufferByteAt(const Buffer *buffer, u64 pos) {
  return GapBufferByteAt(&buffer->text, pos);
}
[[nodiscard]] String8 BufferTextRange(Arena *arena, const Buffer *buffer, RangeU64 range);
[[nodiscard]] String8 BufferTextAll(Arena *arena, const Buffer *buffer);
[[nodiscard]] String8 BufferLineText(Arena *arena, const Buffer *buffer, u64 line);

[[nodiscard]] inline u64 BufferLineFromOffset(const Buffer *buffer, u64 offset) {
  return LineFromOffset(&buffer->lines, offset);
}
[[nodiscard]] inline u64 BufferOffsetFromLine(const Buffer *buffer, u64 line) {
  return OffsetFromLine(&buffer->lines, line);
}
[[nodiscard]] inline RangeU64 BufferLineRange(const Buffer *buffer, u64 line) {
  return LineRange(&buffer->lines, &buffer->text, line);
}
[[nodiscard]] inline u64 BufferLineEnd(const Buffer *buffer, u64 line) {
  return LineEndOffset(&buffer->lines, &buffer->text, line);
}

// Column is measured in codepoints, not bytes, so a line containing multi-byte
// characters still reports the column the cursor visually sits at.
[[nodiscard]] u64 BufferColumnFromOffset(const Buffer *buffer, u64 offset);
[[nodiscard]] u64 BufferOffsetFromColumn(const Buffer *buffer, u64 line, u64 column);

// Codepoint-aware stepping, so the cursor never lands inside a character.
[[nodiscard]] u64 BufferNextCodepoint(const Buffer *buffer, u64 offset);
[[nodiscard]] u64 BufferPrevCodepoint(const Buffer *buffer, u64 offset);

// Decodes the character at `offset`. The gap buffer's storage is not
// contiguous, so text cannot simply be decoded in place -- this gathers the
// sequence's bytes first. Past the end yields codepoint 0 with advance 1, so
// scans terminate.
[[nodiscard]] DecodedCodepoint BufferDecodeAt(const Buffer *buffer, u64 offset);

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------

[[nodiscard]] bool BufferLoadFile(Editor *ed, Buffer *buffer, String8 path);
[[nodiscard]] bool BufferSaveFile(Buffer *buffer, String8 path);
