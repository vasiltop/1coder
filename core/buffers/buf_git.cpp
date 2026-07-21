#include "buffers/buf_git.h"

#include "editor/command.h"
#include "editor/editor.h"
#include "os/os_file.h"

// Git status / log / diff as ordinary buffers. Structured line metadata sits
// beside the rendered text so s/u/TAB know what the cursor is on, the same way
// the explorer ties ids to listing lines.

namespace {

enum class GitViewMode : u8 { Status, Log, Diff };

enum class GitLineKind : u8 {
  Other = 0,
  Section,
  File,
  HunkHeader,
  HunkLine,
  LogEntry,
};

struct GitLineInfo {
  GitLineKind kind;
  GitEntryKind entry_kind;  // for File / Hunk*
  String8 path;
  u64 hunk_index;  // into expanded diff for this path
  String8 hash;    // log
  bool expanded;
};

struct GitExpandedFile {
  String8 path;
  GitEntryKind entry_kind;
  GitDiff diff;  // usually one file
  bool open;
};

struct GitPayload {
  GitViewMode mode;
  String8 root;
  GitFlags flags;

  Arena *data_arena;  // cleared on reload
  GitStatus status;
  GitLog log;

  GitLineInfo *lines;
  u64 line_count;

  GitExpandedFile *expanded;
  u64 expanded_count;
  u64 expanded_capacity;

