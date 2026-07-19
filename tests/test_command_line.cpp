#include "editor/command.h"
#include "editor/editor.h"
#include "test.h"

namespace {

struct Fixture {
  Arena *arena;
  Editor ed;
};

Fixture MakeFixture(const char *text) {
  Fixture f = {};
  f.arena = ArenaAlloc(MB(64));
  EditorInit(&f.ed, f.arena, RectS32{0, 0, 80, 25});

  Buffer *buffer = EditorFocusedBuffer(&f.ed);
  BufferSetText(&f.ed, buffer, Str8C(text));
  ViewSetCursor(EditorFocusedView(&f.ed), buffer, 0);
  return f;
}

void Destroy(Fixture *f) {
  EditorDestroy(&f->ed);
  ArenaRelease(f->arena);
}

CommandParse Parse(Fixture *f, const char *line) {
  return CommandParseLine(f->arena, Str8C(line), EditorFocusedBuffer(&f->ed),
                          EditorFocusedView(&f->ed));
}

String8 TextOf(Fixture *f) { return BufferTextAll(f->arena, EditorFocusedBuffer(&f->ed)); }

}  // namespace

// ---------------------------------------------------------------------------
// Name resolution
// ---------------------------------------------------------------------------

TEST(command_line_resolves_names) {
  Fixture f = MakeFixture("hello");

  CHECK_EQ((u32)Parse(&f, "write").id, (u32)CommandId::write_file);
  CHECK_EQ((u32)Parse(&f, "quit").id, (u32)CommandId::quit);
  CHECK_EQ((u32)Parse(&f, "split-vertical").id, (u32)CommandId::split_vertical);

  Destroy(&f);
}

TEST(command_line_resolves_aliases) {
  Fixture f = MakeFixture("hello");

  // The muscle-memory spellings, which deliberately win over prefix matching:
  // "w" would otherwise be ambiguous between write and write-quit.
  CHECK_EQ((u32)Parse(&f, "w").id, (u32)CommandId::write_file);
  CHECK_EQ((u32)Parse(&f, "q").id, (u32)CommandId::quit);
  CHECK_EQ((u32)Parse(&f, "wq").id, (u32)CommandId::write_quit);
  CHECK_EQ((u32)Parse(&f, "qa").id, (u32)CommandId::quit_all);
  CHECK_EQ((u32)Parse(&f, "vs").id, (u32)CommandId::split_vertical);
  CHECK_EQ((u32)Parse(&f, "sp").id, (u32)CommandId::split_horizontal);

  Destroy(&f);
}

TEST(command_line_resolves_unambiguous_prefixes) {
  Fixture f = MakeFixture("hello");

  // "und" reaches undo because nothing else starts with it.
  CHECK_EQ((u32)Parse(&f, "und").id, (u32)CommandId::undo);
  CHECK_EQ((u32)Parse(&f, "only").id, (u32)CommandId::only_window);

  // "buffer-" matches both buffer-next and buffer-prev, so it resolves to
  // nothing rather than guessing.
  CommandParse ambiguous = Parse(&f, "buffer-");
  CHECK_EQ((u32)ambiguous.id, (u32)CommandId::None);
  CHECK_EQ((u32)ambiguous.status, (u32)CommandParseStatus::AmbiguousPrefix);

  CommandParse unknown = Parse(&f, "zzz");
  CHECK_EQ((u32)unknown.status, (u32)CommandParseStatus::UnknownCommand);

  Destroy(&f);
}

// ---------------------------------------------------------------------------
// Ranges
// ---------------------------------------------------------------------------

TEST(command_line_parses_ranges) {
  Fixture f = MakeFixture("a\nb\nc\nd\ne\nf\ng\nh");

  // Line numbers are 1-based as typed, 0-based internally.
  CommandParse single = Parse(&f, "3d");
  CHECK(single.has_range);
  CHECK_EQ(single.line_first, 2);
  CHECK_EQ(single.line_last, 2);

  CommandParse span = Parse(&f, "2,5d");
  CHECK(span.has_range);
  CHECK_EQ(span.line_first, 1);
  CHECK_EQ(span.line_last, 4);

  // The whole buffer.
  CommandParse all = Parse(&f, "%d");
  CHECK(all.has_range);
  CHECK_EQ(all.line_first, 0);
  CHECK_EQ(all.line_last, 7);

  Destroy(&f);
}

