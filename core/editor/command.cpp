#include "editor/command.h"

#include "os/os_file.h"
#include "vim/vim_motions.h"
#include "vim/vim_operators.h"

// Every command body lives here. The X-macro list in command_id.h generates the
// enum and the table; a missing body is a link error rather than a silent gap.

namespace {

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

void EnterInsertMode(Editor *ed, View *view, Buffer *buffer, u64 at) {
  view->vim.mode = VimMode::Insert;
  view->vim.insert_start = at;
  // The whole insert session becomes one undo step.
  BufferBeginEditGroup(buffer);
  ViewSetCursor(view, buffer, at);
}

void LeaveInsertMode(Editor *ed, View *view, Buffer *buffer) {
  BufferEndEditGroup(buffer);
  view->vim.mode = VimMode::Normal;
  // Vim steps left when leaving insert, since the cursor was sitting one past
  // the last typed character.
  ViewSetCursor(view, buffer, BufferPrevCodepoint(buffer, view->cursor));
}

// Runs a motion, either moving the cursor or resolving a pending operator.
// Every motion command funnels through here, which is what makes operators and
// motions compose without per-pair code.
void RunMotion(CommandArgs *a, MotionProc proc, bool keep_column = false, u32 argument = 0) {
  Editor *ed = a->ed;
  View *view = a->view;
  Buffer *buffer = a->buffer;

  MotionResult motion = proc(buffer, view, view->cursor, a->count, argument);

  VimState *vim = &view->vim;
  bool operator_pending = (vim->mode == VimMode::OperatorPending &&
                           vim->pending_operator != OperatorKind::None);

  if (!motion.valid) {
    // A motion that could not move aborts the operator rather than acting on
    // an empty range.
    if (operator_pending) {
      vim->pending_operator = OperatorKind::None;
      vim->mode = VimMode::Normal;
    }
    return;
  }

  if (operator_pending) {
    OperatorKind op = vim->pending_operator;
    RangeU64 range = VimRangeFromMotion(buffer, view->cursor, motion);
    bool linewise = VimMotionIsLinewise(motion);

    vim->pending_operator = OperatorKind::None;
    vim->mode = VimMode::Normal;

    u64 cursor = VimApplyOperator(ed, view, buffer, op, range, linewise);

    if (op == OperatorKind::Change) {
      EnterInsertMode(ed, view, buffer, cursor);
    } else {
      ViewSetCursor(view, buffer, cursor);
    }
    return;
  }

  if (keep_column) {
    ViewSetCursorKeepColumn(view, buffer, motion.target);
  } else {
    ViewSetCursor(view, buffer, motion.target);
  }
}

// Starts an operator, or applies it immediately to a visual selection.
void BeginOperator(CommandArgs *a, OperatorKind op) {
  Editor *ed = a->ed;
  View *view = a->view;
  Buffer *buffer = a->buffer;
  VimState *vim = &view->vim;

  if (VimModeIsVisual(vim->mode)) {
    // In visual mode the range already exists, so the operator runs at once
    // rather than waiting for a motion.
    RangeU64 range = ViewSelection(view, buffer);
    bool linewise = (vim->mode == VimMode::VisualLine);

    vim->mode = VimMode::Normal;
    u64 cursor = VimApplyOperator(ed, view, buffer, op, range, linewise);

    if (op == OperatorKind::Change) {
      EnterInsertMode(ed, view, buffer, cursor);
    } else {
      ViewSetCursor(view, buffer, cursor);
    }
    return;
  }

  vim->pending_operator = op;
  vim->mode = VimMode::OperatorPending;
  // The count typed before the operator multiplies with any typed after it.
  vim->operator_count = a->has_count ? a->count : 0;
  vim->has_operator_count = a->has_count;
}

// The doubled form of an operator (dd, yy, cc) acting on whole lines.
void OperatorOnLines(CommandArgs *a, OperatorKind op) {
  View *view = a->view;
  Buffer *buffer = a->buffer;

  u64 line = ViewCursorLine(view, buffer);
  u64 last = Min(line + a->count - 1, BufferLineCount(buffer) - 1);

  RangeU64 range = RangeU64{BufferOffsetFromLine(buffer, line),
                            LineRangeWithNewline(&buffer->lines, &buffer->text, last).max};

  view->vim.pending_operator = OperatorKind::None;
  view->vim.mode = VimMode::Normal;

  u64 cursor = VimApplyOperator(a->ed, view, buffer, op, range, true);

  if (op == OperatorKind::Change) {
    EnterInsertMode(a->ed, view, buffer, cursor);
  } else {
    ViewSetCursor(view, buffer, cursor);
  }
}

void EnterVisual(CommandArgs *a, VimMode mode) {
  View *view = a->view;
  // Re-pressing the same visual key leaves visual mode, as vim does.
  if (view->vim.mode == mode) {
    view->vim.mode = VimMode::Normal;
    ViewSetCursor(view, a->buffer, view->cursor);
    return;
  }
  view->vim.visual_anchor = view->cursor;
  view->vim.mode = mode;
}

}  // namespace