  // Pending discard confirmation.
  String8 pending_discard_path;
  GitEntryKind pending_discard_kind;
  Arena *pending_arena;
};

struct GitTokenBuilder {
  Token *tokens;
  u64 count;
  u64 capacity;
};

void AddToken(GitTokenBuilder *builder, u64 start, u64 end, TokenKind kind) {
  if (start >= end || builder->count >= builder->capacity) return;
  builder->tokens[builder->count++] = Token{start, end, kind};
}

u64 FindByte(const Buffer *buffer, u64 start, u64 end, u8 needle) {
  for (u64 offset = start; offset < end; offset += 1) {
    if (BufferByteAt(buffer, offset) == needle) return offset;
  }
  return end;
}

bool LineStartsWith(const Buffer *buffer, RangeU64 range, String8 prefix) {
  if (range.max - range.min < prefix.size) return false;
  for (u64 i = 0; i < prefix.size; i += 1) {
    if (BufferByteAt(buffer, range.min + i) != prefix.str[i]) return false;
  }
  return true;
}

void HighlightLabel(GitTokenBuilder *builder, const Buffer *buffer, RangeU64 range,
                    TokenKind value_kind) {
  u64 colon = FindByte(buffer, range.min, range.max, ':');
  if (colon == range.max) return;
  AddToken(builder, range.min, colon + 1, TokenKind::Keyword);
  u64 value = colon + 1;
  while (value < range.max && BufferByteAt(buffer, value) == ' ') value += 1;
  AddToken(builder, value, range.max, value_kind);
}

void HighlightStatusLine(GitTokenBuilder *builder, const Buffer *buffer, RangeU64 range,
                         const GitLineInfo *info) {
  if (info->kind == GitLineKind::Section) {
    u64 count = FindByte(buffer, range.min, range.max, '(');
    AddToken(builder, range.min, count < range.max ? count : range.max, TokenKind::Keyword);
    AddToken(builder, count, range.max, TokenKind::Number);
    return;
  }

  if (info->kind == GitLineKind::File) {
    u64 marker = Min(range.min + 2, range.max);
    AddToken(builder, marker, Min(marker + 1, range.max), TokenKind::Operator);
    u64 state = Min(range.min + 4, range.max);
    TokenKind state_kind = info->entry_kind == GitEntryKind::Unstaged ? TokenKind::Error
                                                                      : TokenKind::Constant;
    AddToken(builder, state, Min(state + 2, range.max), state_kind);
    u64 path = Min(state + 4, range.max);
    AddToken(builder, path, range.max, TokenKind::String);
    return;
  }

  if (info->kind == GitLineKind::HunkHeader) {
    AddToken(builder, Min(range.min + 4, range.max), range.max, TokenKind::Preprocessor);
    return;
  }

  if (info->kind == GitLineKind::HunkLine) {
    u64 content = Min(range.min + 4, range.max);
    if (content == range.max) return;
    u8 prefix = BufferByteAt(buffer, content);
    TokenKind kind = TokenKind::Comment;
    if (prefix == '+') kind = TokenKind::String;
    if (prefix == '-') kind = TokenKind::Error;
    AddToken(builder, content, range.max, kind);
    return;
  }

  if (LineStartsWith(buffer, range, Str8Lit("Head:"))) {
    HighlightLabel(builder, buffer, range, TokenKind::String);
  } else if (LineStartsWith(buffer, range, Str8Lit("Away:"))) {
    HighlightLabel(builder, buffer, range, TokenKind::Number);
  } else if (LineStartsWith(buffer, range, Str8Lit("Args:"))) {
    HighlightLabel(builder, buffer, range, TokenKind::Operator);
  } else if (LineStartsWith(buffer, range, Str8Lit("Help:"))) {
    HighlightLabel(builder, buffer, range, TokenKind::Comment);
  }
}

void HighlightLogLine(GitTokenBuilder *builder, const Buffer *buffer, RangeU64 range,
                      const GitLineInfo *info) {
  if (info->kind != GitLineKind::LogEntry) {
    AddToken(builder, range.min, range.max, TokenKind::Keyword);
    return;
  }
  u64 hash_end = Min(range.min + info->hash.size, range.max);
  AddToken(builder, range.min, hash_end, TokenKind::Constant);
  u64 subject = hash_end;
  while (subject < range.max && BufferByteAt(buffer, subject) == ' ') subject += 1;
  AddToken(builder, subject, range.max, TokenKind::Function);
}

void HighlightDiffLine(GitTokenBuilder *builder, const Buffer *buffer, RangeU64 range) {
  if (range.min == range.max) return;
  if (LineStartsWith(buffer, range, Str8Lit("@@"))) {
    AddToken(builder, range.min, range.max, TokenKind::Preprocessor);
  } else if (LineStartsWith(buffer, range, Str8Lit("+++"))) {
    AddToken(builder, range.min, range.max, TokenKind::String);
  } else if (LineStartsWith(buffer, range, Str8Lit("---"))) {
    AddToken(builder, range.min, range.max, TokenKind::Error);
  } else if (BufferByteAt(buffer, range.min) == '+') {
    AddToken(builder, range.min, range.max, TokenKind::String);
  } else if (BufferByteAt(buffer, range.min) == '-') {
    AddToken(builder, range.min, range.max, TokenKind::Error);
  } else if (LineStartsWith(buffer, range, Str8Lit("diff --git"))) {
    AddToken(builder, range.min, range.max, TokenKind::Keyword);
  } else if (LineStartsWith(buffer, range, Str8Lit("index "))) {
    AddToken(builder, range.min, range.max, TokenKind::Comment);
  } else if (LineStartsWith(buffer, range, Str8Lit("commit "))) {
    u64 word_end = Min(range.min + 6, range.max);
    AddToken(builder, range.min, word_end, TokenKind::Keyword);
    u64 hash = word_end;
    while (hash < range.max && BufferByteAt(buffer, hash) == ' ') hash += 1;
    AddToken(builder, hash, range.max, TokenKind::Constant);
  } else if (LineStartsWith(buffer, range, Str8Lit("Author:")) ||
             LineStartsWith(buffer, range, Str8Lit("Date:"))) {
    HighlightLabel(builder, buffer, range, TokenKind::String);
  }
}

void HighlightGitBuffer(Buffer *buffer, GitPayload *payload) {
  u64 line_count = BufferLineCount(buffer);
  GitTokenBuilder builder = {};
  builder.capacity = line_count * 4;
  builder.tokens = PushArray(payload->data_arena, Token, builder.capacity);

  for (u64 line = 0; line < line_count; line += 1) {
    RangeU64 range = BufferLineRange(buffer, line);
    if (payload->mode == GitViewMode::Status && line < payload->line_count) {
      HighlightStatusLine(&builder, buffer, range, &payload->lines[line]);
    } else if (payload->mode == GitViewMode::Log && line < payload->line_count) {
      HighlightLogLine(&builder, buffer, range, &payload->lines[line]);
    } else if (payload->mode == GitViewMode::Diff) {
      HighlightDiffLine(&builder, buffer, range);
    }
  }

  buffer->tokens = TokenArray{builder.tokens, builder.count};
}

GitPayload *PayloadOf(Buffer *buffer) {
  if (!buffer || buffer->kind != BufferKind::Git) return nullptr;
  return (GitPayload *)buffer->user_data;
}

const GitPayload *PayloadOf(const Buffer *buffer) {
  if (!buffer || buffer->kind != BufferKind::Git) return nullptr;
  return (const GitPayload *)buffer->user_data;
}

void GitOnClose(Editor *ed, Buffer *buffer) {
  (void)ed;
  GitPayload *payload = PayloadOf(buffer);
  if (!payload) return;
  if (payload->data_arena) ArenaRelease(payload->data_arena);
  if (payload->pending_arena) ArenaRelease(payload->pending_arena);
  payload->data_arena = nullptr;
  payload->pending_arena = nullptr;
}

void GitOnActivate(Editor *ed, Buffer *buffer, View *view) {
  (void)view;
  GitPayload *payload = PayloadOf(buffer);
  if (!payload || payload->mode != GitViewMode::Status) return;
  if (ed->input.awaiting_confirm) return;
  GitBufferReload(ed, buffer);
}

String8 ResolveRoot(Editor *ed) {
  TempArena scratch = ScratchBegin();
  String8 start = ed->cwd;
  View *view = EditorFocusedView(ed);
  Buffer *buf = EditorBufferForView(ed, view);
  if (buf && buf->path.size > 0) start = buf->path;
  // Prefer an existing git buffer's root when nesting from status.
  if (buf && buf->kind == BufferKind::Git) {
    String8 root = GitBufferRoot(buf);
    if (root.size > 0) {
      String8 copy = PushStr8Copy(ed->arena, root);
      ScratchEnd(scratch);
      return copy;
    }
  }
  String8 root = GitFindRoot(scratch.arena, start);
  String8 copy = root.size ? PushStr8Copy(ed->arena, root) : String8{};
  ScratchEnd(scratch);
  return copy;
}

GitExpandedFile *FindExpanded(GitPayload *payload, String8 path, GitEntryKind kind) {
  for (u64 i = 0; i < payload->expanded_count; i += 1) {
    if (payload->expanded[i].entry_kind == kind && Str8Match(payload->expanded[i].path, path)) {
      return &payload->expanded[i];
    }
  }
  return nullptr;
}

void EnsureExpandedCapacity(GitPayload *payload, u64 need) {
  if (need <= payload->expanded_capacity) return;
  u64 cap = Max(payload->expanded_capacity * 2, (u64)8);
  while (cap < need) cap *= 2;
  GitExpandedFile *fresh = PushArray(payload->data_arena, GitExpandedFile, cap);
  for (u64 i = 0; i < payload->expanded_count; i += 1) fresh[i] = payload->expanded[i];
  payload->expanded = fresh;
  payload->expanded_capacity = cap;
}

void ShowSideBuffer(Editor *ed, View *origin, BufferHandle handle) {
  if (!ed || handle.index == 0) return;

  Panel *first = PanelFirstLeaf(ed->root_panel);
  for (Panel *leaf = first; leaf;) {
    if (leaf->view && BufferHandleEqual(leaf->view->buffer, handle)) {
      EditorPushJump(ed, origin);
      EditorFocusPanel(ed, leaf);
      return;
    }
    leaf = PanelNextLeaf(ed->root_panel, leaf);
    if (leaf == first) break;
  }

  EditorPushJump(ed, origin);
  if (!EditorSplit(ed, Axis2::X)) {
    EditorShowBuffer(ed, handle);
    return;
  }
  EditorShowBuffer(ed, handle);
}

void ReportGitResult(Editor *ed, const GitResult *result, String8 ok_msg) {
  if (!result->started) {
    EditorSetStatus(ed, result->stderr_text.size ? result->stderr_text : Str8Lit("git failed"));
    return;
  }
  if (result->exit_code != 0) {
    String8 err = result->stderr_text.size ? result->stderr_text : result->stdout_text;
    if (err.size == 0) {
      EditorSetStatusF(ed, "git exited %d", (int)result->exit_code);
    } else {
      // First line only for the status bar.
      u64 nl = Str8FindFirstChar(err, '\n');
      String8 first = (nl < err.size) ? Str8Prefix(err, nl) : err;
      EditorSetStatusF(ed, "%.*s", (int)first.size, (char *)first.str);
    }
    return;
  }
  EditorSetStatus(ed, ok_msg);
}

void AppendLine(Arena *arena, String8List *text, GitLineInfo **infos, u64 *count, u64 *cap,
                String8 line, GitLineInfo info) {
  if (*count + 1 > *cap) {
    u64 ncap = Max(*cap * 2, (u64)64);
    GitLineInfo *fresh = PushArray(arena, GitLineInfo, ncap);
    for (u64 i = 0; i < *count; i += 1) fresh[i] = (*infos)[i];
    *infos = fresh;
    *cap = ncap;
  }
  (*infos)[*count] = info;
  *count += 1;
  Str8ListPush(arena, text, line);
  Str8ListPush(arena, text, Str8Lit("\n"));
}

void RenderStatus(Editor *ed, Buffer *buffer, GitPayload *payload) {
  Arena *arena = payload->data_arena;
  String8List text = {};
  GitLineInfo *infos = nullptr;
  u64 count = 0;
  u64 cap = 0;

  auto push = [&](String8 line, GitLineInfo info) {
    AppendLine(arena, &text, &infos, &count, &cap, line, info);
  };

  GitBranchInfo *b = &payload->status.branch;
  String8 head_line;
  if (b->detached) {
    head_line = PushStr8F(arena, "Head: %.*s (detached)", (int)b->head.size, (char *)b->head.str);
  } else if (b->upstream.size > 0) {
    head_line = PushStr8F(arena, "Head: %.*s...%.*s", (int)b->head.size, (char *)b->head.str,
                          (int)b->upstream.size, (char *)b->upstream.str);
  } else {
    head_line = PushStr8F(arena, "Head: %.*s", (int)b->head.size, (char *)b->head.str);
  }
  push(head_line, GitLineInfo{});

  if (b->ahead != 0 || b->behind != 0) {
    push(PushStr8F(arena, "Away: %d ahead, %d behind", (int)b->ahead, (int)b->behind),
         GitLineInfo{});
  }

  String8 args = GitFlagsRender(arena, payload->flags);
  push(PushStr8F(arena, "Args: %.*s", (int)args.size, (char *)args.str), GitLineInfo{});
  push(PushStr8Copy(arena, Str8Lit("Help: s/u stage  TAB hunks  c commit  F/P pull/push  r/a/f/U args")),
       GitLineInfo{});
  push(String8{}, GitLineInfo{});

  auto section = [&](String8 title, GitEntryKind kind) {
    u64 n = 0;
    for (u64 i = 0; i < payload->status.count; i += 1) {
      if (payload->status.entries[i].kind == kind) n += 1;
    }
    GitLineInfo info = {};
    info.kind = GitLineKind::Section;
    info.entry_kind = kind;
    push(PushStr8F(arena, "%.*s (%llu)", (int)title.size, (char *)title.str, (unsigned long long)n),
         info);

    for (u64 i = 0; i < payload->status.count; i += 1) {
      GitStatusEntry *e = &payload->status.entries[i];
      if (e->kind != kind) continue;

      GitExpandedFile *exp = FindExpanded(payload, e->path, kind);
      bool open = exp && exp->open;

      GitLineInfo finfo = {};
      finfo.kind = GitLineKind::File;
      finfo.entry_kind = kind;
      finfo.path = e->path;
      finfo.expanded = open;

      char mark = open ? 'v' : '>';
      push(PushStr8F(arena, "  %c %.*s  %.*s", mark, (int)e->xy.size, (char *)e->xy.str,
                     (int)e->path.size, (char *)e->path.str),
           finfo);

      if (!open || !exp) continue;

      for (u64 fi = 0; fi < exp->diff.file_count; fi += 1) {
        GitDiffFile *file = &exp->diff.files[fi];
        for (u64 hi = 0; hi < file->hunk_count; hi += 1) {
          GitHunk *hunk = &file->hunks[hi];
          GitLineInfo hinfo = {};
          hinfo.kind = GitLineKind::HunkHeader;
          hinfo.entry_kind = kind;
          hinfo.path = e->path;
          hinfo.hunk_index = hi;
          push(PushStr8F(arena, "    %.*s", (int)hunk->header.size, (char *)hunk->header.str),
               hinfo);

          String8List body_lines = Str8SplitChar(arena, hunk->body, '\n');
          for (String8Node *node = body_lines.first; node; node = node->next) {
            if (node->string.size == 0 && !node->next) continue;
            GitLineInfo linfo = hinfo;
            linfo.kind = GitLineKind::HunkLine;
            push(PushStr8F(arena, "    %.*s", (int)node->string.size, (char *)node->string.str),
                 linfo);
          }
        }
      }
    }
  };

  section(Str8Lit("Untracked files"), GitEntryKind::Untracked);
  push(String8{}, GitLineInfo{});
  section(Str8Lit("Unstaged changes"), GitEntryKind::Unstaged);
  push(String8{}, GitLineInfo{});
  section(Str8Lit("Staged changes"), GitEntryKind::Staged);

  String8 body = Str8ListJoin(arena, &text, String8{});
  // Drop trailing newline that BufferSetText would treat oddly — keep final_newline.
  if (body.size > 0 && body.str[body.size - 1] == '\n') body = Str8Chop(body, 1);

  buffer->flags &= ~BufferFlags::ReadOnly;
  BufferSetText(ed, buffer, body);
  buffer->flags |= BufferFlags::ReadOnly;

  payload->lines = infos;
  payload->line_count = count;
  HighlightGitBuffer(buffer, payload);
}

void RenderLog(Editor *ed, Buffer *buffer, GitPayload *payload) {
  Arena *arena = payload->data_arena;
  String8List text = {};
  GitLineInfo *infos = nullptr;
  u64 count = 0;
  u64 cap = 0;

  AppendLine(arena, &text, &infos, &count, &cap,
             PushStr8Copy(arena, Str8Lit("Commits (Enter to show)")), GitLineInfo{});
  AppendLine(arena, &text, &infos, &count, &cap, String8{}, GitLineInfo{});

  for (u64 i = 0; i < payload->log.count; i += 1) {
    GitLogEntry *e = &payload->log.entries[i];
    GitLineInfo info = {};
    info.kind = GitLineKind::LogEntry;
    info.hash = e->hash;
    String8 line =
        PushStr8F(arena, "%.*s  %.*s", (int)e->hash.size, (char *)e->hash.str, (int)e->subject.size,
                  (char *)e->subject.str);
    AppendLine(arena, &text, &infos, &count, &cap, line, info);
  }

  String8 body = Str8ListJoin(arena, &text, String8{});
  if (body.size > 0 && body.str[body.size - 1] == '\n') body = Str8Chop(body, 1);

  buffer->flags &= ~BufferFlags::ReadOnly;
  BufferSetText(ed, buffer, body);
  buffer->flags |= BufferFlags::ReadOnly;
  payload->lines = infos;
  payload->line_count = count;
  HighlightGitBuffer(buffer, payload);
}

GitLineInfo *LineAtCursor(GitPayload *payload, View *view, Buffer *buffer) {
  if (!payload || !view || !buffer) return nullptr;
  u64 line = ViewCursorLine(view, buffer);
  if (line >= payload->line_count) return nullptr;
  return &payload->lines[line];
}

BufferHandle OpenNamedGit(Editor *ed, String8 name, GitViewMode mode, String8 root) {
  BufferHandle handle = BufferFromName(&ed->buffers, name);
  if (handle.index == 0) {
    handle = BufferOpen(&ed->buffers, BufferKind::Git, name);
  }
  Buffer *buffer = BufferFromHandle(&ed->buffers, handle);
  if (!buffer) return BufferHandleZero();

  GitPayload *payload = PayloadOf(buffer);
  if (!payload) {
    payload = PushStruct(buffer->arena, GitPayload);
    payload->data_arena = ArenaAlloc(MB(16));
    payload->pending_arena = ArenaAlloc(MB(1));
    buffer->user_data = payload;
    buffer->hooks.on_close = GitOnClose;
    buffer->hooks.on_activate = GitOnActivate;

    Keymap *keymap = KeymapAlloc(ed->arena, ed->normal_map);
    KeymapBind(keymap, "s", CommandId::git_stage);
    KeymapBind(keymap, "u", CommandId::git_unstage);
    KeymapBind(keymap, "x", CommandId::git_discard);
    KeymapBind(keymap, "<Tab>", CommandId::git_toggle);
    KeymapBind(keymap, "d", CommandId::git_diff);
    KeymapBind(keymap, "<CR>", CommandId::git_open);
    KeymapBind(keymap, "l", CommandId::git_log);
    KeymapBind(keymap, "c", CommandId::git_commit);
    KeymapBind(keymap, "R", CommandId::git_refresh);
    KeymapBind(keymap, "F", CommandId::git_pull);
    KeymapBind(keymap, "P", CommandId::git_push);
    KeymapBind(keymap, "r", CommandId::git_arg_rebase);
    KeymapBind(keymap, "a", CommandId::git_arg_autostash);
    KeymapBind(keymap, "f", CommandId::git_arg_ff_only);
    KeymapBind(keymap, "U", CommandId::git_arg_set_upstream);
    buffer->hooks.keymap = keymap;
  }

  payload->mode = mode;
  if (root.size > 0) payload->root = PushStr8Copy(buffer->arena, root);
  buffer->flags |= BufferFlags::ReadOnly;
  return handle;
}

}  // namespace

