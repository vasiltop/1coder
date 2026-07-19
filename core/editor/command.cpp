#include "editor/command.h"

#include "os/os_file.h"
#include "vim/vim_motions.h"
#include "vim/vim_operators.h"
#include "vim/vim_search.h"

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

// The doubled form of an operator (dd, yy, cc) acting on whole lines. Also the
// entry point for the range forms typed at the command window, where the range
// names the lines outright instead of counting from the cursor.
void OperatorOnLines(CommandArgs *a, OperatorKind op) {
  View *view = a->view;
  Buffer *buffer = a->buffer;

  u64 line = a->has_range ? Min(a->line_first, BufferLineCount(buffer) - 1)
                          : ViewCursorLine(view, buffer);
  u64 last = a->has_range ? Min(a->line_last, BufferLineCount(buffer) - 1)
                          : Min(line + a->count - 1, BufferLineCount(buffer) - 1);

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
static void Cmd_line_first_non_blank_linewise(CommandArgs *a) {
  RunMotion(a, MotionFirstNonBlankLinewise);
}
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

// ---------------------------------------------------------------------------
// Text objects
//
// A text object names a *thing* rather than a direction, so `ciw` changes the
// word the cursor is standing in wherever in it that happens to be. They yield
// a range directly, which is the same currency motions are converted into --
// so the operator machinery below needs no special case for them.
// ---------------------------------------------------------------------------

namespace {

[[nodiscard]] TextObjectResult ResolveTextObject(const Buffer *buffer, u64 pos, u64 count,
                                                 bool inner, u32 object) {
  switch (object) {
    case 'w': return TextObjectWord(buffer, pos, count, inner, false);
    case 'W': return TextObjectWord(buffer, pos, count, inner, true);
    case 'p': return TextObjectParagraph(buffer, pos, inner);

    case '"':
    case '\'':
    case '`':
      return TextObjectQuoted(buffer, pos, (u8)object, inner);

    // vim spells the paren pair three ways: either bracket, or `b`.
    case '(':
    case ')':
    case 'b':
      return TextObjectDelimited(buffer, pos, '(', ')', inner);
    case '{':
    case '}':
    case 'B':
      return TextObjectDelimited(buffer, pos, '{', '}', inner);
    case '[':
    case ']':
      return TextObjectDelimited(buffer, pos, '[', ']', inner);
    case '<':
    case '>':
      return TextObjectDelimited(buffer, pos, '<', '>', inner);

    default:
      return TextObjectResult{RangeU64{pos, pos}, false, false};
  }
}

}  // namespace

static void Cmd_text_object_inner(CommandArgs *a) {
  a->ed->input.awaiting_text_object = true;
  a->ed->input.text_object_inner = true;
}

static void Cmd_text_object_around(CommandArgs *a) {
  a->ed->input.awaiting_text_object = true;
  a->ed->input.text_object_inner = false;
}

static void Cmd_apply_text_object(CommandArgs *a) {
  View *view = a->view;
  Buffer *buffer = a->buffer;
  VimState *vim = &view->vim;

  TextObjectResult object = ResolveTextObject(buffer, view->cursor, a->count,
                                              a->ed->input.text_object_inner,
                                              a->chord.codepoint);

  if (!object.valid) {
    // Nothing to act on, so abandon any operator rather than acting on an
    // empty range.
    vim->pending_operator = OperatorKind::None;
    if (vim->mode == VimMode::OperatorPending) vim->mode = VimMode::Normal;
    return;
  }

  // In visual mode the object becomes the selection instead of being consumed.
  if (VimModeIsVisual(vim->mode)) {
    vim->visual_anchor = object.range.min;
    u64 last = (object.range.max > object.range.min)
                   ? BufferPrevCodepoint(buffer, object.range.max)
                   : object.range.min;
    ViewSetCursor(view, buffer, last);
    return;
  }

  if (vim->mode != VimMode::OperatorPending || vim->pending_operator == OperatorKind::None) {
    return;  // `iw` alone does nothing in normal mode, as in vim
  }

  OperatorKind op = vim->pending_operator;
  vim->pending_operator = OperatorKind::None;
  vim->mode = VimMode::Normal;

  u64 cursor = VimApplyOperator(a->ed, view, buffer, op, object.range, object.linewise);

  if (op == OperatorKind::Change) {
    EnterInsertMode(a->ed, view, buffer, cursor);
  } else {
    ViewSetCursor(view, buffer, cursor);
  }
}

// ---------------------------------------------------------------------------
// Macros
//
// A recorded macro is stored as a binding spec in a register, so replaying it
// is just feeding that text back through the input layer -- and the macro can
// be pasted, edited and yanked back like any other register.
// ---------------------------------------------------------------------------

static void Cmd_macro_start(CommandArgs *a) {
  InputState *input = &a->ed->input;
  input->recording_macro = true;
  input->macro_register = RegisterNormalise(a->view->vim.pending_register);
  input->macro_count = 0;
  EditorSetStatusF(a->ed, "recording @%c", (int)input->macro_register);
}

static void Cmd_macro_record(CommandArgs *a) {
  Editor *ed = a->ed;
  InputState *input = &ed->input;

  if (input->recording_macro) {
    input->recording_macro = false;

    TempArena scratch = ScratchBegin();
    KeyChordSequence seq = {input->macro_chords, input->macro_count};
    String8 text = KeyChordSequenceToString(scratch.arena, seq);
    EditorSetRegister(ed, input->macro_register, text, false);
    ScratchEnd(scratch);

    EditorSetStatus(ed, Str8Lit(""));
    return;
  }

  // Not recording yet, so the next chord names the register to record into.
  input->awaiting_register = true;
  input->register_follow_up = CommandId::macro_start;
}

static void Cmd_macro_replay(CommandArgs *a) {
  Editor *ed = a->ed;
  u8 name = RegisterNormalise(a->view->vim.pending_register);

  // `@@` reaches here as the register named '@', because the first `@` already
  // started waiting for a name. Vim spells "the last one" the same way.
  if (name == '@') name = ed->input.last_macro_register;
  if (name == 0) return;

  Register reg = EditorGetRegister(ed, name);
  if (reg.text.size == 0 || ed->input.replaying_macro) return;

  ed->input.last_macro_register = name;

  TempArena scratch = ScratchBegin();
  String8 spec = PushStr8Copy(scratch.arena, reg.text);

  u64 repeats = Max(ed->input.macro_count_pending, (u64)1);
  ed->input.macro_count_pending = 0;

  // Release the register before replaying. It is still selected here -- the
  // caller clears it only after this returns -- so a macro containing a delete
  // or a yank would write into the very register holding the macro, replacing
  // it with the text it just captured.
  a->view->vim.pending_register = 0;

  // The guard keeps a macro that contains `@` from recursing without end.
  ed->input.replaying_macro = true;
  for (u64 i = 0; i < repeats; i += 1) EditorProcessSpec(ed, spec);
  ed->input.replaying_macro = false;

  ScratchEnd(scratch);
}

static void Cmd_macro_play(CommandArgs *a) {
  a->ed->input.awaiting_register = true;
  a->ed->input.register_follow_up = CommandId::macro_replay;
  // Held here because the register name has not been typed yet.
  a->ed->input.macro_count_pending = a->count;
}

static void Cmd_macro_repeat(CommandArgs *a) {
  a->view->vim.pending_register = a->ed->input.last_macro_register;
  a->ed->input.macro_count_pending = a->count;
  Cmd_macro_replay(a);
}

// `"` -- the chord after it names the register rather than running a command,
// which the input layer handles.
static void Cmd_select_register(CommandArgs *a) {
  a->ed->input.awaiting_register = true;
  a->ed->input.register_follow_up = CommandId::None;
}

// Insert mode's <C-r> waits the same way, but acts as soon as the name lands
// instead of storing it for a later operator.
static void Cmd_insert_register_prompt(CommandArgs *a) {
  a->ed->input.awaiting_register = true;
  a->ed->input.register_follow_up = CommandId::insert_register;
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

  VimYankRange(a->ed, view, buffer, RangeU64{view->cursor, end}, false);
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

  VimYankRange(a->ed, view, buffer, RangeU64{view->cursor, end}, false);
  BufferDelete(a->ed, buffer, RangeU64{view->cursor, end}, view->cursor, view->cursor);
  ViewSetCursor(view, buffer, view->cursor);
}

static void Cmd_change_to_line_end(CommandArgs *a) {
  Buffer *buffer = a->buffer;
  View *view = a->view;
  u64 end = BufferLineEnd(buffer, ViewCursorLine(view, buffer));

  if (end > view->cursor) {
    VimYankRange(a->ed, view, buffer, RangeU64{view->cursor, end}, false);
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

// Insert-mode word rubout. Bound to <C-w> as vim does, and to <C-h> and
// <C-BS>, which is what a terminal and a window system respectively send for
// ctrl-backspace.
static void Cmd_delete_word_before(CommandArgs *a) {
  View *view = a->view;
  Buffer *buffer = a->buffer;
  if (view->cursor == 0) return;

  u64 line_start = BufferOffsetFromLine(buffer, ViewCursorLine(view, buffer));

  // At the very start of a line there is no word to rub out, so fall back to
  // deleting the line break.
  if (view->cursor <= line_start) {
    Cmd_backspace(a);
    return;
  }

  MotionResult motion = MotionWordBackward(buffer, view, view->cursor, a->count, 0);
  u64 start = motion.valid ? Max(motion.target, line_start) : line_start;
  if (start >= view->cursor) start = line_start;

  BufferDelete(a->ed, buffer, RangeU64{start, view->cursor}, view->cursor, start);
  ViewSetCursor(view, buffer, start);
}

// <C-r>{reg} in insert mode, which is how vim pastes without leaving it.
static void Cmd_insert_register(CommandArgs *a) {
  Register reg = EditorGetRegister(a->ed, RegisterNormalise(a->view->vim.pending_register));
  if (reg.text.size == 0) return;

  TempArena scratch = ScratchBegin();
  String8 text = PushStr8Copy(scratch.arena, reg.text);
  BufferInsert(a->ed, a->buffer, a->view->cursor, text, a->view->cursor,
               a->view->cursor + text.size);
  ViewSetCursor(a->view, a->buffer, a->view->cursor + text.size);
  ScratchEnd(scratch);
}

// ---------------------------------------------------------------------------
// Zoom
//
// The core holds only a number; the app watches it and rebuilds the glyph
// atlas. Because layout is in cells, a size change resizes the grid through
// exactly the same path a window resize takes.
// ---------------------------------------------------------------------------

static void Cmd_zoom_in(CommandArgs *a) {
  EditorSetFontSize(a->ed, a->ed->font_size + (f32)a->count);
}
static void Cmd_zoom_out(CommandArgs *a) {
  EditorSetFontSize(a->ed, a->ed->font_size - (f32)a->count);
}
static void Cmd_zoom_reset(CommandArgs *a) { EditorSetFontSize(a->ed, kFontSizeDefault); }

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

// A split may name a file to open in the new window, as vim's :vsplit does.
static void SplitAndMaybeOpen(CommandArgs *a, Axis2 axis) {
  if (!EditorSplit(a->ed, axis)) return;
  if (a->arg.size == 0) return;

  BufferHandle handle = EditorOpenFile(a->ed, a->arg);
  if (handle.index != 0) EditorShowBuffer(a->ed, handle);
}

static void Cmd_split_vertical(CommandArgs *a) { SplitAndMaybeOpen(a, Axis2::X); }
static void Cmd_split_horizontal(CommandArgs *a) { SplitAndMaybeOpen(a, Axis2::Y); }

static void Cmd_focus_left(CommandArgs *a) { EditorFocusDir(a->ed, Dir2::Left); }
static void Cmd_focus_right(CommandArgs *a) { EditorFocusDir(a->ed, Dir2::Right); }
static void Cmd_focus_up(CommandArgs *a) { EditorFocusDir(a->ed, Dir2::Up); }
static void Cmd_focus_down(CommandArgs *a) { EditorFocusDir(a->ed, Dir2::Down); }

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
  // Opening over unsaved work needs the forced variant.
  if (BufferIsDirty(a->buffer) && !a->bang && a->buffer->kind == BufferKind::File) {
    EditorSetStatus(a->ed, Str8Lit("No write since last change (add ! to override)"));
    return;
  }

  BufferHandle handle = EditorOpenFile(a->ed, a->arg);
  if (handle.index == 0) {
    EditorSetStatusF(a->ed, "Cannot open %.*s", (int)a->arg.size, (char *)a->arg.str);
    return;
  }
  EditorShowBuffer(a->ed, handle);
}

static void Cmd_buffer_switch(CommandArgs *a) {
  BufferHandle handle = BufferFromName(&a->ed->buffers, a->arg);
  if (handle.index == 0) {
    EditorSetStatusF(a->ed, "No buffer named %.*s", (int)a->arg.size, (char *)a->arg.str);
    return;
  }
  EditorShowBuffer(a->ed, handle);
}

static void Cmd_goto_line(CommandArgs *a) {
  u64 line = a->has_range ? a->line_last : 0;
  ViewSetCursorLineColumn(a->view, a->buffer, line, 0);
  a->view->preferred_column = 0;
}

static void Cmd_write_file(CommandArgs *a) {
  Buffer *buffer = a->buffer;
  String8 path = a->arg;

  if (BufferIsReadOnly(buffer) && !a->bang) {
    EditorSetStatus(a->ed, Str8Lit("Buffer is read-only (add ! to override)"));
    return;
  }

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
  // Refuse to discard unsaved work unless explicitly forced.
  if (BufferIsDirty(a->buffer) && !a->bang && a->buffer->kind == BufferKind::File) {
    EditorSetStatus(a->ed, Str8Lit("No write since last change (add ! to override)"));
    return;
  }
  EditorClosePanel(a->ed, a->ed->focused_panel);
}

static void Cmd_close_window(CommandArgs *a) { Cmd_quit(a); }

static void Cmd_write_quit(CommandArgs *a) {
  Cmd_write_file(a);
  if (!BufferIsDirty(a->buffer)) EditorClosePanel(a->ed, a->ed->focused_panel);
}

static void Cmd_quit_all(CommandArgs *a) {
  if (!a->bang) {
    // Check every buffer, not just the focused one.
    for (BufferHandle h = BufferFirst(&a->ed->buffers); h.index != 0;
         h = BufferNext(&a->ed->buffers, h)) {
      Buffer *b = BufferFromHandle(&a->ed->buffers, h);
      if (b && BufferIsDirty(b) && b->kind == BufferKind::File) {
        EditorSetStatusF(a->ed, "No write since last change for %.*s (add ! to override)",
                         (int)b->name.size, (char *)b->name.str);
        return;
      }
    }
  }
  a->ed->quit = true;
}

// ---------------------------------------------------------------------------
// In-file search
// ---------------------------------------------------------------------------
//
// `/` and `?` open the ordinary command window with a different prompt
// character, so search inherits the whole editing model: Esc drops to normal
// mode, `dw` and `b` work on the pattern being typed, and the line is a buffer
// like any other. Only the submit path differs.

void EditorSetSearchPattern(Editor *ed, String8 pattern, bool forward) {
  if (!ed->search_arena) ed->search_arena = ArenaAlloc();
  ArenaClear(ed->search_arena);

  ed->search_pattern = PushStr8Copy(ed->search_arena, pattern);
  ed->search_forward = forward;
  ed->search_highlight = true;
}

bool EditorSearchMove(Editor *ed, View *view, bool forward, u64 count) {
  Buffer *buffer = EditorBufferForView(ed, view);
  if (!buffer) return false;

  if (ed->search_pattern.size == 0) {
    EditorSetStatus(ed, Str8Lit("no previous search pattern"));
    return false;
  }

  u64 at = view->cursor;
  bool wrapped = false;
  for (u64 i = 0; i < Max(count, (u64)1); i += 1) {
    SearchHit hit = BufferSearch(buffer, ed->search_pattern, at, forward, true);
    if (!hit.found) {
      EditorSetStatusF(ed, "pattern not found: %.*s", (int)ed->search_pattern.size,
                       (const char *)ed->search_pattern.str);
      return false;
    }
    at = hit.offset;
    wrapped = wrapped || hit.wrapped;
  }

  ViewSetCursor(view, buffer, at);
  ed->search_highlight = true;

  // Vim announces a wrap rather than leaving you to wonder why the cursor
  // jumped to the other end of the file.
  if (wrapped) {
    EditorSetStatus(ed, forward ? Str8Lit("search hit BOTTOM, continuing at TOP")
                                : Str8Lit("search hit TOP, continuing at BOTTOM"));
  }
  return true;
}

void EditorSearchPreview(Editor *ed) {
  View *view = ed->search_origin_view;
  if (!view) return;

  Buffer *target = EditorBufferForView(ed, view);
  Buffer *prompt = BufferFromHandle(&ed->buffers, ed->command_buffer);
  if (!target || !prompt) return;

  TempArena scratch = ScratchBegin();
  String8 pattern = BufferTextAll(scratch.arena, prompt);
  bool forward = (ed->command_line_prompt != '?');

  // An empty or unmatched pattern shows the origin, so deleting back to nothing
  // undoes the preview rather than leaving the cursor at the last match.
  SearchHit hit = {0, false, false};
  if (pattern.size > 0) {
    hit = BufferSearch(target, pattern, ed->search_origin, forward, true);
  }

  ViewSetCursor(view, target, hit.found ? hit.offset : ed->search_origin);
  if (!hit.found) view->scroll_line = ed->search_origin_scroll;

  // Highlighting during the preview needs the in-progress pattern, but it must
  // not become what `n` repeats -- only submitting does that.
  if (!ed->search_arena) ed->search_arena = ArenaAlloc();
  ArenaClear(ed->search_arena);
  ed->search_pattern = PushStr8Copy(ed->search_arena, pattern);
  ed->search_highlight = true;

  EditorScrollFocusedToCursor(ed);
  ScratchEnd(scratch);
}

namespace {

// Opens the search prompt, remembering where to return to if it is abandoned.
void SearchPromptOpen(Editor *ed, bool forward) {
  Buffer *buffer = BufferFromHandle(&ed->buffers, ed->command_buffer);
  View *view = EditorFocusedView(ed);
  if (!buffer || !ed->command_view || !view) return;

  ed->search_origin_view = view;
  ed->search_origin = view->cursor;
  ed->search_origin_scroll = view->scroll_line;

  BufferSetText(ed, buffer, String8{nullptr, 0});
  ViewInit(ed->command_view, ed->command_buffer);
  ed->command_view->vim.mode = VimMode::Insert;
  ed->command_line_active = true;
  ed->command_line_prompt = forward ? '/' : '?';
}

// `*` and `#`: search for the word under the cursor without typing it.
void SearchWordUnderCursor(Editor *ed, View *view, bool forward) {
  Buffer *buffer = EditorBufferForView(ed, view);
  if (!buffer || !view) return;

  TempArena scratch = ScratchBegin();
  String8 word = BufferWordAtCursor(scratch.arena, buffer, view->cursor);
  if (word.size == 0) {
    EditorSetStatus(ed, Str8Lit("no word under cursor"));
    ScratchEnd(scratch);
    return;
  }

  EditorSetSearchPattern(ed, word, forward);
  ScratchEnd(scratch);

  // Vim starts `*` from the beginning of the word, so a cursor in the middle of
  // one does not match the very word it started from.
  u64 saved = view->cursor;
  SearchHit start = BufferSearch(buffer, ed->search_pattern, saved + 1, false, false);
  if (start.found && start.offset + ed->search_pattern.size > saved) {
    ViewSetCursor(view, buffer, start.offset);
  }

  if (!EditorSearchMove(ed, view, forward, 1)) ViewSetCursor(view, buffer, saved);
}

}  // namespace

static void Cmd_search_forward(CommandArgs *a) {
  // With an argument the search runs directly, which is what `:search foo` and
  // a submitted `/foo` both do; without one it opens the prompt.
  if (a->text.size > 0) {
    EditorSetSearchPattern(a->ed, a->text, true);
    EditorSearchMove(a->ed, a->view, true, 1);
    return;
  }
  SearchPromptOpen(a->ed, true);
}

static void Cmd_search_backward(CommandArgs *a) {
  if (a->text.size > 0) {
    EditorSetSearchPattern(a->ed, a->text, false);
    EditorSearchMove(a->ed, a->view, false, 1);
    return;
  }
  SearchPromptOpen(a->ed, false);
}

// `n` repeats in the direction the search was made, `N` against it, so `n`
// after a `?` walks backward.
static void Cmd_search_next(CommandArgs *a) {
  EditorSearchMove(a->ed, a->view, a->ed->search_forward, a->count);
}

static void Cmd_search_prev(CommandArgs *a) {
  EditorSearchMove(a->ed, a->view, !a->ed->search_forward, a->count);
}

static void Cmd_search_word_forward(CommandArgs *a) {
  SearchWordUnderCursor(a->ed, a->view, true);
}

static void Cmd_search_word_backward(CommandArgs *a) {
  SearchWordUnderCursor(a->ed, a->view, false);
}

static void Cmd_search_clear(CommandArgs *a) { a->ed->search_highlight = false; }

// ---------------------------------------------------------------------------
// Command line
// ---------------------------------------------------------------------------

namespace {

// Closes the command window and clears it, leaving the view ready for next
// time. Resetting the view matters because its cursor would otherwise be left
// pointing past the end of the now-empty buffer.
void CommandLineClose(Editor *ed) {
  ed->command_line_active = false;
  ed->command_line_prompt = ':';
  ed->search_origin_view = nullptr;

  Buffer *buffer = BufferFromHandle(&ed->buffers, ed->command_buffer);
  if (buffer) BufferSetText(ed, buffer, String8{nullptr, 0});
  if (ed->command_view) ViewInit(ed->command_view, ed->command_buffer);
}

// Abandoning a search undoes its incremental preview: the cursor goes back to
// where `/` was pressed, and the highlight goes with it. Abandoning a `:`
// command has nothing to undo.
void CommandLineDismiss(Editor *ed) {
  View *view = ed->search_origin_view;
  if (view && ed->command_line_prompt != ':') {
    Buffer *target = EditorBufferForView(ed, view);
    if (target) {
      ViewSetCursor(view, target, ed->search_origin);
      view->scroll_line = ed->search_origin_scroll;
    }
    ed->search_highlight = false;
  }
  CommandLineClose(ed);
}

}  // namespace

namespace {

// Opens the command window with `prefill` already typed and the cursor after
// it. This is how a keybinding asks for something it cannot supply itself: a
// binding carries no argument text, so `<leader>pg` cannot grep for anything
// until the pattern has been typed.
void CommandLineOpenWith(Editor *ed, String8 prefill) {
  Buffer *buffer = BufferFromHandle(&ed->buffers, ed->command_buffer);
  if (!buffer || !ed->command_view) return;

  BufferSetText(ed, buffer, prefill);
  ViewInit(ed->command_view, ed->command_buffer);
  ed->command_view->vim.mode = VimMode::Insert;
  ViewSetCursor(ed->command_view, buffer, BufferSize(buffer));
  ed->command_line_active = true;
  ed->command_line_prompt = ':';
}

}  // namespace

static void Cmd_command_line_open(CommandArgs *a) {
  Editor *ed = a->ed;
  Buffer *buffer = BufferFromHandle(&ed->buffers, ed->command_buffer);
  if (!buffer || !ed->command_view) return;

  BufferSetText(ed, buffer, String8{nullptr, 0});
  ViewInit(ed->command_view, ed->command_buffer);

  // Open in insert mode, as `q:i` does in vim: you are usually here to type a
  // command, but Esc drops to normal mode and every motion and operator works
  // on the line.
  ed->command_view->vim.mode = VimMode::Insert;
  ed->command_line_active = true;
  ed->command_line_prompt = ':';
}

static void Cmd_command_line_submit(CommandArgs *a) {
  Editor *ed = a->ed;
  Buffer *buffer = BufferFromHandle(&ed->buffers, ed->command_buffer);
  if (!buffer) return;

  TempArena scratch = ScratchBegin();
  String8 line = PushStr8Copy(scratch.arena, BufferTextAll(scratch.arena, buffer));
  u8 prompt = ed->command_line_prompt;
  View *origin = ed->search_origin_view;

  if (prompt == '/' || prompt == '?') {
    bool forward = (prompt == '/');
    // An empty pattern repeats the previous one, as vim does, so a bare `/<CR>`
    // is another `n`.
    if (line.size > 0) EditorSetSearchPattern(ed, line, forward);

    // Search from where the prompt opened, not from where the preview left the
    // cursor -- otherwise submitting would skip to the match after the one
    // being shown.
    if (origin) ViewSetCursor(origin, EditorBufferForView(ed, origin), ed->search_origin);
    CommandLineClose(ed);
    if (origin) {
      EditorSearchMove(ed, origin, forward, 1);
      EditorScrollFocusedToCursor(ed);
    }
    ScratchEnd(scratch);
    return;
  }

  // Close first, so the command runs against the window underneath rather than
  // against the command line it was typed into.
  CommandLineDismiss(ed);
  (void)CommandExecLine(ed, line);

  ScratchEnd(scratch);
}

static void Cmd_command_line_cancel(CommandArgs *a) { CommandLineDismiss(a->ed); }

// Esc means "leave insert mode" the first time and "give up on this command"
// the second, so the window behaves like any other buffer until there is
// nothing left for Esc to do.
static void Cmd_command_line_escape(CommandArgs *a) {
  Editor *ed = a->ed;
  View *view = ed->command_view;
  if (!view) return;

  if (VimModeIsInsert(view->vim.mode)) {
    Cmd_normal_mode(a);
    return;
  }
  CommandLineDismiss(ed);
}

static void Cmd_command_line_complete(CommandArgs *a) {
  Editor *ed = a->ed;
  Buffer *buffer = BufferFromHandle(&ed->buffers, ed->command_buffer);
  if (!buffer) return;

  TempArena scratch = ScratchBegin();
  String8 line = BufferTextAll(scratch.arena, buffer);
  CommandCompletion completion = CommandCompletionsFor(scratch.arena, ed, line);

  if (completion.count == 0) {
    ScratchEnd(scratch);
    return;
  }

  // With one candidate, accept it. With several, extend to their longest
  // common prefix and show them, which is what makes repeated Tab useful
  // rather than cycling blindly.
  String8 replacement = completion.items[0];
  if (completion.count > 1) {
    u64 common = replacement.size;
    for (u64 i = 1; i < completion.count; i += 1) {
      String8 other = completion.items[i];
      u64 n = Min(common, other.size);
      u64 k = 0;
      while (k < n && replacement.str[k] == other.str[k]) k += 1;
      common = k;
    }
    replacement = Str8Prefix(replacement, common);

    TempArena list_scratch = ScratchBegin1(scratch.arena);
    String8List names = {};
    for (u64 i = 0; i < completion.count && i < 12; i += 1) {
      Str8ListPush(list_scratch.arena, &names, completion.items[i]);
    }
    EditorSetStatus(ed, Str8ListJoin(list_scratch.arena, &names, Str8Lit("  ")));
    ScratchEnd(list_scratch);
  } else {
    EditorSetStatus(ed, String8{nullptr, 0});
  }

  RangeU64 replace = RangeU64{completion.replace_start, completion.replace_end};
  if (replacement.size > 0) {
    BufferReplace(ed, buffer, replace, replacement, 0, 0);
    View *command_view = a->view;
    if (command_view) ViewSetCursor(command_view, buffer, BufferSize(buffer));
  }

  ScratchEnd(scratch);
}

// ---------------------------------------------------------------------------
// Listings
//
// Each opens an ordinary scratch buffer. Showing a list needs no new UI
// concept, which is the same property a grep results pane or a file explorer
// will rely on.
// ---------------------------------------------------------------------------

namespace {

void ShowListing(Editor *ed, String8 name, String8 text) {
  BufferHandle handle = BufferFromName(&ed->buffers, name);
  if (handle.index == 0) handle = BufferOpen(&ed->buffers, BufferKind::Scratch, name);

  Buffer *buffer = BufferFromHandle(&ed->buffers, handle);
  if (!buffer) return;

  buffer->flags &= ~BufferFlags::ReadOnly;
  BufferSetText(ed, buffer, text);
  buffer->flags |= BufferFlags::ReadOnly;

  EditorShowBuffer(ed, handle);
}

}  // namespace

// Provided by core/buffers/buf_picker.cpp.
BufferHandle GrepBufferOpen(Editor *ed, String8 pattern);
BufferHandle FinderBufferOpen(Editor *ed);
BufferHandle LiveGrepBufferOpen(Editor *ed);

// Opens a picker whose first line is the query, ready to type into. Shared by
// the file finder and the live search, which differ only in what they do with
// what is typed.
static void OpenQueryPicker(CommandArgs *a, BufferHandle handle) {
  if (handle.index == 0) return;
  EditorShowBuffer(a->ed, handle);

  View *view = EditorFocusedView(a->ed);
  Buffer *buffer = EditorFocusedBuffer(a->ed);
  if (!view || !buffer) return;

  ViewSetCursorLineColumn(view, buffer, 0, 0);
  view->vim.mode = VimMode::Insert;

  // A query given up front skips the typing.
  if (a->text.size > 0) {
    BufferInsert(a->ed, buffer, 0, a->text, 0, a->text.size);
    ViewSetCursor(view, buffer, a->text.size);
  }
}

static void Cmd_live_grep(CommandArgs *a) {
  OpenQueryPicker(a, LiveGrepBufferOpen(a->ed));
}

static void Cmd_grep(CommandArgs *a) {
  // A keybinding carries no argument, so with nothing to search for the only
  // useful thing to do is ask. Searching for "" and reporting no matches looks
  // like a broken command.
  if (Str8SkipChopWhitespace(a->text).size == 0) {
    CommandLineOpenWith(a->ed, Str8Lit("grep "));
    return;
  }

  BufferHandle handle = GrepBufferOpen(a->ed, a->text);
  if (handle.index == 0) return;

  EditorShowBuffer(a->ed, handle);
  Buffer *buffer = EditorFocusedBuffer(a->ed);
  View *view = EditorFocusedView(a->ed);
  // Skip the two header lines so the cursor starts on a result.
  if (buffer && view && BufferLineCount(buffer) > 2) {
    ViewSetCursorLineColumn(view, buffer, 2, 0);
  }
}

static void Cmd_find_file(CommandArgs *a) { OpenQueryPicker(a, FinderBufferOpen(a->ed)); }

// <CR> in a results buffer. The buffer's own on_submit knows what its lines
// mean; this just hands the current one over.
static void Cmd_result_open(CommandArgs *a) {
  Buffer *buffer = a->buffer;
  if (!buffer->hooks.on_submit) return;

  TempArena scratch = ScratchBegin();
  u64 line = ViewCursorLine(a->view, buffer);
  String8 text = BufferLineText(scratch.arena, buffer, line);
  buffer->hooks.on_submit(a->ed, buffer, a->view, text);
  ScratchEnd(scratch);
}

static void Cmd_list_commands(CommandArgs *a) {
  TempArena scratch = ScratchBegin();
  String8List lines = {};

  Str8ListPush(scratch.arena, &lines, Str8Lit("COMMAND                  ARGUMENT   DESCRIPTION"));
  Str8ListPush(scratch.arena, &lines, Str8Lit(""));

  for (u16 i = 1; i < (u16)CommandId::COUNT; i += 1) {
    const CommandSpec *spec = CommandSpecFromId((CommandId)i);
    if (!spec || HasFlag(spec->flags, CommandFlags::Hidden)) continue;

    const char *arg = "";
    switch (spec->arg) {
      case CommandArg::Path: arg = "path"; break;
      case CommandArg::BufferName: arg = "buffer"; break;
      case CommandArg::CommandName: arg = "command"; break;
      case CommandArg::Text: arg = "text"; break;
      default: arg = "-"; break;
    }

    // Show the shape as well as the name, so the listing doubles as the
    // reference for what each command accepts.
    TempArena inner = ScratchBegin1(scratch.arena);
    String8 decorated = PushStr8F(inner.arena, "%.*s%s%s", (int)spec->name.size,
                                  (char *)spec->name.str,
                                  HasFlag(spec->flags, CommandFlags::Bang) ? "[!]" : "",
                                  HasFlag(spec->flags, CommandFlags::Range) ? " [range]" : "");
    Str8ListPush(scratch.arena, &lines,
                 PushStr8F(scratch.arena, "%-24.*s %-10s %.*s", (int)decorated.size,
                           (char *)decorated.str, arg, (int)spec->desc.size,
                           (char *)spec->desc.str));
    ScratchEnd(inner);
  }

  ShowListing(a->ed, Str8Lit("[commands]"), Str8ListJoin(scratch.arena, &lines, Str8Lit("\n")));
  ScratchEnd(scratch);
}

static void Cmd_list_buffers(CommandArgs *a) {
  TempArena scratch = ScratchBegin();
  String8List lines = {};

  for (BufferHandle h = BufferFirst(&a->ed->buffers); h.index != 0;
       h = BufferNext(&a->ed->buffers, h)) {
    Buffer *b = BufferFromHandle(&a->ed->buffers, h);
    if (!b) continue;
    Str8ListPush(scratch.arena, &lines,
                 PushStr8F(scratch.arena, "%3llu %s %-24.*s %.*s", (unsigned long long)h.index,
                           BufferIsDirty(b) ? "+" : " ", (int)b->name.size, (char *)b->name.str,
                           (int)b->path.size, (char *)b->path.str));
  }

  ShowListing(a->ed, Str8Lit("[buffers]"), Str8ListJoin(scratch.arena, &lines, Str8Lit("\n")));
  ScratchEnd(scratch);
}

static void Cmd_list_bindings(CommandArgs *a) {
  TempArena scratch = ScratchBegin();
  String8List lines = {};

  struct MapEntry { const char *label; Keymap *map; };
  MapEntry maps[] = {
      {"normal", a->ed->normal_map},   {"visual", a->ed->visual_map},
      {"insert", a->ed->insert_map},   {"operator-pending", a->ed->operator_pending_map},
      {"global", a->ed->global_map},
  };

  for (u64 i = 0; i < ArrayCount(maps); i += 1) {
    Str8ListPush(scratch.arena, &lines,
                 PushStr8F(scratch.arena, "-- %s --", maps[i].label));

    KeymapEntryList entries = KeymapAllBindings(scratch.arena, maps[i].map);
    for (u64 j = 0; j < entries.count; j += 1) {
      String8 name = CommandName(entries.entries[j].command);
      Str8ListPush(scratch.arena, &lines,
                   PushStr8F(scratch.arena, "  %-12.*s %.*s", (int)entries.entries[j].spec.size,
                             (char *)entries.entries[j].spec.str, (int)name.size,
                             (char *)name.str));
    }
    Str8ListPush(scratch.arena, &lines, Str8Lit(""));
  }

  ShowListing(a->ed, Str8Lit("[bindings]"), Str8ListJoin(scratch.arena, &lines, Str8Lit("\n")));
  ScratchEnd(scratch);
}

// ---------------------------------------------------------------------------
// Table
// ---------------------------------------------------------------------------

namespace {

constexpr Command kCommands[] = {
    {CommandId::None, nullptr},
#define X(id, name, desc, arg, flags) {CommandId::id, Cmd_##id},
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
  CommandArgs args = {};
  args.text = text;
  args.chord = chord;
  CommandExecArgs(ed, id, &args);
}

void CommandExecArgs(Editor *ed, CommandId id, CommandArgs *supplied) {
  const Command *command = CommandFromId(id);
  if (!command || !command->proc) return;

  // The window receiving input, which is the command window while it is open.
  // Motions and edits must act on what the user is actually typing into; a
  // command *submitted* from there still reaches the file underneath, because
  // submitting closes the window before running the line.
  View *view = EditorInputView(ed);
  Buffer *buffer = EditorBufferForView(ed, view);
  if (!view || !buffer) return;

  CommandArgs args = *supplied;
  args.ed = ed;
  args.view = view;
  args.buffer = buffer;

  // Resolve the vim count once, here, so no command has to reimplement the
  // "operator count times motion count, defaulting to one" rule.
  args.has_count = view->vim.has_count || view->vim.has_operator_count;
  args.count = VimEffectiveCount(&view->vim);

  // Clear the pending count before running, or a command that itself feeds
  // input back through the editor would see a stale one.
  view->vim.count = 0;
  view->vim.has_count = false;

  // A range implies a count for commands that think in lines, so ":1,5d" and
  // "5dd" reach the same code.
  if (args.has_range) {
    args.count = args.line_last - args.line_first + 1;
    args.has_count = true;
  }

  command->proc(&args);

  // The operator count and the selected register survive only until the motion
  // that consumes them, so `"+d2w` carries both through to the end.
  if (view->vim.mode != VimMode::OperatorPending) {
    view->vim.operator_count = 0;
    view->vim.has_operator_count = false;
    view->vim.pending_register = 0;
  }

  EditorScrollFocusedToCursor(ed);
}
