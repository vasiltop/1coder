#include "editor/buffer.h"

#include "os/os_file.h"

namespace {

// Buffers are numerous, so their arenas reserve modestly rather than taking the
// default gigabyte apiece. Reservations are address space only; pages commit on
// demand as a buffer actually grows.
constexpr u64 kBufferArenaReserve = MB(64);
constexpr u64 kBufferTextReserve = MB(512);

}  // namespace

void BufferInit(Buffer *buffer, BufferKind kind, String8 name) {
  buffer->kind = kind;
  buffer->flags = BufferFlags::None;
  // New files conventionally end with a newline.
  buffer->final_newline = true;

  buffer->arena = ArenaAlloc(kBufferArenaReserve);
  buffer->text_arena = ArenaAlloc(kBufferTextReserve);
  buffer->index_arena = ArenaAlloc(kBufferArenaReserve);
  buffer->undo_record_arena = ArenaAlloc(kBufferArenaReserve);
  buffer->undo_text_arena = ArenaAlloc(kBufferArenaReserve);

  buffer->name = PushStr8Copy(buffer->arena, name);
  buffer->path = String8{nullptr, 0};

  GapBufferInit(&buffer->text, buffer->text_arena);
  LineIndexInit(&buffer->lines, buffer->index_arena);
  UndoInit(&buffer->undo, buffer->undo_record_arena, buffer->undo_text_arena);

  buffer->tokens = TokenArray{nullptr, 0};
  buffer->hooks = BufferHooks{};
  buffer->user_data = nullptr;
}

void BufferDestroy(Buffer *buffer) {
  ArenaRelease(buffer->arena);
  ArenaRelease(buffer->text_arena);
  ArenaRelease(buffer->index_arena);
  ArenaRelease(buffer->undo_record_arena);
  ArenaRelease(buffer->undo_text_arena);
  *buffer = Buffer{};
}

// The single mutation path: every text change in the editor arrives here, which
// is what keeps the line index, undo stack, dirty flag and hooks in step.
void BufferReplace(Editor *ed, Buffer *buffer, RangeU64 range, String8 new_text,
                   u64 cursor_before, u64 cursor_after) {
  // Enforced here rather than in each command: this is the only way text
  // changes, so one check covers every caller.
  if (BufferIsReadOnly(buffer)) return;

  // A one-line buffer takes the text up to the first newline and drops the
  // rest, so no command -- typing, pasting, joining -- can turn it into two.
  if (HasFlag(buffer->flags, BufferFlags::SingleLine)) {
    u64 newline = Str8FindFirstChar(new_text, '\n');
    if (newline < new_text.size) new_text = Str8Prefix(new_text, newline);
  }

  u64 size = BufferSize(buffer);
  RangeU64 clamped = RangeU64{Min(range.min, size), Min(range.max, size)};
  if (clamped.max < clamped.min) clamped.max = clamped.min;

  if (RangeEmpty(clamped) && new_text.size == 0) return;

  // Snapshot the outgoing text for undo before the gap buffer forgets it.
  TempArena scratch = ScratchBegin();
  String8 old_text = GapBufferCopyRange(scratch.arena, &buffer->text, clamped);

  // The incoming text may be a view into this very buffer -- a paste of a yank
  // taken from it -- and the gap buffer cannot read its own storage while
  // writing to it, so copy first.
  String8 insert_text = PushStr8Copy(scratch.arena, new_text);

  if (!RangeEmpty(clamped)) GapBufferDelete(&buffer->text, clamped);
  if (insert_text.size) GapBufferInsert(&buffer->text, clamped.min, insert_text);

  LineIndexEdit(&buffer->lines, &buffer->text, clamped, insert_text.size);

  UndoPush(&buffer->undo, clamped, old_text, insert_text, cursor_before, cursor_after);

  buffer->flags |= BufferFlags::Dirty;
  buffer->edit_serial += 1;

  if (buffer->hooks.on_edit) {
    buffer->hooks.on_edit(ed, buffer, clamped, insert_text.size);
  }

  ScratchEnd(scratch);
}