BufferHandle GitBufferOpenStatus(Editor *ed) {
  if (!ed) return BufferHandleZero();
  if (!GitAvailable(ed->arena)) {
    EditorSetStatus(ed, Str8Lit("git not found on PATH"));
    return BufferHandleZero();
  }

  String8 root = ResolveRoot(ed);
  if (root.size == 0) {
    EditorSetStatus(ed, Str8Lit("not a git repository"));
    return BufferHandleZero();
  }

  BufferHandle handle = OpenNamedGit(ed, Str8Lit("[git]"), GitViewMode::Status, root);
  Buffer *buffer = BufferFromHandle(&ed->buffers, handle);
  if (buffer) GitBufferReload(ed, buffer);
  return handle;
}

BufferHandle GitBufferOpenLog(Editor *ed) {
  if (!ed) return BufferHandleZero();
  String8 root = ResolveRoot(ed);
  if (root.size == 0) {
    EditorSetStatus(ed, Str8Lit("not a git repository"));
    return BufferHandleZero();
  }
  BufferHandle handle = OpenNamedGit(ed, Str8Lit("[git-log]"), GitViewMode::Log, root);
  Buffer *buffer = BufferFromHandle(&ed->buffers, handle);
  if (buffer) GitBufferReload(ed, buffer);
  return handle;
}

