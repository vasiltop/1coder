#include "editor/multicursor.h"

#include "editor/command.h"
#include "editor/editor.h"

namespace {

// The primary cursor's fields, gathered into the same shape as a secondary so
// the fan-out loop can treat every cursor alike.
[[nodiscard]] Cursor PrimaryCursor(const View *view) {
  return Cursor{view->cursor, view->preferred_column, view->vim.visual_anchor};
}

void InstallCursor(View *view, const Cursor *c) {
  view->cursor = c->offset;
  view->preferred_column = c->preferred_column;
  view->vim.visual_anchor = c->anchor;
}

[[nodiscard]] Cursor ReadBackCursor(const View *view) { return PrimaryCursor(view); }

// Shifts an offset that sits above an edit by the edit's size delta. Offsets
// below the edit are untouched.
[[nodiscard]] u64 ShiftOffset(u64 offset, i64 delta) {
  if (delta >= 0) return offset + (u64)delta;
  u64 magnitude = (u64)(-delta);
  return (offset > magnitude) ? offset - magnitude : 0;
}

}  // namespace

void ViewClearExtraCursors(View *view) {
  view->extra_count = 0;
}

bool ViewAddCursor(View *view, const Buffer *buffer, u64 offset) {
  u64 clamped = ViewClampCursorAllowLineEnd(buffer, offset);
  if (clamped == view->cursor) return true;
  for (u64 i = 0; i < view->extra_count; i += 1) {
    if (view->extras[i].offset == clamped) return true;
  }
  if (view->extra_count >= kMaxCursors) return false;

  view->extras[view->extra_count] =
      Cursor{clamped, BufferColumnFromOffset(buffer, clamped), clamped};
  view->extra_count += 1;
  return true;
}

bool ViewRemoveCursorAt(View *view, const Buffer *buffer, u64 offset) {
  // The view must keep a primary, so the last cursor cannot be taken away.
  if (view->extra_count == 0) return false;

  u64 clamped = ViewClampCursorAllowLineEnd(buffer, offset);

  if (view->cursor == clamped) {
    // Promote the first secondary rather than leaving the view headless.
    InstallCursor(view, &view->extras[0]);
    for (u64 i = 1; i < view->extra_count; i += 1) view->extras[i - 1] = view->extras[i];
    view->extra_count -= 1;
    return true;
  }

  for (u64 i = 0; i < view->extra_count; i += 1) {
    if (view->extras[i].offset != clamped) continue;
    for (u64 j = i + 1; j < view->extra_count; j += 1) view->extras[j - 1] = view->extras[j];
    view->extra_count -= 1;
    return true;
  }
  return false;
}

void MultiCursorNormalize(View *view, const Buffer *buffer) {
  if (view->extra_count == 0) {
    view->cursor = ViewClampCursorToMode(view, buffer, view->cursor);
    return;
  }

  // Gather everything into one array so the primary is ordered and merged on
  // the same terms as the rest, then find it again by the tag afterwards.
  Cursor all[kMaxCursors + 1];
  bool is_primary[kMaxCursors + 1];
  u64 count = 0;

  all[count] = PrimaryCursor(view);
  is_primary[count] = true;
  count += 1;
  for (u64 i = 0; i < view->extra_count; i += 1) {
    all[count] = view->extras[i];
    is_primary[count] = false;
    count += 1;
  }

  // An edit can leave a cursor past the end of the buffer, or on a line that
  // has since shrunk. Clamping here is what keeps the rest of the editor from
  // ever seeing an out-of-range cursor. End-of-line positions are kept, since a
  // cursor is put there deliberately and normal mode's "not past the last
  // character" rule would quietly undo the placement.
  for (u64 i = 0; i < count; i += 1) {
    all[i].offset = ViewClampCursorAllowLineEnd(buffer, all[i].offset);
  }

  // Insertion sort by offset: the count is small and this runs once per
  // keystroke, so the simple thing is the right thing.
  for (u64 i = 1; i < count; i += 1) {
    Cursor cursor = all[i];
    bool primary = is_primary[i];
    u64 j = i;
    while (j > 0 && all[j - 1].offset > cursor.offset) {
      all[j] = all[j - 1];
      is_primary[j] = is_primary[j - 1];
      j -= 1;
    }
    all[j] = cursor;
    is_primary[j] = primary;
  }

  // Merge cursors that ended up in the same place. A merged-away primary hands
  // its identity to whichever cursor absorbed it, so the view is never left
  // without one.
  u64 unique = 0;
  for (u64 i = 0; i < count; i += 1) {
    if (unique > 0 && all[unique - 1].offset == all[i].offset) {
      is_primary[unique - 1] = is_primary[unique - 1] || is_primary[i];
      continue;
    }
    all[unique] = all[i];
    is_primary[unique] = is_primary[i];
    unique += 1;
  }

  u64 primary_index = 0;
  for (u64 i = 0; i < unique; i += 1) {
    if (is_primary[i]) {
      primary_index = i;
      break;
    }
  }

  view->cursor = all[primary_index].offset;
  view->preferred_column = all[primary_index].preferred_column;
  view->vim.visual_anchor = all[primary_index].anchor;

  view->extra_count = 0;
  for (u64 i = 0; i < unique; i += 1) {
    if (i == primary_index) continue;
    view->extras[view->extra_count] = all[i];
    view->extra_count += 1;
  }
}