// ---------------------------------------------------------------------------
// Motions
// ---------------------------------------------------------------------------

static void Cmd_cursor_left(CommandArgs *a) { RunMotion(a, MotionCharLeft); }
static void Cmd_cursor_right(CommandArgs *a) { RunMotion(a, MotionCharRight); }
// j and k keep the remembered column so travelling through short lines does
// not lose the cursor's horizontal position.
static void Cmd_cursor_up(CommandArgs *a) { RunMotion(a, MotionLineUp, true); }
static void Cmd_cursor_down(CommandArgs *a) { RunMotion(a, MotionLineDown, true); }

static void Cmd_word_forward(CommandArgs *a) { RunMotion(a, MotionWordForward); }
static void Cmd_word_backward(CommandArgs *a) { RunMotion(a, MotionWordBackward); }
static void Cmd_word_end(CommandArgs *a) { RunMotion(a, MotionWordEnd); }
static void Cmd_word_forward_big(CommandArgs *a) { RunMotion(a, MotionWordForwardBig); }
static void Cmd_word_backward_big(CommandArgs *a) { RunMotion(a, MotionWordBackwardBig); }
static void Cmd_word_end_big(CommandArgs *a) { RunMotion(a, MotionWordEndBig); }

static void Cmd_line_start(CommandArgs *a) { RunMotion(a, MotionLineStart); }
static void Cmd_line_first_non_blank(CommandArgs *a) { RunMotion(a, MotionLineFirstNonBlank); }
static void Cmd_line_end(CommandArgs *a) { RunMotion(a, MotionLineEnd); }

static void Cmd_file_start(CommandArgs *a) { RunMotion(a, MotionFileStart); }
// G reads its count as a line number, which the motion distinguishes via the
// argument rather than by inspecting editor state.
static void Cmd_file_end(CommandArgs *a) { RunMotion(a, MotionFileEnd, false, a->has_count); }

static void Cmd_paragraph_forward(CommandArgs *a) { RunMotion(a, MotionParagraphForward); }
static void Cmd_paragraph_backward(CommandArgs *a) { RunMotion(a, MotionParagraphBackward); }

static void Cmd_matching_bracket(CommandArgs *a) { RunMotion(a, MotionMatchingBracket); }

// f/F/t/T take the following keystroke as their target, which the input layer
// supplies as the triggering chord's codepoint.
static void Cmd_find_char_forward(CommandArgs *a) {
  a->view->vim.last_find_char = a->chord.codepoint;
  a->view->vim.last_find_forward = true;
  a->view->vim.last_find_till = false;
  RunMotion(a, MotionFindCharForward, false, a->chord.codepoint);
}
static void Cmd_find_char_backward(CommandArgs *a) {
  a->view->vim.last_find_char = a->chord.codepoint;
  a->view->vim.last_find_forward = false;
  a->view->vim.last_find_till = false;
  RunMotion(a, MotionFindCharBackward, false, a->chord.codepoint);
}
static void Cmd_till_char_forward(CommandArgs *a) {
  a->view->vim.last_find_char = a->chord.codepoint;
  a->view->vim.last_find_forward = true;
  a->view->vim.last_find_till = true;
  RunMotion(a, MotionTillCharForward, false, a->chord.codepoint);
}
static void Cmd_till_char_backward(CommandArgs *a) {
  a->view->vim.last_find_char = a->chord.codepoint;
  a->view->vim.last_find_forward = false;
  a->view->vim.last_find_till = true;
  RunMotion(a, MotionTillCharBackward, false, a->chord.codepoint);
}