BufferHandle GitBufferOpenDiff(Editor *ed, String8 title, String8 body) {
  if (!ed) return BufferHandleZero();
  String8 root = ResolveRoot(ed);
  BufferHandle handle = OpenNamedGit(ed, Str8Lit("[git-diff]"), GitViewMode::Diff, root);
  Buffer *buffer = BufferFromHandle(&ed->buffers, handle);
  if (!buffer) return BufferHandleZero();

  GitPayload *payload = PayloadOf(buffer);
  ArenaClear(payload->data_arena);
  payload->lines = nullptr;
  payload->line_count = 0;

  if (title.size > 0) {
    // Keep the buffer's registry name as [git-diff] so reuse works; the title
    // is informational only via the status line's first content line.
    (void)title;
  }

  buffer->flags &= ~BufferFlags::ReadOnly;
  BufferSetText(ed, buffer, body);
  buffer->flags |= BufferFlags::ReadOnly;
  HighlightGitBuffer(buffer, payload);
  return handle;
}

void GitBufferReload(Editor *ed, Buffer *buffer) {
  GitPayload *payload = PayloadOf(buffer);
  if (!payload) return;

  // Preserve which files were expanded across reload.
  struct SavedExpand {
    String8 path;
    GitEntryKind kind;
  };
  TempArena scratch = ScratchBegin();
  SavedExpand *saved = PushArray(scratch.arena, SavedExpand, payload->expanded_count);
  u64 saved_count = 0;
  for (u64 i = 0; i < payload->expanded_count; i += 1) {
    if (!payload->expanded[i].open) continue;
    saved[saved_count].path = PushStr8Copy(scratch.arena, payload->expanded[i].path);
    saved[saved_count].kind = payload->expanded[i].entry_kind;
    saved_count += 1;
  }

  GitFlags flags = payload->flags;
  String8 root = PushStr8Copy(scratch.arena, payload->root);
  ArenaClear(payload->data_arena);
  payload->flags = flags;
  payload->root = PushStr8Copy(payload->data_arena, root);
  payload->expanded = nullptr;
  payload->expanded_count = 0;
  payload->expanded_capacity = 0;
  payload->lines = nullptr;
  payload->line_count = 0;

  if (payload->mode == GitViewMode::Status) {
    payload->status = GitLoadStatus(payload->data_arena, payload->root);
    if (payload->status.error.size > 0) {
      EditorSetStatusF(ed, "%.*s", (int)payload->status.error.size,
                       (char *)payload->status.error.str);
    }

    // Re-expand previously open files.
    for (u64 i = 0; i < saved_count; i += 1) {
      EnsureExpandedCapacity(payload, payload->expanded_count + 1);
      GitExpandedFile *exp = &payload->expanded[payload->expanded_count++];
      exp->path = PushStr8Copy(payload->data_arena, saved[i].path);
      exp->entry_kind = saved[i].kind;
      exp->open = true;
      bool cached = saved[i].kind == GitEntryKind::Staged;
      if (saved[i].kind == GitEntryKind::Untracked) {
        exp->diff = {};
      } else {
        exp->diff = GitLoadDiff(payload->data_arena, payload->root, exp->path, cached);
      }
    }

    RenderStatus(ed, buffer, payload);
  } else if (payload->mode == GitViewMode::Log) {
    payload->log = GitLoadLog(payload->data_arena, payload->root, 100);
    RenderLog(ed, buffer, payload);
  }

  ScratchEnd(scratch);
}