void MultiCursorRun(View *view, Buffer *buffer, CommandProc proc, CommandArgs *args) {
  Cursor all[kMaxCursors + 1];
  bool is_primary[kMaxCursors + 1];
  u64 count = 0;

  all[count] = PrimaryCursor(view);
  is_primary[count] = true;
  count += 1;
  for (u64 i = 0; i < view->extra_count; i += 1) {
    all[count] = view->extras[i];
    is_primary[count] = false;
    count += 1;
  }

  // Descending by offset. Visiting the highest cursor first means each edit
  // lands entirely above every cursor not yet visited, so their offsets stay
  // valid without any adjustment; only the ones already visited need shifting.
  for (u64 i = 1; i < count; i += 1) {
    Cursor cursor = all[i];
    bool primary = is_primary[i];
    u64 j = i;
    while (j > 0 && all[j - 1].offset < cursor.offset) {
      all[j] = all[j - 1];
      is_primary[j] = is_primary[j - 1];
      j -= 1;
    }
    all[j] = cursor;
    is_primary[j] = primary;
  }

  // Modal state is shared by the whole view, and a command may consume it: the
  // `w` of a `dw` clears the pending operator once it has applied it. Every
  // cursor has to see the state the command was invoked in, or the second and
  // subsequent cursors would find the operator already spent and merely move.
  // The pass therefore reinstates it before each call and keeps whatever the
  // last call left, which is the state the user should end up in.
  VimState vim_before = view->vim;

  // One undo step for the whole pass. Groups nest, so a command that groups its
  // own edits -- or an insert session already under way -- is unaffected.
  BufferBeginEditGroup(buffer);

  for (u64 i = 0; i < count; i += 1) {
    view->vim = vim_before;
    InstallCursor(view, &all[i]);

    u64 size_before = BufferSize(buffer);
    proc(args);
    i64 delta = (i64)BufferSize(buffer) - (i64)size_before;

    all[i] = ReadBackCursor(view);

    // Everything visited so far sits above this edit.
    if (delta != 0) {
      for (u64 j = 0; j < i; j += 1) {
        all[j].offset = ShiftOffset(all[j].offset, delta);
        all[j].anchor = ShiftOffset(all[j].anchor, delta);
      }
    }
  }

  BufferEndEditGroup(buffer);

  u64 primary_index = 0;
  for (u64 i = 0; i < count; i += 1) {
    if (is_primary[i]) {
      primary_index = i;
      break;
    }
  }

  InstallCursor(view, &all[primary_index]);
  view->extra_count = 0;
  for (u64 i = 0; i < count; i += 1) {
    if (i == primary_index) continue;
    view->extras[view->extra_count] = all[i];
    view->extra_count += 1;
  }

  MultiCursorNormalize(view, buffer);
}

// ---------------------------------------------------------------------------
// Placement
// ---------------------------------------------------------------------------

void ViewPlacementBegin(View *view) {
  view->placing = true;
  view->pending_count = 0;
}

namespace {

void ToggleMarkAt(View *view, u64 offset) {
  for (u64 i = 0; i < view->pending_count; i += 1) {
    if (view->pending[i] != offset) continue;
    // Marking a position twice removes it, so a misplaced mark is undone the
    // same way it was made.
    for (u64 j = i + 1; j < view->pending_count; j += 1) {
      view->pending[j - 1] = view->pending[j];
    }
    view->pending_count -= 1;
    return;
  }

  if (view->pending_count >= kMaxCursors) return;
  view->pending[view->pending_count] = offset;
  view->pending_count += 1;
}

}  // namespace

void ViewPlacementToggleMark(View *view) {
  if (!view->placing) return;
  ToggleMarkAt(view, view->cursor);
}

void ViewPlacementToggleMarkLineEnd(View *view, const Buffer *buffer) {
  if (!view->placing) return;

  // The position *after* the last character, which normal mode has no way to
  // rest on. Without this there would be no way to build the very set multiple
  // cursors are most wanted for -- the end of one line and the start of
  // another.
  ToggleMarkAt(view, BufferLineEnd(buffer, BufferLineFromOffset(buffer, view->cursor)));
}

void ViewPlacementConfirm(View *view, const Buffer *buffer) {
  if (!view->placing) return;

  // The marks are the cursor set, exactly -- including the primary, which moves
  // to the last position marked. Letting wherever the cursor happened to stop
  // become a cursor too would make unmarking a position fail to remove it.
  if (view->pending_count > 0) {
    view->cursor = ViewClampCursorAllowLineEnd(buffer, view->pending[view->pending_count - 1]);
    view->preferred_column = BufferColumnFromOffset(buffer, view->cursor);
    ViewClearExtraCursors(view);
    for (u64 i = 0; i < view->pending_count; i += 1) {
      ViewAddCursor(view, buffer, view->pending[i]);
    }
  }

  view->placing = false;
  view->pending_count = 0;
  MultiCursorNormalize(view, buffer);
}

void ViewPlacementCancel(View *view) {
  view->placing = false;
  view->pending_count = 0;
}