// ---------------------------------------------------------------------------
// Modes
// ---------------------------------------------------------------------------

static void Cmd_normal_mode(CommandArgs *a) {
  View *view = a->view;
  Buffer *buffer = a->buffer;

  if (VimModeIsInsert(view->vim.mode)) {
    LeaveInsertMode(a->ed, view, buffer);
  } else {
    view->vim.mode = VimMode::Normal;
    ViewSetCursor(view, buffer, view->cursor);
  }
  VimClearPending(&view->vim);
}

static void Cmd_insert_mode(CommandArgs *a) {
  EnterInsertMode(a->ed, a->view, a->buffer, a->view->cursor);
}

static void Cmd_append(CommandArgs *a) {
  u64 at = Min(BufferNextCodepoint(a->buffer, a->view->cursor), BufferSize(a->buffer));
  EnterInsertMode(a->ed, a->view, a->buffer, at);
}

static void Cmd_insert_line_start(CommandArgs *a) {
  MotionResult m = MotionLineFirstNonBlank(a->buffer, a->view, a->view->cursor, 1, 0);
  EnterInsertMode(a->ed, a->view, a->buffer, m.target);
}

static void Cmd_append_line_end(CommandArgs *a) {
  u64 line = ViewCursorLine(a->view, a->buffer);
  EnterInsertMode(a->ed, a->view, a->buffer, BufferLineEnd(a->buffer, line));
}

static void Cmd_open_line_below(CommandArgs *a) {
  Buffer *buffer = a->buffer;
  u64 line = ViewCursorLine(a->view, buffer);
  u64 at = BufferLineEnd(buffer, line);

  EnterInsertMode(a->ed, a->view, buffer, at);
  BufferInsert(a->ed, buffer, at, Str8Lit("\n"), a->view->cursor, at + 1);
  ViewSetCursor(a->view, buffer, at + 1);
}

static void Cmd_open_line_above(CommandArgs *a) {
  Buffer *buffer = a->buffer;
  u64 line = ViewCursorLine(a->view, buffer);
  u64 at = BufferOffsetFromLine(buffer, line);

  EnterInsertMode(a->ed, a->view, buffer, at);
  BufferInsert(a->ed, buffer, at, Str8Lit("\n"), a->view->cursor, at);
  ViewSetCursor(a->view, buffer, at);
}

static void Cmd_visual_mode(CommandArgs *a) { EnterVisual(a, VimMode::Visual); }
static void Cmd_visual_line_mode(CommandArgs *a) { EnterVisual(a, VimMode::VisualLine); }

static void Cmd_replace_mode(CommandArgs *a) {
  a->view->vim.mode = VimMode::Replace;
  a->view->vim.insert_start = a->view->cursor;
  BufferBeginEditGroup(a->buffer);
}

// ---------------------------------------------------------------------------
// Operators
// ---------------------------------------------------------------------------

static void Cmd_operator_delete(CommandArgs *a) { BeginOperator(a, OperatorKind::Delete); }
static void Cmd_operator_change(CommandArgs *a) { BeginOperator(a, OperatorKind::Change); }
static void Cmd_operator_yank(CommandArgs *a) { BeginOperator(a, OperatorKind::Yank); }
static void Cmd_operator_indent(CommandArgs *a) { BeginOperator(a, OperatorKind::Indent); }
static void Cmd_operator_dedent(CommandArgs *a) { BeginOperator(a, OperatorKind::Dedent); }

// ---------------------------------------------------------------------------
// Edits
// ---------------------------------------------------------------------------