String8 GitBufferRoot(const Buffer *buffer) {
  const GitPayload *payload = PayloadOf(buffer);
  return payload ? payload->root : String8{};
}

GitFlags GitBufferFlags(const Buffer *buffer) {
  const GitPayload *payload = PayloadOf(buffer);
  return payload ? payload->flags : GitFlags{};
}

void GitBufferSetFlags(Buffer *buffer, GitFlags flags) {
  GitPayload *payload = PayloadOf(buffer);
  if (payload) payload->flags = flags;
}

bool GitBufferToggleExpand(Editor *ed, Buffer *buffer, View *view) {
  GitPayload *payload = PayloadOf(buffer);
  GitLineInfo *info = LineAtCursor(payload, view, buffer);
  if (!payload || !info || info->kind != GitLineKind::File) return false;
  if (info->entry_kind == GitEntryKind::Untracked) {
    EditorSetStatus(ed, Str8Lit("no hunks for untracked files"));
    return false;
  }

  GitExpandedFile *exp = FindExpanded(payload, info->path, info->entry_kind);
  if (exp && exp->open) {
    exp->open = false;
  } else {
    if (!exp) {
      EnsureExpandedCapacity(payload, payload->expanded_count + 1);
      exp = &payload->expanded[payload->expanded_count++];
      exp->path = PushStr8Copy(payload->data_arena, info->path);
      exp->entry_kind = info->entry_kind;
    }
    exp->open = true;
    bool cached = info->entry_kind == GitEntryKind::Staged;
    exp->diff = GitLoadDiff(payload->data_arena, payload->root, exp->path, cached);
  }

  u64 line = ViewCursorLine(view, buffer);
  RenderStatus(ed, buffer, payload);
  ViewSetCursorLineColumn(view, buffer, Min(line, BufferLineCount(buffer) - 1), 0);
  return true;
}

