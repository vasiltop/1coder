#include "editor/command.h"
#include "os/os_file.h"

// Parsing and completion for the command window. Kept apart from the command
// bodies in command.cpp so that "what did the user type" and "what does each
// command do" stay separable -- the parser is pure and has no side effects,
// which is what lets the window show errors and suggestions as you type.

namespace {

[[nodiscard]] bool IsNameChar(u8 c) {
  return CharIsAlpha(c) || CharIsDigit(c) || c == '-' || c == '_';
}

// Parses one endpoint of a range: a number, ".", "$", "'<", "'>", or a
// "+N"/"-N" offset from the current line. Returns false if there is none.
bool ParseRangeEndpoint(String8 line, u64 *offset, const Buffer *buffer, const View *view,
                        u64 *out_line, bool *bad) {
  u64 at = *offset;
  if (at >= line.size) return false;

  u64 line_count = buffer ? BufferLineCount(buffer) : 1;
  u64 current = (buffer && view) ? BufferLineFromOffset(buffer, view->cursor) : 0;

  i64 base = -1;

  if (line.str[at] == '.') {
    base = (i64)current;
    at += 1;
  } else if (line.str[at] == '$') {
    base = (i64)line_count - 1;
    at += 1;
  } else if (line.str[at] == '\'' && at + 1 < line.size &&
             (line.str[at + 1] == '<' || line.str[at + 1] == '>')) {
    // The visual selection's bounds, which vim inserts automatically when you
    // press ':' with a selection active.
    if (buffer && view) {
      RangeU64 selection = ViewSelection(view, buffer);
      u64 first = BufferLineFromOffset(buffer, selection.min);
      u64 last = BufferLineFromOffset(buffer,
                                      (selection.max > selection.min) ? selection.max - 1
                                                                      : selection.min);
      base = (i64)((line.str[at + 1] == '<') ? first : last);
    } else {
      base = (i64)current;
    }
    at += 2;
  } else if (CharIsDigit(line.str[at])) {
    u64 value = 0;
    while (at < line.size && CharIsDigit(line.str[at])) {
      value = value * 10 + (u64)(line.str[at] - '0');
      at += 1;
    }
    // Line numbers are 1-based as displayed, 0-based internally.
    base = (value > 0) ? (i64)value - 1 : 0;
  } else if (line.str[at] == '+' || line.str[at] == '-') {
    base = (i64)current;
  } else {
    return false;
  }

  // Optional relative adjustment, so ".,+3" and "$-2" work.
  while (at < line.size && (line.str[at] == '+' || line.str[at] == '-')) {
    bool negative = (line.str[at] == '-');
    at += 1;

    u64 amount = 0;
    bool had_digits = false;
    while (at < line.size && CharIsDigit(line.str[at])) {
      amount = amount * 10 + (u64)(line.str[at] - '0');
      at += 1;
      had_digits = true;
    }
    if (!had_digits) amount = 1;  // bare "+" means one line

    base += negative ? -(i64)amount : (i64)amount;
  }

  if (base < 0) base = 0;
  if (buffer && (u64)base >= line_count) {
    *bad = true;
    base = (i64)line_count - 1;
  }

  *out_line = (u64)base;
  *offset = at;
  return true;
}

// Unquotes one argument, honouring double quotes and backslash escapes.
String8 UnquoteFirst(Arena *arena, String8 text) {
  if (text.size == 0) return text;

  if (text.str[0] != '"') {
    u64 end = Str8FindFirstChar(text, ' ');
    return Str8Prefix(text, end);
  }

  u8 *out = PushArrayNoZero(arena, u8, text.size + 1);
  u64 length = 0;
  for (u64 i = 1; i < text.size; i += 1) {
    u8 c = text.str[i];
    if (c == '\\' && i + 1 < text.size) {
      i += 1;
      out[length++] = text.str[i];
      continue;
    }
    if (c == '"') break;
    out[length++] = c;
  }
  out[length] = 0;
  return String8{out, length};
}

// Start of the word the cursor sits in, for completion.
[[nodiscard]] u64 WordStart(String8 line) {
  u64 at = line.size;
  while (at > 0 && line.str[at - 1] != ' ') at -= 1;
  return at;
}

}  // namespace