void BufferSetText(Editor *ed, Buffer *buffer, String8 text) {
  // Loading is not an edit: it establishes the buffer's baseline, so undo
  // history starts empty rather than offering to undo the load itself.
  GapBufferClear(&buffer->text);
  if (text.size) GapBufferInsert(&buffer->text, 0, text);

  LineIndexRebuild(&buffer->lines, &buffer->text);
  UndoClear(&buffer->undo);
  buffer->flags &= ~BufferFlags::Dirty;
  buffer->edit_serial += 1;

  if (buffer->hooks.on_edit) {
    buffer->hooks.on_edit(ed, buffer, RangeU64{0, 0}, text.size);
  }
}

u64 BufferUndo(Editor *ed, Buffer *buffer, bool *moved) {
  UndoStep step = UndoStepUndo(&buffer->undo);
  if (step.count == 0) {
    if (moved) *moved = false;
    return BufferSize(buffer);
  }

  // Walk the group backwards, reverting each record by putting its old text
  // back where its new text now sits.
  for (u64 i = step.count; i > 0; i -= 1) {
    const UndoRecord *rec = &step.records[i - 1];
    RangeU64 current = RangeU64{rec->range.min, rec->range.min + rec->new_text.size};

    TempArena scratch = ScratchBegin();
    String8 old_text = PushStr8Copy(scratch.arena, rec->old_text);

    if (!RangeEmpty(current)) GapBufferDelete(&buffer->text, current);
    if (old_text.size) GapBufferInsert(&buffer->text, current.min, old_text);
    LineIndexEdit(&buffer->lines, &buffer->text, current, old_text.size);
    buffer->edit_serial += 1;

    if (buffer->hooks.on_edit) {
      buffer->hooks.on_edit(ed, buffer, current, old_text.size);
    }
    ScratchEnd(scratch);
  }

  buffer->flags |= BufferFlags::Dirty;
  if (moved) *moved = true;
  return Min(step.cursor, BufferSize(buffer));
}

u64 BufferRedo(Editor *ed, Buffer *buffer, bool *moved) {
  UndoStep step = UndoStepRedo(&buffer->undo);
  if (step.count == 0) {
    if (moved) *moved = false;
    return BufferSize(buffer);
  }

  // Forwards this time, reapplying each record.
  for (u64 i = 0; i < step.count; i += 1) {
    const UndoRecord *rec = &step.records[i];
    RangeU64 current = RangeU64{rec->range.min, rec->range.min + rec->old_text.size};

    TempArena scratch = ScratchBegin();
    String8 new_text = PushStr8Copy(scratch.arena, rec->new_text);

    if (!RangeEmpty(current)) GapBufferDelete(&buffer->text, current);
    if (new_text.size) GapBufferInsert(&buffer->text, current.min, new_text);
    LineIndexEdit(&buffer->lines, &buffer->text, current, new_text.size);
    buffer->edit_serial += 1;

    if (buffer->hooks.on_edit) {
      buffer->hooks.on_edit(ed, buffer, current, new_text.size);
    }
    ScratchEnd(scratch);
  }

  buffer->flags |= BufferFlags::Dirty;
  if (moved) *moved = true;
  return Min(step.cursor, BufferSize(buffer));
}

void BufferBeginEditGroup(Buffer *buffer) { UndoBeginGroup(&buffer->undo); }
void BufferEndEditGroup(Buffer *buffer) { UndoEndGroup(&buffer->undo); }

String8 BufferTextRange(Arena *arena, const Buffer *buffer, RangeU64 range) {
  return GapBufferCopyRange(arena, &buffer->text, range);
}

String8 BufferTextAll(Arena *arena, const Buffer *buffer) {
  return GapBufferCopyAll(arena, &buffer->text);
}

String8 BufferLineText(Arena *arena, const Buffer *buffer, u64 line) {
  return GapBufferCopyRange(arena, &buffer->text, BufferLineRange(buffer, line));
}

u64 BufferNextCodepoint(const Buffer *buffer, u64 offset) {
  u64 size = BufferSize(buffer);
  if (offset >= size) return size;

  // Step one byte, then skip any continuation bytes, so the cursor lands on a
  // character boundary rather than mid-sequence.
  u64 next = offset + 1;
  while (next < size && Utf8IsContinuation(BufferByteAt(buffer, next))) next += 1;
  return next;
}