bool GitBufferStageUnderCursor(Editor *ed, Buffer *buffer, View *view) {
  GitPayload *payload = PayloadOf(buffer);
  GitLineInfo *info = LineAtCursor(payload, view, buffer);
  if (!payload || !info) return false;

  TempArena scratch = ScratchBegin();
  GitResult result = {};

  if (info->kind == GitLineKind::File || info->kind == GitLineKind::Section) {
    if (info->kind == GitLineKind::Section) {
      ScratchEnd(scratch);
      return false;
    }
    if (info->entry_kind == GitEntryKind::Staged) {
      EditorSetStatus(ed, Str8Lit("already staged"));
      ScratchEnd(scratch);
      return false;
    }
    result = GitStagePath(scratch.arena, payload->root, info->path);
    ReportGitResult(ed, &result, Str8Lit("staged"));
  } else if (info->kind == GitLineKind::HunkHeader || info->kind == GitLineKind::HunkLine) {
    if (info->entry_kind != GitEntryKind::Unstaged) {
      EditorSetStatus(ed, Str8Lit("hunk is not unstaged"));
      ScratchEnd(scratch);
      return false;
    }
    GitExpandedFile *exp = FindExpanded(payload, info->path, info->entry_kind);
    if (!exp || exp->diff.file_count == 0) {
      ScratchEnd(scratch);
      return false;
    }
    GitDiffFile *file = &exp->diff.files[0];
    if (info->hunk_index >= file->hunk_count) {
      ScratchEnd(scratch);
      return false;
    }
    String8 patch = GitHunkPatch(scratch.arena, file, &file->hunks[info->hunk_index]);
    result = GitStageHunk(scratch.arena, payload->root, patch);
    ReportGitResult(ed, &result, Str8Lit("hunk staged"));
  } else {
    ScratchEnd(scratch);
    return false;
  }

  ScratchEnd(scratch);
  GitBufferReload(ed, buffer);
  return result.started && result.exit_code == 0;
}