TEST(command_line_range_symbols_and_offsets) {
  Fixture f = MakeFixture("a\nb\nc\nd\ne\nf\ng\nh");

  ViewSetCursorLineColumn(EditorFocusedView(&f.ed), EditorFocusedBuffer(&f.ed), 3, 0);

  // "." is the current line, "$" the last.
  CommandParse current = Parse(&f, ".d");
  CHECK_EQ(current.line_first, 3);

  CommandParse last = Parse(&f, "$d");
  CHECK_EQ(last.line_first, 7);

  // Offsets relative to the current line.
  CommandParse forward = Parse(&f, ".,+2d");
  CHECK_EQ(forward.line_first, 3);
  CHECK_EQ(forward.line_last, 5);

  CommandParse backward = Parse(&f, "-2,.d");
  CHECK_EQ(backward.line_first, 1);
  CHECK_EQ(backward.line_last, 3);

  // A backwards range is normalised rather than rejected.
  CommandParse reversed = Parse(&f, "5,2d");
  CHECK_EQ(reversed.line_first, 1);
  CHECK_EQ(reversed.line_last, 4);

  // Past the end of the buffer is an error, not a silent clamp.
  CHECK_EQ((u32)Parse(&f, "99d").status, (u32)CommandParseStatus::BadRange);

  Destroy(&f);
}

TEST(command_line_bare_range_goes_to_line) {
  Fixture f = MakeFixture("a\nb\nc\nd\ne");

  // ":3" with no command means "go to line 3", as vim does.
  CommandParse parse = Parse(&f, "3");
  CHECK_EQ((u32)parse.status, (u32)CommandParseStatus::Ok);
  CHECK_EQ((u32)parse.id, (u32)CommandId::goto_line);

  CHECK(CommandExecLine(&f.ed, Str8Lit("4")));
  CHECK_EQ(ViewCursorLine(EditorFocusedView(&f.ed), EditorFocusedBuffer(&f.ed)), 3);

  Destroy(&f);
}

TEST(command_line_rejects_range_on_commands_that_take_none) {
  Fixture f = MakeFixture("a\nb\nc");

  // A range on something that cannot use one is a mistake worth reporting,
  // not something to silently ignore.
  CHECK_EQ((u32)Parse(&f, "1,2quit").status, (u32)CommandParseStatus::UnexpectedRange);

  Destroy(&f);
}

TEST(command_line_range_drives_line_commands) {
  Fixture f = MakeFixture("one\ntwo\nthree\nfour\nfive");

  CHECK(CommandExecLine(&f.ed, Str8Lit("2,3d")));
  CHECK_STR(TextOf(&f), Str8Lit("one\nfour\nfive"));

  // The same code path a keybinding reaches, so ":1d" and "dd" agree.
  CHECK(CommandExecLine(&f.ed, Str8Lit("1d")));
  CHECK_STR(TextOf(&f), Str8Lit("four\nfive"));

  Destroy(&f);
}

TEST(command_line_percent_range_affects_whole_buffer) {
  Fixture f = MakeFixture("a\nb\nc");

  CHECK(CommandExecLine(&f.ed, Str8Lit("%indent")));
  CHECK_STR(TextOf(&f), Str8Lit("  a\n  b\n  c"));

  CHECK(CommandExecLine(&f.ed, Str8Lit("%dedent")));
  CHECK_STR(TextOf(&f), Str8Lit("a\nb\nc"));

  Destroy(&f);
}

// ---------------------------------------------------------------------------
// Bang
// ---------------------------------------------------------------------------

TEST(command_line_parses_bang) {
  Fixture f = MakeFixture("hello");

  CommandParse forced = Parse(&f, "q!");
  CHECK(forced.bang);
  CHECK_EQ((u32)forced.id, (u32)CommandId::quit);

  CHECK(!Parse(&f, "q").bang);

  // A command with no forced variant rejects it rather than ignoring it.
  CHECK_EQ((u32)Parse(&f, "only!").status, (u32)CommandParseStatus::UnexpectedBang);

  Destroy(&f);
}

TEST(command_line_bang_overrides_unsaved_guard) {
  Fixture f = MakeFixture("hello");

  Buffer *buffer = EditorFocusedBuffer(&f.ed);
  buffer->kind = BufferKind::File;
  buffer->flags |= BufferFlags::Dirty;

  // Without the bang, quitting is refused and says so.
  CHECK(CommandExecLine(&f.ed, Str8Lit("qa")));
  CHECK(!f.ed.quit);
  CHECK(f.ed.status_message.size > 0);

  // With it, the work is discarded as asked.
  CHECK(CommandExecLine(&f.ed, Str8Lit("qa!")));
  CHECK(f.ed.quit);

  Destroy(&f);
}