u64 BufferPrevCodepoint(const Buffer *buffer, u64 offset) {
  if (offset == 0) return 0;

  u64 prev = offset - 1;
  while (prev > 0 && Utf8IsContinuation(BufferByteAt(buffer, prev))) prev -= 1;
  return prev;
}

DecodedCodepoint BufferDecodeAt(const Buffer *buffer, u64 offset) {
  u64 size = BufferSize(buffer);
  if (offset >= size) return DecodedCodepoint{0, 1};

  // Gather the lead byte plus any continuation bytes, then decode normally.
  u8 bytes[4] = {};
  u32 length = 1;
  bytes[0] = BufferByteAt(buffer, offset);
  while (length < 4 && offset + length < size &&
         Utf8IsContinuation(BufferByteAt(buffer, offset + length))) {
    bytes[length] = BufferByteAt(buffer, offset + length);
    length += 1;
  }

  return Utf8Decode(String8{bytes, length}, 0);
}

u64 BufferColumnFromOffset(const Buffer *buffer, u64 offset) {
  // Clamp first. A view can hold a cursor past the end after the buffer it
  // points at is replaced or truncated, and BufferNextCodepoint saturates at
  // the end -- so without this the walk below would never reach `offset`.
  u64 target = Min(offset, BufferSize(buffer));

  u64 line = BufferLineFromOffset(buffer, target);
  u64 start = BufferOffsetFromLine(buffer, line);

  u64 column = 0;
  for (u64 p = start; p < target;) {
    p = BufferNextCodepoint(buffer, p);
    column += 1;
  }
  return column;
}

u64 BufferOffsetFromColumn(const Buffer *buffer, u64 line, u64 column) {
  u64 start = BufferOffsetFromLine(buffer, line);
  u64 end = BufferLineEnd(buffer, line);

  u64 p = start;
  for (u64 i = 0; i < column && p < end; i += 1) {
    p = BufferNextCodepoint(buffer, p);
  }
  return Min(p, end);
}

bool BufferLoadFile(Editor *ed, Buffer *buffer, String8 path) {
  TempArena scratch = ScratchBegin1(buffer->arena);
  FileContents contents = OsFileRead(scratch.arena, path);

  if (!contents.ok) {
    ScratchEnd(scratch);
    return false;
  }

  // The terminating newline is metadata, not text. Keeping it out of the buffer
  // is what makes a file of one line read as one line.
  String8 text = contents.data;
  buffer->final_newline = (text.size > 0 && text.str[text.size - 1] == '\n');
  if (buffer->final_newline) text = Str8Chop(text, 1);

  BufferSetText(ed, buffer, text);
  buffer->path = PushStr8Copy(buffer->arena, path);
  buffer->name = PushStr8Copy(buffer->arena, Str8PathBase(path));

  ScratchEnd(scratch);
  return true;
}

bool BufferSaveFile(Buffer *buffer, String8 path) {
  String8 target = (path.size > 0) ? path : buffer->path;
  if (target.size == 0) return false;

  TempArena scratch = ScratchBegin1(buffer->arena);
  String8 text = BufferTextAll(scratch.arena, buffer);
  // Append the final newline when the buffer has content (Neovim's fixeol
  // always adds one) or when the original file had one.  An empty buffer that
  // came from an empty file (final_newline=false) stays empty on save; a
  // single-newline file or a scratch buffer (final_newline=true) saves as "\n".
  if (text.size > 0 || buffer->final_newline)
    text = PushStr8Cat(scratch.arena, text, Str8Lit("\n"));
  bool ok = OsFileWrite(target, text);

  if (ok) {
    // Adopt the path on a successful save-as, so the next bare write goes to
    // the same place.
    if (path.size > 0 && !Str8Match(path, buffer->path)) {
      buffer->path = PushStr8Copy(buffer->arena, path);
      buffer->name = PushStr8Copy(buffer->arena, Str8PathBase(path));
    }
    buffer->flags &= ~BufferFlags::Dirty;
  }

  ScratchEnd(scratch);
  return ok;
}