bool GitBufferUnstageUnderCursor(Editor *ed, Buffer *buffer, View *view) {
  GitPayload *payload = PayloadOf(buffer);
  GitLineInfo *info = LineAtCursor(payload, view, buffer);
  if (!payload || !info) return false;

  TempArena scratch = ScratchBegin();
  GitResult result = {};

  if (info->kind == GitLineKind::File) {
    if (info->entry_kind != GitEntryKind::Staged) {
      EditorSetStatus(ed, Str8Lit("not staged"));
      ScratchEnd(scratch);
      return false;
    }
    result = GitUnstagePath(scratch.arena, payload->root, info->path);
    ReportGitResult(ed, &result, Str8Lit("unstaged"));
  } else if (info->kind == GitLineKind::HunkHeader || info->kind == GitLineKind::HunkLine) {
    if (info->entry_kind != GitEntryKind::Staged) {
      EditorSetStatus(ed, Str8Lit("hunk is not staged"));
      ScratchEnd(scratch);
      return false;
    }
    GitExpandedFile *exp = FindExpanded(payload, info->path, info->entry_kind);
    if (!exp || exp->diff.file_count == 0) {
      ScratchEnd(scratch);
      return false;
    }
    GitDiffFile *file = &exp->diff.files[0];
    if (info->hunk_index >= file->hunk_count) {
      ScratchEnd(scratch);
      return false;
    }
    String8 patch = GitHunkPatch(scratch.arena, file, &file->hunks[info->hunk_index]);
    result = GitUnstageHunk(scratch.arena, payload->root, patch);
    ReportGitResult(ed, &result, Str8Lit("hunk unstaged"));
  } else {
    ScratchEnd(scratch);
    return false;
  }

  ScratchEnd(scratch);
  GitBufferReload(ed, buffer);
  return result.started && result.exit_code == 0;
}

void GitBufferRequestDiscard(Editor *ed, Buffer *buffer, View *view) {
  GitPayload *payload = PayloadOf(buffer);
  GitLineInfo *info = LineAtCursor(payload, view, buffer);
  if (!payload || !info || info->kind != GitLineKind::File) {
    EditorSetStatus(ed, Str8Lit("discard: put cursor on a file"));
    return;
  }
  if (info->entry_kind == GitEntryKind::Staged) {
    EditorSetStatus(ed, Str8Lit("unstage before discarding"));
    return;
  }

  ArenaClear(payload->pending_arena);
  payload->pending_discard_path = PushStr8Copy(payload->pending_arena, info->path);
  payload->pending_discard_kind = info->entry_kind;
  EditorSetStatusF(ed, "Discard %.*s? [y/N]", (int)info->path.size, (char *)info->path.str);
  EditorAwaitConfirm(ed, CommandId::git_discard_apply, buffer->handle);
}

void GitBufferApplyDiscard(Editor *ed, Buffer *buffer) {
  GitPayload *payload = PayloadOf(buffer);
  if (!payload || payload->pending_discard_path.size == 0) return;

  TempArena scratch = ScratchBegin();
  GitResult result =
      GitDiscardPath(scratch.arena, payload->root, payload->pending_discard_path);
  ReportGitResult(ed, &result, Str8Lit("discarded"));
  payload->pending_discard_path = String8{};
  ScratchEnd(scratch);
  GitBufferReload(ed, buffer);
}

bool GitBufferDiscardUnderCursor(Editor *ed, Buffer *buffer, View *view) {
  GitBufferRequestDiscard(ed, buffer, view);
  return true;
}

bool GitBufferOpenUnderCursor(Editor *ed, Buffer *buffer, View *view) {
  GitPayload *payload = PayloadOf(buffer);
  GitLineInfo *info = LineAtCursor(payload, view, buffer);
  if (!payload || !info) return false;

  if (info->kind == GitLineKind::LogEntry && info->hash.size > 0) {
    TempArena scratch = ScratchBegin();
    String8 body = GitShow(scratch.arena, payload->root, info->hash);
    String8 title =
        PushStr8F(scratch.arena, "[git-diff %.*s]", (int)info->hash.size, (char *)info->hash.str);
    BufferHandle handle = GitBufferOpenDiff(ed, title, body);
    ScratchEnd(scratch);
    ShowSideBuffer(ed, view, handle);
    return true;
  }

  if (info->kind == GitLineKind::File && info->path.size > 0) {
    TempArena scratch = ScratchBegin();
    String8 abs = OsPathJoin(scratch.arena, payload->root, info->path);
    // Strip trailing slash for directories.
    while (abs.size > 1 && abs.str[abs.size - 1] == '/') abs = Str8Chop(abs, 1);
    BufferHandle handle = EditorOpenFile(ed, abs);
    ScratchEnd(scratch);
    if (handle.index == 0) return false;
    EditorPushJump(ed, view);
    EditorShowBuffer(ed, handle);
    return true;
  }

  return GitBufferDiffUnderCursor(ed, buffer, view);
}