// ---------------------------------------------------------------------------
// Arguments
// ---------------------------------------------------------------------------

TEST(command_line_parses_arguments) {
  Fixture f = MakeFixture("hello");

  CommandParse parse = Parse(&f, "edit src/main.cpp");
  CHECK_EQ((u32)parse.id, (u32)CommandId::edit_file);
  CHECK_STR(parse.arg, Str8Lit("src/main.cpp"));

  // Quoted arguments keep their spaces.
  CommandParse quoted = Parse(&f, "edit \"my file.txt\"");
  CHECK_STR(quoted.arg, Str8Lit("my file.txt"));

  // A backslash escapes a quote inside one.
  CommandParse escaped = Parse(&f, "edit \"odd\\\"name\"");
  CHECK_STR(escaped.arg, Str8Lit("odd\"name"));

  Destroy(&f);
}

TEST(command_line_requires_declared_arguments) {
  Fixture f = MakeFixture("hello");

  // `edit` declares its path mandatory, so the parser catches its absence
  // before the command body has to.
  CHECK_EQ((u32)Parse(&f, "edit").status, (u32)CommandParseStatus::MissingArgument);
  CHECK_EQ((u32)Parse(&f, "edit x").status, (u32)CommandParseStatus::Ok);

  // `write` takes an optional path.
  CHECK_EQ((u32)Parse(&f, "write").status, (u32)CommandParseStatus::Ok);

  Destroy(&f);
}

TEST(command_line_empty_and_whitespace) {
  Fixture f = MakeFixture("hello");

  CHECK_EQ((u32)Parse(&f, "").status, (u32)CommandParseStatus::Empty);
  CHECK_EQ((u32)Parse(&f, "   ").status, (u32)CommandParseStatus::Empty);
  // An empty line is not an error.
  CHECK(CommandExecLine(&f.ed, Str8Lit("")));

  Destroy(&f);
}

TEST(command_line_status_messages_name_the_problem) {
  Fixture f = MakeFixture("hello");

  const char *bad[] = {"zzz", "buffer-", "1,2quit", "only!", "edit"};
  for (u64 i = 0; i < ArrayCount(bad); i += 1) {
    CommandParse parse = Parse(&f, bad[i]);
    String8 message = CommandParseStatusMessage(f.arena, &parse);
    CHECK(message.size > 0);
  }

  // A valid command produces no complaint.
  CommandParse ok = Parse(&f, "write");
  CHECK_EQ(CommandParseStatusMessage(f.arena, &ok).size, 0);

  Destroy(&f);
}

// ---------------------------------------------------------------------------
// Completion
// ---------------------------------------------------------------------------

TEST(command_line_completes_command_names) {
  Fixture f = MakeFixture("hello");

  CommandCompletion completion = CommandCompletionsFor(f.arena, &f.ed, Str8Lit("spl"));
  CHECK_EQ(completion.count, 2);  // split-vertical, split-horizontal
  CHECK_EQ(completion.replace_start, 0);
  CHECK_EQ(completion.replace_end, 3);

  CommandCompletion exact = CommandCompletionsFor(f.arena, &f.ed, Str8Lit("split-v"));
  CHECK_EQ(exact.count, 1);
  CHECK_STR(exact.items[0], Str8Lit("split-vertical"));

  // Hidden keybinding-only actions stay out of the suggestions.
  CommandCompletion hidden = CommandCompletionsFor(f.arena, &f.ed, Str8Lit("cursor-"));
  CHECK_EQ(hidden.count, 0);

  Destroy(&f);
}

TEST(command_line_completes_buffer_names) {
  Fixture f = MakeFixture("hello");

  (void)BufferOpen(&f.ed.buffers, BufferKind::File, Str8Lit("alpha.cpp"));
  (void)BufferOpen(&f.ed.buffers, BufferKind::File, Str8Lit("alps.txt"));
  (void)BufferOpen(&f.ed.buffers, BufferKind::File, Str8Lit("beta.cpp"));

  // Past the command name, completion switches to the declared argument kind.
  CommandCompletion completion = CommandCompletionsFor(f.arena, &f.ed, Str8Lit("buffer al"));
  CHECK_EQ(completion.count, 2);
  CHECK_EQ(completion.replace_start, 7);

  Destroy(&f);
}