CommandParse CommandParseLine(Arena *arena, String8 line, const Buffer *buffer,
                              const View *view) {
  CommandParse parse = {};

  String8 trimmed = Str8SkipChopWhitespace(line);
  if (trimmed.size == 0) {
    parse.status = CommandParseStatus::Empty;
    return parse;
  }

  u64 at = 0;

  // ---- range ----
  if (trimmed.str[0] == '%') {
    // The whole buffer.
    parse.has_range = true;
    parse.line_first = 0;
    parse.line_last = buffer ? BufferLineCount(buffer) - 1 : 0;
    at = 1;
  } else {
    bool bad = false;
    u64 first = 0;
    if (ParseRangeEndpoint(trimmed, &at, buffer, view, &first, &bad)) {
      parse.has_range = true;
      parse.line_first = first;
      parse.line_last = first;

      if (at < trimmed.size && trimmed.str[at] == ',') {
        at += 1;
        u64 last = 0;
        if (!ParseRangeEndpoint(trimmed, &at, buffer, view, &last, &bad)) {
          parse.status = CommandParseStatus::BadRange;
          return parse;
        }
        parse.line_last = last;
      }
      if (bad) {
        parse.status = CommandParseStatus::BadRange;
        return parse;
      }
      // A backwards range is normalised rather than rejected.
      if (parse.line_first > parse.line_last) {
        u64 swap = parse.line_first;
        parse.line_first = parse.line_last;
        parse.line_last = swap;
      }
    }
  }

  while (at < trimmed.size && trimmed.str[at] == ' ') at += 1;

  // ---- name ----
  u64 name_start = at;
  while (at < trimmed.size && IsNameChar(trimmed.str[at])) at += 1;
  parse.name = Str8Substr(trimmed, RangeU64{name_start, at});

  // A bare range with no command means "go to that line", as vim does.
  if (parse.name.size == 0) {
    if (parse.has_range) {
      parse.id = CommandId::goto_line;
      parse.status = CommandParseStatus::Ok;
      return parse;
    }
    parse.status = CommandParseStatus::UnknownCommand;
    return parse;
  }

  // ---- bang ----
  if (at < trimmed.size && trimmed.str[at] == '!') {
    parse.bang = true;
    at += 1;
  }

  // ---- arguments ----
  parse.text = Str8SkipChopWhitespace(Str8Skip(trimmed, at));
  parse.arg = UnquoteFirst(arena, parse.text);

  // ---- resolve and validate against the command's declared shape ----
  parse.id = CommandIdResolve(parse.name);
  if (parse.id == CommandId::None) {
    // Distinguish "no such command" from "you have not typed enough yet", so
    // the window can say something useful.
    CommandIdList candidates = CommandIdsWithPrefix(arena, parse.name);
    parse.status = (candidates.count > 1) ? CommandParseStatus::AmbiguousPrefix
                                          : CommandParseStatus::UnknownCommand;
    return parse;
  }

  const CommandSpec *spec = CommandSpecFromId(parse.id);
  if (parse.bang && !HasFlag(spec->flags, CommandFlags::Bang)) {
    parse.status = CommandParseStatus::UnexpectedBang;
    return parse;
  }
  if (parse.has_range && !HasFlag(spec->flags, CommandFlags::Range)) {
    parse.status = CommandParseStatus::UnexpectedRange;
    return parse;
  }
  if (HasFlag(spec->flags, CommandFlags::RequiresArg) && parse.arg.size == 0) {
    parse.status = CommandParseStatus::MissingArgument;
    return parse;
  }

  parse.status = CommandParseStatus::Ok;
  return parse;
}

String8 CommandParseStatusMessage(Arena *arena, const CommandParse *parse) {
  String8 name = parse->name;

  switch (parse->status) {
    case CommandParseStatus::Ok:
    case CommandParseStatus::Empty:
      return String8{nullptr, 0};
    case CommandParseStatus::UnknownCommand:
      return PushStr8F(arena, "Not a command: %.*s", (int)name.size, (char *)name.str);
    case CommandParseStatus::AmbiguousPrefix:
      return PushStr8F(arena, "Ambiguous command: %.*s", (int)name.size, (char *)name.str);
    case CommandParseStatus::BadRange:
      return PushStr8Copy(arena, Str8Lit("Invalid range"));
    case CommandParseStatus::UnexpectedBang:
      return PushStr8F(arena, "%.*s takes no !", (int)name.size, (char *)name.str);
    case CommandParseStatus::UnexpectedRange:
      return PushStr8F(arena, "%.*s takes no range", (int)name.size, (char *)name.str);
    case CommandParseStatus::MissingArgument:
      return PushStr8F(arena, "%.*s needs an argument", (int)name.size, (char *)name.str);
    default:
      return PushStr8Copy(arena, Str8Lit("Bad command"));
  }
}