static void Cmd_delete_char(CommandArgs *a) {
  Buffer *buffer = a->buffer;
  View *view = a->view;

  if (VimModeIsVisual(view->vim.mode)) {
    BeginOperator(a, OperatorKind::Delete);
    return;
  }

  u64 line_end = BufferLineEnd(buffer, ViewCursorLine(view, buffer));
  u64 end = view->cursor;
  for (u64 i = 0; i < a->count && end < line_end; i += 1) {
    end = BufferNextCodepoint(buffer, end);
  }
  if (end == view->cursor) return;

  VimYankRange(a->ed, buffer, RangeU64{view->cursor, end}, false);
  BufferDelete(a->ed, buffer, RangeU64{view->cursor, end}, view->cursor, view->cursor);
  ViewSetCursor(view, buffer, view->cursor);
}

static void Cmd_delete_char_before(CommandArgs *a) {
  Buffer *buffer = a->buffer;
  View *view = a->view;

  u64 line_start = BufferOffsetFromLine(buffer, ViewCursorLine(view, buffer));
  u64 start = view->cursor;
  for (u64 i = 0; i < a->count && start > line_start; i += 1) {
    start = BufferPrevCodepoint(buffer, start);
  }
  if (start == view->cursor) return;

  BufferDelete(a->ed, buffer, RangeU64{start, view->cursor}, view->cursor, start);
  ViewSetCursor(view, buffer, start);
}

static void Cmd_delete_line(CommandArgs *a) { OperatorOnLines(a, OperatorKind::Delete); }
static void Cmd_change_line(CommandArgs *a) { OperatorOnLines(a, OperatorKind::Change); }
static void Cmd_yank_line(CommandArgs *a) { OperatorOnLines(a, OperatorKind::Yank); }

static void Cmd_delete_to_line_end(CommandArgs *a) {
  Buffer *buffer = a->buffer;
  View *view = a->view;
  u64 end = BufferLineEnd(buffer, ViewCursorLine(view, buffer));
  if (end <= view->cursor) return;

  VimYankRange(a->ed, buffer, RangeU64{view->cursor, end}, false);
  BufferDelete(a->ed, buffer, RangeU64{view->cursor, end}, view->cursor, view->cursor);
  ViewSetCursor(view, buffer, view->cursor);
}

static void Cmd_change_to_line_end(CommandArgs *a) {
  Buffer *buffer = a->buffer;
  View *view = a->view;
  u64 end = BufferLineEnd(buffer, ViewCursorLine(view, buffer));

  if (end > view->cursor) {
    VimYankRange(a->ed, buffer, RangeU64{view->cursor, end}, false);
    BufferDelete(a->ed, buffer, RangeU64{view->cursor, end}, view->cursor, view->cursor);
  }
  EnterInsertMode(a->ed, view, buffer, view->cursor);
}

static void Cmd_paste_after(CommandArgs *a) {
  u64 cursor = VimPaste(a->ed, a->view, a->buffer, a->view->cursor, a->count, true);
  ViewSetCursor(a->view, a->buffer, cursor);
}

static void Cmd_paste_before(CommandArgs *a) {
  u64 cursor = VimPaste(a->ed, a->view, a->buffer, a->view->cursor, a->count, false);
  ViewSetCursor(a->view, a->buffer, cursor);
}

static void Cmd_join_lines(CommandArgs *a) {
  u64 cursor = VimJoinLines(a->ed, a->view, a->buffer, a->view->cursor, a->count);
  ViewSetCursor(a->view, a->buffer, cursor);
}

// >> and << are the doubled forms, and like dd/yy/cc they must clear the
// pending operator -- otherwise the second keypress would be read as a fresh
// operator-pending motion and act twice.
static void Cmd_indent_line(CommandArgs *a) { OperatorOnLines(a, OperatorKind::Indent); }
static void Cmd_dedent_line(CommandArgs *a) { OperatorOnLines(a, OperatorKind::Dedent); }

static void Cmd_undo(CommandArgs *a) {
  for (u64 i = 0; i < a->count; i += 1) {
    bool moved = false;
    u64 cursor = BufferUndo(a->ed, a->buffer, &moved);
    if (!moved) {
      if (i == 0) EditorSetStatus(a->ed, Str8Lit("Already at oldest change"));
      break;
    }
    ViewSetCursor(a->view, a->buffer, cursor);
  }
}