TEST(command_line_completes_paths) {
  Fixture f = MakeFixture("hello");

  // The cwd is the test binary's, which sits in a real directory; completing an
  // empty path argument should offer whatever is in it.
  CommandCompletion completion = CommandCompletionsFor(f.arena, &f.ed, Str8Lit("edit "));
  CHECK(completion.count > 0);
  CHECK_EQ(completion.replace_start, 5);

  Destroy(&f);
}

// ---------------------------------------------------------------------------
// The command window as a buffer
// ---------------------------------------------------------------------------

TEST(command_window_typing_and_submitting) {
  Fixture f = MakeFixture("one\ntwo\nthree");

  // Open it the way a keypress does.
  EditorProcessSpec(&f.ed, ":");
  CHECK(f.ed.command_line_active);

  Buffer *command = BufferFromHandle(&f.ed.buffers, f.ed.command_buffer);

  // Typing goes into the command buffer like any other text, because input is
  // routed to the command window's own view while it is open.
  EditorProcessSpec(&f.ed, "1d");
  CHECK_STR(BufferTextAll(f.arena, command), Str8Lit("1d"));

  // Enter submits.
  EditorProcessSpec(&f.ed, "<CR>");

  CHECK(!f.ed.command_line_active);
  CHECK_STR(TextOf(&f), Str8Lit("two\nthree"));
  // The prompt clears itself for next time.
  CHECK_EQ(BufferSize(command), 0);

  Destroy(&f);
}

TEST(command_window_escape_leaves_insert_then_dismisses) {
  Fixture f = MakeFixture("one\ntwo");

  EditorProcessSpec(&f.ed, ":");
  CHECK(f.ed.command_line_active);
  // It opens in insert mode, as `q:i` does in vim.
  CHECK_EQ((u32)f.ed.command_view->vim.mode, (u32)VimMode::Insert);

  Buffer *command = BufferFromHandle(&f.ed.buffers, f.ed.command_buffer);
  EditorProcessSpec(&f.ed, "1d");

  // The first Escape leaves insert mode without abandoning what was typed.
  EditorProcessSpec(&f.ed, "<Esc>");
  CHECK(f.ed.command_line_active);
  CHECK_EQ((u32)f.ed.command_view->vim.mode, (u32)VimMode::Normal);
  CHECK_STR(BufferTextAll(f.arena, command), Str8Lit("1d"));

  // The second gives up on the command.
  EditorProcessSpec(&f.ed, "<Esc>");
  CHECK(!f.ed.command_line_active);
  CHECK_STR(TextOf(&f), Str8Lit("one\ntwo"));
  CHECK_EQ(BufferSize(command), 0);

  Destroy(&f);
}

// ---------------------------------------------------------------------------
// Modal editing inside the command window
//
// The command window is a vim buffer like any other. These pin that down,
// because the whole point of the buffer-local keymap layering above the mode
// map -- rather than replacing it -- is that a buffer can claim a key or two
// without giving up modal editing. A file explorer or grep pane will rely on
// exactly this.
// ---------------------------------------------------------------------------

namespace {

// Opens the command window and types `text` into it.
void OpenCommandLine(Fixture *f, const char *text) {
  EditorProcessSpec(&f->ed, ":");
  EditorProcessSpec(&f->ed, text);
}

String8 CommandText(Fixture *f) {
  Buffer *command = BufferFromHandle(&f->ed.buffers, f->ed.command_buffer);
  return BufferTextAll(f->arena, command);
}

}  // namespace

TEST(command_window_supports_vim_motions) {
  Fixture f = MakeFixture("hello");

  OpenCommandLine(&f, "split-vertical");
  EditorProcessSpec(&f.ed, "<Esc>");

  View *view = f.ed.command_view;
  Buffer *command = BufferFromHandle(&f.ed.buffers, f.ed.command_buffer);

  // Leaving insert steps back onto the last character, as vim does.
  CHECK_EQ(ViewCursorColumn(view, command), 13);

  EditorProcessSpec(&f.ed, "0");
  CHECK_EQ(ViewCursorColumn(view, command), 0);
  EditorProcessSpec(&f.ed, "$");
  CHECK_EQ(ViewCursorColumn(view, command), 13);
  EditorProcessSpec(&f.ed, "b");
  CHECK_EQ(ViewCursorColumn(view, command), 6);  // start of "vertical"
  EditorProcessSpec(&f.ed, "w");
  CHECK_EQ(ViewCursorColumn(view, command), 13);

  Destroy(&f);
}