bool CommandExecLine(Editor *ed, String8 line) {
  TempArena scratch = ScratchBegin1(ed->arena);

  View *view = EditorFocusedView(ed);
  Buffer *buffer = EditorBufferForView(ed, view);

  CommandParse parse = CommandParseLine(scratch.arena, line, buffer, view);

  if (parse.status == CommandParseStatus::Empty) {
    ScratchEnd(scratch);
    return true;
  }
  if (parse.status != CommandParseStatus::Ok) {
    EditorSetStatus(ed, CommandParseStatusMessage(scratch.arena, &parse));
    ScratchEnd(scratch);
    return false;
  }

  CommandArgs args = {};
  args.text = parse.text;
  args.arg = parse.arg;
  args.bang = parse.bang;
  args.has_range = parse.has_range;
  args.line_first = parse.line_first;
  args.line_last = parse.line_last;

  CommandExecArgs(ed, parse.id, &args);

  ScratchEnd(scratch);
  return true;
}

CommandCompletion CommandCompletionsFor(Arena *arena, Editor *ed, String8 line) {
  CommandCompletion completion = {};
  completion.replace_start = WordStart(line);
  completion.replace_end = line.size;

  String8 word = Str8Substr(line, RangeU64{completion.replace_start, line.size});

  TempArena scratch = ScratchBegin1(arena);
  View *view = EditorFocusedView(ed);
  Buffer *buffer = EditorBufferForView(ed, view);
  CommandParse parse = CommandParseLine(scratch.arena, line, buffer, view);

  // Still typing the name -- that is, the word under the cursor is the name
  // itself -- so complete against command names.
  bool completing_name = (completion.replace_start <= line.size) &&
                         (parse.text.size == 0 || parse.name.size == 0 ||
                          Str8Match(word, parse.name));

  if (completing_name) {
    CommandIdList ids = CommandIdsWithPrefix(arena, word);
    completion.items = PushArray(arena, String8, Max(ids.count, (u64)1));
    for (u64 i = 0; i < ids.count; i += 1) {
      completion.items[i] = PushStr8Copy(arena, CommandName(ids.ids[i]));
    }
    completion.count = ids.count;
    ScratchEnd(scratch);
    return completion;
  }

  const CommandSpec *spec = CommandSpecFromId(parse.id);
  if (!spec) {
    ScratchEnd(scratch);
    return completion;
  }

  switch (spec->arg) {
    case CommandArg::Path: {
      // Split the typed word into the directory to list and the prefix to
      // match inside it.
      String8 dir = Str8PathDir(word);
      String8 leaf = Str8PathBase(word);
      String8 listing_dir = (dir.size > 0) ? dir : ed->cwd;

      FileList files = OsDirList(scratch.arena, listing_dir);
      completion.items = PushArray(arena, String8, Max(files.count, (u64)1));

      for (u64 i = 0; i < files.count; i += 1) {
        if (!Str8StartsWith(files.files[i].name, leaf)) continue;

        // Offer the whole path as typed so far, so accepting a suggestion does
        // not lose the directory the user already entered.
        String8 item = (dir.size > 0)
                           ? OsPathJoin(arena, dir, files.files[i].name)
                           : PushStr8Copy(arena, files.files[i].name);
        if (files.files[i].is_dir) item = PushStr8Cat(arena, item, Str8Lit("/"));
        completion.items[completion.count] = item;
        completion.count += 1;
      }
      break;
    }

    case CommandArg::BufferName: {
      completion.items = PushArray(arena, String8, kMaxBuffers);
      for (BufferHandle h = BufferFirst(&ed->buffers); h.index != 0;
           h = BufferNext(&ed->buffers, h)) {
        Buffer *b = BufferFromHandle(&ed->buffers, h);
        if (!b || !Str8StartsWith(b->name, word)) continue;
        completion.items[completion.count] = PushStr8Copy(arena, b->name);
        completion.count += 1;
      }
      break;
    }

    case CommandArg::CommandName: {
      CommandIdList ids = CommandIdsWithPrefix(arena, word);
      completion.items = PushArray(arena, String8, Max(ids.count, (u64)1));
      for (u64 i = 0; i < ids.count; i += 1) {
        completion.items[i] = PushStr8Copy(arena, CommandName(ids.ids[i]));
      }
      completion.count = ids.count;
      break;
    }

    default:
      break;
  }

  ScratchEnd(scratch);
  return completion;
}