static void Cmd_redo(CommandArgs *a) {
  for (u64 i = 0; i < a->count; i += 1) {
    bool moved = false;
    u64 cursor = BufferRedo(a->ed, a->buffer, &moved);
    if (!moved) {
      if (i == 0) EditorSetStatus(a->ed, Str8Lit("Already at newest change"));
      break;
    }
    ViewSetCursor(a->view, a->buffer, cursor);
  }
}

static void Cmd_repeat(CommandArgs *a) {
  Editor *ed = a->ed;
  InputState *input = &ed->input;
  if (input->last_change_count == 0 || input->replaying) return;

  // Replaying the chords that produced the last change reproduces operators,
  // counts and typed text without needing a separate representation of it.
  KeyChord chords[kMaxRecordedChords];
  u64 count = input->last_change_count;
  for (u64 i = 0; i < count; i += 1) chords[i] = input->last_change[i];

  input->replaying = true;
  for (u64 i = 0; i < count; i += 1) EditorProcessChord(ed, chords[i]);
  input->replaying = false;

  // Stop the in-progress recording, or `.` itself would be saved as the last
  // change and the next `.` would replay nothing but a `.`.
  input->recording = false;
  input->record_count = 0;
}

// ---------------------------------------------------------------------------
// Insert-mode input
// ---------------------------------------------------------------------------

static void Cmd_insert_newline(CommandArgs *a) {
  Buffer *buffer = a->buffer;
  View *view = a->view;

  if (HasFlag(buffer->flags, BufferFlags::SingleLine)) {
    // A single-line buffer treats Enter as "submit" rather than "insert".
    if (buffer->hooks.on_submit) {
      TempArena scratch = ScratchBegin();
      String8 line = BufferLineText(scratch.arena, buffer, 0);
      buffer->hooks.on_submit(a->ed, buffer, view, line);
      ScratchEnd(scratch);
    }
    return;
  }

  BufferInsert(a->ed, buffer, view->cursor, Str8Lit("\n"), view->cursor, view->cursor + 1);
  ViewSetCursor(view, buffer, view->cursor + 1);
}

static void Cmd_insert_tab(CommandArgs *a) {
  TempArena scratch = ScratchBegin();
  String8 spaces = PushStr8F(scratch.arena, "%*s", (int)kShiftWidth, "");
  BufferInsert(a->ed, a->buffer, a->view->cursor, spaces, a->view->cursor,
               a->view->cursor + spaces.size);
  ViewSetCursor(a->view, a->buffer, a->view->cursor + spaces.size);
  ScratchEnd(scratch);
}

static void Cmd_backspace(CommandArgs *a) {
  View *view = a->view;
  Buffer *buffer = a->buffer;
  if (view->cursor == 0) return;

  // Unlike normal-mode X, insert-mode backspace joins onto the previous line.
  u64 start = BufferPrevCodepoint(buffer, view->cursor);
  BufferDelete(a->ed, buffer, RangeU64{start, view->cursor}, view->cursor, start);
  ViewSetCursor(view, buffer, start);
}

// ---------------------------------------------------------------------------
// Scrolling
// ---------------------------------------------------------------------------

static void Cmd_scroll_half_page_down(CommandArgs *a) {
  ViewScrollHalfPage(a->view, a->buffer, 1, EditorPanelTextHeight(a->ed, a->ed->focused_panel));
}
static void Cmd_scroll_half_page_up(CommandArgs *a) {
  ViewScrollHalfPage(a->view, a->buffer, -1, EditorPanelTextHeight(a->ed, a->ed->focused_panel));
}
static void Cmd_scroll_line_down(CommandArgs *a) {
  ViewScrollLines(a->view, a->buffer, (i64)a->count,
                  EditorPanelTextHeight(a->ed, a->ed->focused_panel));
}
static void Cmd_scroll_line_up(CommandArgs *a) {
  ViewScrollLines(a->view, a->buffer, -(i64)a->count,
                  EditorPanelTextHeight(a->ed, a->ed->focused_panel));
}
static void Cmd_center_line(CommandArgs *a) {
  ViewCenterOnCursor(a->view, a->buffer, EditorPanelTextHeight(a->ed, a->ed->focused_panel));
}