TEST(command_window_supports_vim_operators) {
  Fixture f = MakeFixture("hello");

  OpenCommandLine(&f, "split-vertical");
  EditorProcessSpec(&f.ed, "<Esc>");

  // dw on the command line edits the command line, not the file underneath.
  // `w` stops at the hyphen, since punctuation is its own word class.
  EditorProcessSpec(&f.ed, "0dw");
  CHECK_STR(CommandText(&f), Str8Lit("-vertical"));
  CHECK_STR(TextOf(&f), Str8Lit("hello"));

  // A text object reaches the whole word regardless.
  EditorProcessSpec(&f.ed, "u0dW");
  CHECK_STR(CommandText(&f), Str8Lit(""));

  // Undo works here too.
  EditorProcessSpec(&f.ed, "u");
  CHECK_STR(CommandText(&f), Str8Lit("split-vertical"));

  Destroy(&f);
}

TEST(command_window_supports_counts_and_insert_variants) {
  Fixture f = MakeFixture("hello");

  OpenCommandLine(&f, "abcdef");
  EditorProcessSpec(&f.ed, "<Esc>");

  // A count in normal mode is a count, not text.
  EditorProcessSpec(&f.ed, "0");
  EditorProcessSpec(&f.ed, "3x");
  CHECK_STR(CommandText(&f), Str8Lit("def"));

  // Append at the end, then insert at the start.
  EditorProcessSpec(&f.ed, "Aghi<Esc>");
  CHECK_STR(CommandText(&f), Str8Lit("defghi"));
  EditorProcessSpec(&f.ed, "Ixyz<Esc>");
  CHECK_STR(CommandText(&f), Str8Lit("xyzdefghi"));

  Destroy(&f);
}

TEST(command_window_stays_one_line) {
  Fixture f = MakeFixture("hello");

  OpenCommandLine(&f, "quit");
  EditorProcessSpec(&f.ed, "<Esc>");

  // `o` and `O` would ordinarily add a line; a SingleLine buffer drops it.
  EditorProcessSpec(&f.ed, "oxyz<Esc>");
  EditorProcessSpec(&f.ed, "Oabc<Esc>");

  Buffer *command = BufferFromHandle(&f.ed.buffers, f.ed.command_buffer);
  CHECK_EQ(BufferLineCount(command), 1);

  Destroy(&f);
}

TEST(command_window_submits_from_normal_mode) {
  Fixture f = MakeFixture("one\ntwo\nthree");

  OpenCommandLine(&f, "1d");
  EditorProcessSpec(&f.ed, "<Esc>");
  CHECK_EQ((u32)f.ed.command_view->vim.mode, (u32)VimMode::Normal);

  // <CR> is bound in the buffer-local map, so it works in every mode.
  EditorProcessSpec(&f.ed, "<CR>");
  CHECK(!f.ed.command_line_active);
  CHECK_STR(TextOf(&f), Str8Lit("two\nthree"));

  Destroy(&f);
}

TEST(command_window_editing_does_not_touch_the_file) {
  Fixture f = MakeFixture("one\ntwo\nthree");

  View *file_view = EditorFocusedView(&f.ed);
  u64 cursor_before = file_view->cursor;

  OpenCommandLine(&f, "quit");
  EditorProcessSpec(&f.ed, "<Esc>");
  // Motions, deletions and undo, all aimed at the command line.
  EditorProcessSpec(&f.ed, "0dwu$b");

  CHECK_STR(TextOf(&f), Str8Lit("one\ntwo\nthree"));
  CHECK_EQ(file_view->cursor, cursor_before);

  Destroy(&f);
}

