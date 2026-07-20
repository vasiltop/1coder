#pragma once

#include "editor/view.h"

struct Editor;
struct CommandArgs;
using CommandProc = void (*)(CommandArgs *args);

// Several cursors in one view.
//
// The design goal is that no command knows this exists. A command is written
// against `view->cursor` -- one cursor, the ordinary case -- and multi-cursor
// is achieved by running that same command once per cursor, swapping each one
// through the primary slot for the duration of the call. Motions, operators and
// typed text therefore all fan out for free, and a command added later gets the
// behaviour without being told about it.
//
// Two rules make that safe:
//
//   Descending order. Cursors are visited highest offset first, so an edit made
//   at one cursor lies entirely above every cursor still to be visited and
//   cannot invalidate their offsets. Cursors already visited sit above the edit
//   and are shifted by its size delta.
//
//   Normalise afterwards. Cursors are clamped back into the buffer, sorted, and
//   merged when they land on the same place -- so a fan-out that collapses two
//   cursors onto one position leaves one cursor, not two coincident ones.

// Runs `proc` once per cursor, as one undo step. Only worth calling when the
// view actually has secondary cursors. Takes the proc rather than a command id
// so that a command which is itself Single -- Esc, which must decide what to
// back out of before acting -- can still fan its own tail out over the cursors.
void MultiCursorRun(View *view, Buffer *buffer, CommandProc proc, CommandArgs *args);

// Clamps every cursor into the buffer, orders them, and merges coincident ones.
// The primary survives merging: it drives scrolling, the status line, and every
// command that does not fan out, so there must always be exactly one.
void MultiCursorNormalize(View *view, const Buffer *buffer);

// Drops every secondary cursor, leaving the primary where it is.
void ViewClearExtraCursors(View *view);

// Adds a secondary cursor at `offset`. No-op when one is already there or the
// view is full. Returns false when the view is full.
bool ViewAddCursor(View *view, const Buffer *buffer, u64 offset);

// Adds a cursor at `offset` and makes it the primary, demoting the one that was
// primary to a secondary. Used by ctrl-click, so that a drag from the new cursor
// grows its own selection and leaves the others where they are.
void ViewAddCursorAsPrimary(View *view, const Buffer *buffer, u64 offset);

// Removes the cursor sitting at `offset`, if there is one. The primary can be
// removed too -- a secondary is promoted in its place -- but the last remaining
// cursor never is, since the view always has exactly one primary. Returns
// whether a cursor was found there.
bool ViewRemoveCursorAt(View *view, const Buffer *buffer, u64 offset);

[[nodiscard]] inline u64 ViewCursorCount(const View *view) { return view->extra_count + 1; }

// ---------------------------------------------------------------------------
// Placement
// ---------------------------------------------------------------------------
//
// Cursors are placed in two stages: the user marks positions using ordinary
// motions, then confirms, and only then do the marks become live cursors. The
// alternative -- cursors appearing the instant a key is pressed -- would need a
// dedicated key per way of choosing a position; staging it means the whole
// motion vocabulary is available for aiming.

void ViewPlacementBegin(View *view);
// Marks the current cursor position, or unmarks it if already marked.
void ViewPlacementToggleMark(View *view);
// Marks the position just past the current line's last character, which no
// motion can rest on in normal mode.
void ViewPlacementToggleMarkLineEnd(View *view, const Buffer *buffer);
// Turns the marks into live cursors and leaves placement.
void ViewPlacementConfirm(View *view, const Buffer *buffer);
// Abandons the marks and leaves placement.
void ViewPlacementCancel(View *view);