// ---------------------------------------------------------------------------
// Windows
// ---------------------------------------------------------------------------

static void Cmd_split_vertical(CommandArgs *a) { (void)EditorSplit(a->ed, Axis2::X); }
static void Cmd_split_horizontal(CommandArgs *a) { (void)EditorSplit(a->ed, Axis2::Y); }

static void Cmd_focus_left(CommandArgs *a) { EditorFocusDir(a->ed, Dir2::Left); }
static void Cmd_focus_right(CommandArgs *a) { EditorFocusDir(a->ed, Dir2::Right); }
static void Cmd_focus_up(CommandArgs *a) { EditorFocusDir(a->ed, Dir2::Up); }
static void Cmd_focus_down(CommandArgs *a) { EditorFocusDir(a->ed, Dir2::Down); }

static void Cmd_close_window(CommandArgs *a) { EditorClosePanel(a->ed, a->ed->focused_panel); }

static void Cmd_only_window(CommandArgs *a) {
  Editor *ed = a->ed;
  // Close everything except the focused panel, re-checking each time since the
  // tree collapses as panels go.
  while (PanelLeafCount(ed->root_panel) > 1) {
    Panel *victim = PanelFirstLeaf(ed->root_panel);
    if (victim == ed->focused_panel) victim = PanelNextLeaf(ed->root_panel, victim);
    if (!victim || victim == ed->focused_panel) break;

    Panel *keep = ed->focused_panel;
    PanelClose(victim, &ed->root_panel);
    ed->focused_panel = keep;
  }
  EditorLayout(ed);
}

// ---------------------------------------------------------------------------
// Buffers and files
// ---------------------------------------------------------------------------

static void Cmd_buffer_next(CommandArgs *a) {
  BufferHandle next = BufferNextWrapping(&a->ed->buffers, a->view->buffer);
  if (next.index != 0) EditorShowBuffer(a->ed, next);
}

static void Cmd_buffer_prev(CommandArgs *a) {
  BufferHandle prev = BufferPrevWrapping(&a->ed->buffers, a->view->buffer);
  if (prev.index != 0) EditorShowBuffer(a->ed, prev);
}

static void Cmd_edit_file(CommandArgs *a) {
  if (a->text.size == 0) {
    EditorSetStatus(a->ed, Str8Lit("edit: expected a path"));
    return;
  }
  BufferHandle handle = EditorOpenFile(a->ed, a->text);
  if (handle.index == 0) {
    EditorSetStatusF(a->ed, "edit: cannot open %.*s", (int)a->text.size, (char *)a->text.str);
    return;
  }
  EditorShowBuffer(a->ed, handle);
}

static void Cmd_write_file(CommandArgs *a) {
  Buffer *buffer = a->buffer;
  String8 path = a->text;

  if (path.size == 0 && buffer->path.size == 0) {
    EditorSetStatus(a->ed, Str8Lit("write: no file name"));
    return;
  }
  if (!BufferSaveFile(buffer, path)) {
    EditorSetStatus(a->ed, Str8Lit("write: failed"));
    return;
  }
  EditorSetStatusF(a->ed, "\"%.*s\" written", (int)buffer->path.size, (char *)buffer->path.str);
}

static void Cmd_quit(CommandArgs *a) {
  // Refuse to discard unsaved work silently.
  if (BufferIsDirty(a->buffer) && a->buffer->kind == BufferKind::File) {
    EditorSetStatus(a->ed, Str8Lit("No write since last change"));
    return;
  }
  EditorClosePanel(a->ed, a->ed->focused_panel);
}

static void Cmd_write_quit(CommandArgs *a) {
  Cmd_write_file(a);
  if (!BufferIsDirty(a->buffer)) EditorClosePanel(a->ed, a->ed->focused_panel);
}

static void Cmd_quit_all(CommandArgs *a) { a->ed->quit = true; }

// ---------------------------------------------------------------------------
// Command line
// ---------------------------------------------------------------------------

static void Cmd_command_line_open(CommandArgs *a) {
  Editor *ed = a->ed;
  Buffer *buffer = BufferFromHandle(&ed->buffers, ed->command_buffer);
  if (!buffer) return;

  // Opening clears whatever was typed last time.
  BufferSetText(ed, buffer, String8{nullptr, 0});
  ed->command_line_active = true;
}