TEST(buffer_local_keymap_layers_over_the_mode_map) {
  Fixture f = MakeFixture("hello");

  Buffer *command = BufferFromHandle(&f.ed.buffers, f.ed.command_buffer);
  CHECK(command->hooks.keymap != nullptr);

  EditorProcessSpec(&f.ed, ":");

  // In insert mode the buffer's own <CR> wins, and the insert map is reachable
  // underneath it.
  Keymap *active = EditorActiveKeymap(&f.ed);
  CHECK(active == command->hooks.keymap);
  CHECK(active->parent == f.ed.insert_map);
  CHECK_EQ((u32)KeymapLookupSequence(active, Str8Lit("<CR>")).command,
           (u32)CommandId::command_line_submit);
  CHECK_EQ((u32)KeymapLookupSequence(active, Str8Lit("<BS>")).command,
           (u32)CommandId::backspace);

  // Dropping to normal mode reparents onto the normal map, so motions resolve
  // without the buffer having to bind any of them.
  EditorProcessSpec(&f.ed, "<Esc>");
  active = EditorActiveKeymap(&f.ed);
  CHECK(active == command->hooks.keymap);
  CHECK(active->parent == f.ed.normal_map);
  CHECK_EQ((u32)KeymapLookupSequence(active, Str8Lit("w")).command,
           (u32)CommandId::word_forward);
  CHECK_EQ((u32)KeymapLookupSequence(active, Str8Lit("d")).command,
           (u32)CommandId::operator_delete);
  // And the buffer's own binding still wins over the mode map.
  CHECK_EQ((u32)KeymapLookupSequence(active, Str8Lit("<CR>")).command,
           (u32)CommandId::command_line_submit);

  Destroy(&f);
}

TEST(listing_commands_opens_a_buffer) {
  Fixture f = MakeFixture("hello");

  // Showing a list needs no new UI concept: it is an ordinary scratch buffer,
  // which is what a grep pane or explorer will also be.
  CHECK(CommandExecLine(&f.ed, Str8Lit("commands")));

  Buffer *listing = EditorFocusedBuffer(&f.ed);
  CHECK_STR(listing->name, Str8Lit("[commands]"));
  CHECK_EQ((u32)listing->kind, (u32)BufferKind::Scratch);
  CHECK(BufferIsReadOnly(listing));
  CHECK(BufferLineCount(listing) > 10);

  // It contains what it claims to.
  String8 text = BufferTextAll(f.arena, listing);
  CHECK(Str8FindFirst(text, Str8Lit("split-vertical")) < text.size);
  CHECK(Str8FindFirst(text, Str8Lit("write")) < text.size);

  Destroy(&f);
}

TEST(listing_buffers_and_bindings) {
  Fixture f = MakeFixture("hello");

  CHECK(CommandExecLine(&f.ed, Str8Lit("buffers")));
  String8 buffers = BufferTextAll(f.arena, EditorFocusedBuffer(&f.ed));
  CHECK(Str8FindFirst(buffers, Str8Lit("[command]")) < buffers.size);

  CHECK(CommandExecLine(&f.ed, Str8Lit("bindings")));
  String8 bindings = BufferTextAll(f.arena, EditorFocusedBuffer(&f.ed));
  CHECK(Str8FindFirst(bindings, Str8Lit("normal")) < bindings.size);
  // A binding installed by default must show up with its command name.
  CHECK(Str8FindFirst(bindings, Str8Lit("split-vertical")) < bindings.size);

  Destroy(&f);
}

TEST(read_only_buffer_rejects_edits) {
  Fixture f = MakeFixture("hello");

  CHECK(CommandExecLine(&f.ed, Str8Lit("commands")));
  Buffer *listing = EditorFocusedBuffer(&f.ed);
  u64 size_before = BufferSize(listing);

  // A listing is read-only, so normal editing keys must not damage it.
  EditorProcessSpec(&f.ed, "dd");
  EditorProcessSpec(&f.ed, "x");
  CHECK_EQ(BufferSize(listing), size_before);

  Destroy(&f);
}

TEST(command_and_keybinding_reach_the_same_code) {
  Fixture f = MakeFixture("one\ntwo\nthree");

  // Via the command window.
  CHECK(CommandExecLine(&f.ed, Str8Lit("split-vertical")));
  CHECK_EQ(PanelLeafCount(f.ed.root_panel), 2);

  // Via a keybinding. Same command id, same body.
  EditorProcessSpec(&f.ed, "<C-w>v");
  CHECK_EQ(PanelLeafCount(f.ed.root_panel), 3);

  // And the editing commands agree too.
  Fixture g = MakeFixture("a\nb\nc");
  CHECK(CommandExecLine(&g.ed, Str8Lit("1d")));
  Fixture h = MakeFixture("a\nb\nc");
  EditorProcessSpec(&h.ed, "dd");
  CHECK_STR(TextOf(&g), TextOf(&h));
  Destroy(&g);
  Destroy(&h);

  Destroy(&f);
}