bool GitBufferDiffUnderCursor(Editor *ed, Buffer *buffer, View *view) {
  GitPayload *payload = PayloadOf(buffer);
  GitLineInfo *info = LineAtCursor(payload, view, buffer);
  if (!payload) return false;

  TempArena scratch = ScratchBegin();
  String8 body = {};
  String8 title = Str8Lit("[git-diff]");

  if (info && info->path.size > 0 &&
      (info->kind == GitLineKind::File || info->kind == GitLineKind::HunkHeader ||
       info->kind == GitLineKind::HunkLine)) {
    bool cached = info->entry_kind == GitEntryKind::Staged;
    String8 args_label = cached ? Str8Lit("staged") : Str8Lit("unstaged");
    if (info->entry_kind == GitEntryKind::Untracked) {
      EditorSetStatus(ed, Str8Lit("no diff for untracked file"));
      ScratchEnd(scratch);
      return false;
    }
    String8 git_args_cached[] = {Str8Lit("diff"), Str8Lit("--cached"), Str8Lit("--"), info->path};
    String8 git_args[] = {Str8Lit("diff"), Str8Lit("--"), info->path};
    GitResult result =
        cached ? GitRun(scratch.arena, payload->root, git_args_cached, ArrayCount(git_args_cached))
               : GitRun(scratch.arena, payload->root, git_args, ArrayCount(git_args));
    body = result.stdout_text.size ? result.stdout_text : Str8Lit("(no diff)\n");
    title = PushStr8F(scratch.arena, "[git-diff %.*s %.*s]", (int)args_label.size,
                      (char *)args_label.str, (int)info->path.size, (char *)info->path.str);
  } else {
    // Whole unstaged + staged overview.
    String8 args[] = {Str8Lit("diff")};
    String8 args_c[] = {Str8Lit("diff"), Str8Lit("--cached")};
    GitResult u = GitRun(scratch.arena, payload->root, args, ArrayCount(args));
    GitResult s = GitRun(scratch.arena, payload->root, args_c, ArrayCount(args_c));
    String8List parts = {};
    Str8ListPush(scratch.arena, &parts, Str8Lit("# Unstaged\n"));
    Str8ListPush(scratch.arena, &parts, u.stdout_text.size ? u.stdout_text : Str8Lit("(none)\n"));
    Str8ListPush(scratch.arena, &parts, Str8Lit("\n# Staged\n"));
    Str8ListPush(scratch.arena, &parts, s.stdout_text.size ? s.stdout_text : Str8Lit("(none)\n"));
    body = Str8ListJoin(scratch.arena, &parts, String8{});
  }

  BufferHandle handle = GitBufferOpenDiff(ed, title, body);
  ScratchEnd(scratch);
  ShowSideBuffer(ed, view, handle);
  return true;
}

bool GitBufferToggleFlag(Editor *ed, Buffer *buffer, char which) {
  GitPayload *payload = PayloadOf(buffer);
  if (!payload || payload->mode != GitViewMode::Status) {
    // Allow toggling from any git buffer that shares status — prefer status.
    BufferHandle status = BufferFromName(&ed->buffers, Str8Lit("[git]"));
    buffer = BufferFromHandle(&ed->buffers, status);
    payload = PayloadOf(buffer);
    if (!payload) return false;
  }

  switch (which) {
    case 'r':
      payload->flags.rebase = !payload->flags.rebase;
      break;
    case 'a':
      payload->flags.autostash = !payload->flags.autostash;
      break;
    case 'f':
      payload->flags.ff_only = !payload->flags.ff_only;
      break;
    case 'U':
      payload->flags.set_upstream = !payload->flags.set_upstream;
      break;
    default:
      return false;
  }

  TempArena scratch = ScratchBegin();
  String8 args = GitFlagsRender(scratch.arena, payload->flags);
  EditorSetStatusF(ed, "Args: %.*s", (int)args.size, (char *)args.str);
  ScratchEnd(scratch);

  if (payload->mode == GitViewMode::Status) RenderStatus(ed, buffer, payload);
  return true;
}

bool GitBufferPull(Editor *ed, Buffer *buffer) {
  GitPayload *payload = PayloadOf(buffer);
  if (!payload) {
    BufferHandle status = GitBufferOpenStatus(ed);
    buffer = BufferFromHandle(&ed->buffers, status);
    payload = PayloadOf(buffer);
  }
  if (!payload) return false;

  TempArena scratch = ScratchBegin();
  GitResult result = GitPull(scratch.arena, payload->root, payload->flags);
  String8 msg = PushStr8F(scratch.arena, "pulled%s", payload->flags.rebase ? " (--rebase)" : "");
  ReportGitResult(ed, &result, msg);
  ScratchEnd(scratch);
  GitBufferReload(ed, buffer);
  return result.started && result.exit_code == 0;
}

bool GitBufferPush(Editor *ed, Buffer *buffer) {
  GitPayload *payload = PayloadOf(buffer);
  if (!payload) {
    BufferHandle status = GitBufferOpenStatus(ed);
    buffer = BufferFromHandle(&ed->buffers, status);
    payload = PayloadOf(buffer);
  }
  if (!payload) return false;

  TempArena scratch = ScratchBegin();
  GitResult result = GitPush(scratch.arena, payload->root, payload->flags);
  ReportGitResult(ed, &result, Str8Lit("pushed"));
  ScratchEnd(scratch);
  GitBufferReload(ed, buffer);
  return result.started && result.exit_code == 0;
}

bool GitBufferCommit(Editor *ed, Buffer *buffer, String8 message) {
  GitPayload *payload = PayloadOf(buffer);
  if (!payload) {
    BufferHandle status = BufferFromName(&ed->buffers, Str8Lit("[git]"));
    buffer = BufferFromHandle(&ed->buffers, status);
    payload = PayloadOf(buffer);
  }
  if (!payload) {
    EditorSetStatus(ed, Str8Lit("open :git first"));
    return false;
  }

  message = Str8SkipChopWhitespace(message);
  if (message.size == 0) {
    EditorSetStatus(ed, Str8Lit("commit: empty message"));
    return false;
  }

  TempArena scratch = ScratchBegin();
  GitResult result = GitCommit(scratch.arena, payload->root, message);
  ReportGitResult(ed, &result, Str8Lit("committed"));
  ScratchEnd(scratch);
  GitBufferReload(ed, buffer);
  return result.started && result.exit_code == 0;
}