static void Cmd_command_line_submit(CommandArgs *a) {
  Editor *ed = a->ed;
  Buffer *buffer = BufferFromHandle(&ed->buffers, ed->command_buffer);
  if (!buffer) return;

  TempArena scratch = ScratchBegin();
  String8 line = BufferTextAll(scratch.arena, buffer);

  ed->command_line_active = false;
  (void)CommandExecLine(ed, line);

  ScratchEnd(scratch);
}

static void Cmd_command_line_cancel(CommandArgs *a) {
  a->ed->command_line_active = false;
}

// ---------------------------------------------------------------------------
// Table
// ---------------------------------------------------------------------------

namespace {

const Command kCommands[] = {
    {String8{nullptr, 0}, String8{nullptr, 0}, nullptr},  // CommandId::None
#define X(id, name, desc) {Str8LitComp(name), Str8LitComp(desc), Cmd_##id},
    COMMAND_LIST
#undef X
};

static_assert(ArrayCount(kCommands) == (u64)CommandId::COUNT,
              "command table must cover every CommandId");

}  // namespace

const Command *CommandFromId(CommandId id) {
  if (id == CommandId::None || id >= CommandId::COUNT) return nullptr;
  return &kCommands[(u64)id];
}

void CommandExec(Editor *ed, CommandId id, String8 text, KeyChord chord) {
  const Command *command = CommandFromId(id);
  if (!command || !command->proc) return;

  View *view = EditorFocusedView(ed);
  Buffer *buffer = EditorBufferForView(ed, view);
  if (!view || !buffer) return;

  CommandArgs args = {};
  args.ed = ed;
  args.view = view;
  args.buffer = buffer;
  args.text = text;
  args.chord = chord;

  // Resolve the vim count once, here, so no command has to reimplement the
  // "operator count times motion count, defaulting to one" rule.
  args.has_count = view->vim.has_count || view->vim.has_operator_count;
  args.count = VimEffectiveCount(&view->vim);

  // Clear the pending count before running, or a command that itself feeds
  // input back through the editor would see a stale one.
  view->vim.count = 0;
  view->vim.has_count = false;

  command->proc(&args);

  // The operator count survives only until the motion that consumes it.
  if (view->vim.mode != VimMode::OperatorPending) {
    view->vim.operator_count = 0;
    view->vim.has_operator_count = false;
  }

  EditorScrollFocusedToCursor(ed);
}

bool CommandExecLine(Editor *ed, String8 line) {
  String8 trimmed = Str8SkipChopWhitespace(line);
  if (trimmed.size == 0) return true;

  // Split into the command name and the rest, which becomes its argument.
  u64 space = Str8FindFirstChar(trimmed, ' ');
  String8 name = Str8Prefix(trimmed, space);
  String8 argument = Str8SkipChopWhitespace(Str8Skip(trimmed, space));

  CommandId id = CommandIdFromName(name);

  // Vim's single-letter shorthands, which are what people actually type.
  if (id == CommandId::None) {
    if (Str8Match(name, Str8Lit("w"))) id = CommandId::write_file;
    else if (Str8Match(name, Str8Lit("q"))) id = CommandId::quit;
    else if (Str8Match(name, Str8Lit("wq"))) id = CommandId::write_quit;
    else if (Str8Match(name, Str8Lit("qa"))) id = CommandId::quit_all;
    else if (Str8Match(name, Str8Lit("e"))) id = CommandId::edit_file;
    else if (Str8Match(name, Str8Lit("bn"))) id = CommandId::buffer_next;
    else if (Str8Match(name, Str8Lit("bp"))) id = CommandId::buffer_prev;
    else if (Str8Match(name, Str8Lit("vs"))) id = CommandId::split_vertical;
    else if (Str8Match(name, Str8Lit("sp"))) id = CommandId::split_horizontal;
  }

  if (id == CommandId::None) {
    EditorSetStatusF(ed, "Not a command: %.*s", (int)name.size, (char *)name.str);
    return false;
  }

  CommandExec(ed, id, argument);
  return true;
}
