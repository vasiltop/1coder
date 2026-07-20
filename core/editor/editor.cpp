#include "editor/editor.h"

#include "buffers/buf_compile.h"
#include "buffers/buf_explorer.h"
#include "buffers/buf_image.h"
#include "editor/command.h"
#include "editor/lsp.h"
#include "editor/lsp_ui.h"
#include "os/os_file.h"
#include "text/syntax.h"

// Provided by core/buffers/buf_command.cpp.
BufferHandle CommandLineBufferOpen(Editor *ed);

#include <stdarg.h>
// Needed in its own right: base_types.h only pulls stdio in for Assert, which
// compiles out in release builds.
#include <stdio.h>

void EditorInit(Editor *ed, Arena *arena, RectS32 screen) {
  *ed = Editor{};
  ed->arena = arena;
  ed->screen = screen;

  BufferRegistryInit(&ed->buffers, arena);

  ed->global_map = KeymapAlloc(arena);
  ed->normal_map = KeymapAlloc(arena, ed->global_map);
  ed->insert_map = KeymapAlloc(arena, ed->global_map);
  ed->visual_map = KeymapAlloc(arena, ed->global_map);
  ed->operator_pending_map = KeymapAlloc(arena, ed->global_map);

  ed->cwd = OsGetCwd(arena);
  ed->status_arena = ArenaAlloc(MB(1));
  ed->command_line_arena = ArenaAlloc(KB(64));
  ed->font_size = kFontSizeDefault;
  ed->command_line_prompt = ':';
  ed->search_forward = true;
  ed->line_number_mode = kLineNumberModeDefault;

  // Start on an empty scratch buffer so there is always something focused.
  BufferHandle scratch = BufferOpen(&ed->buffers, BufferKind::Scratch, Str8Lit("[scratch]"));

  View *view = PushStruct(arena, View);
  ViewInit(view, scratch);

  ed->root_panel = PanelAllocLeaf(arena, view);
  ed->focused_panel = ed->root_panel;

  ed->command_buffer = CommandLineBufferOpen(ed);
  ed->command_view = PushStruct(arena, View);
  ViewInit(ed->command_view, ed->command_buffer);
  EditorLspInit(ed);
  ed->lsp_ui = EditorLspUiCreate();

  // What <CR> on a path opens. Images are the only kind that needs more than
  // "read it as text"; anything unregistered falls through to exactly that.
  ImageRegisterFiletypes(ed);

  EditorInstallDefaultBindings(ed);
  EditorLayout(ed);
}

void EditorDestroy(Editor *ed) {
  EditorLspDestroy(ed);
  if (ed->lsp_ui) EditorLspUiDestroy(ed->lsp_ui);
  ed->lsp_ui = nullptr;
  CompileBufferShutdown(ed);
  BufferRegistryDestroy(&ed->buffers);

  for (u64 i = 0; i < kRegisterCount; i += 1) {
    if (ed->register_arenas[i]) ArenaRelease(ed->register_arenas[i]);
    ed->register_arenas[i] = nullptr;
  }
  if (ed->status_arena) ArenaRelease(ed->status_arena);
  if (ed->command_line_arena) ArenaRelease(ed->command_line_arena);
  if (ed->search_arena) ArenaRelease(ed->search_arena);
  if (ed->compile_arena) ArenaRelease(ed->compile_arena);
  ed->status_arena = nullptr;
  ed->command_line_arena = nullptr;
  ed->compile_arena = nullptr;
}

bool EditorTick(Editor *ed) {
  bool lsp_changed = EditorLspTick(ed);
  bool compile_changed = CompileBufferTick(ed);
  return lsp_changed || compile_changed;
}

void EditorLayout(Editor *ed) {
  // The bottom row belongs to the command line and global status, so panels
  // get everything above it.
  RectS32 rect = ed->screen;
  rect.y1 = Max(rect.y1 - 1, rect.y0);
  PanelLayout(ed->root_panel, rect);
}

void EditorSetScreen(Editor *ed, RectS32 screen) {
  ed->screen = screen;
  EditorLayout(ed);
}

View *EditorFocusedView(Editor *ed) {
  return ed->focused_panel ? ed->focused_panel->view : nullptr;
}

Buffer *EditorBufferForView(Editor *ed, View *view) {
  return view ? BufferFromHandle(&ed->buffers, view->buffer) : nullptr;
}

Buffer *EditorFocusedBuffer(Editor *ed) {
  return EditorBufferForView(ed, EditorFocusedView(ed));
}

View *EditorInputView(Editor *ed) {
  if (ed->command_line_active && ed->command_view) return ed->command_view;
  return EditorFocusedView(ed);
}

Buffer *EditorInputBuffer(Editor *ed) { return EditorBufferForView(ed, EditorInputView(ed)); }

i32 EditorGutterWidth(Editor *ed, const Panel *panel) {
  if (!ed || !panel || ed->line_number_mode == LineNumberMode::Off) return 0;

  Buffer *buffer = EditorBufferForView(ed, panel->view);
  if (!buffer) return 0;

  // Numbering a picture says nothing, and the placeholder wants the full width.
  if (ImageBufferInfo(buffer)) return 0;

  // Relative numbers never exceed the distance to the furthest line, but sizing
  // on the line count keeps the gutter still as the cursor moves. A gutter that
  // breathed would reflow the text on every j.
  u64 largest = BufferLineCount(buffer);
  i32 digits = 1;
  for (u64 n = largest; n >= 10; n /= 10) digits += 1;

  return Max(digits, kLineNumberMinDigits) + 1;  // +1 blank column before the text
}

u64 EditorLineNumberLabel(const Editor *ed, const View *view, const Buffer *buffer, u64 line) {
  if (ed && ed->line_number_mode == LineNumberMode::Relative) {
    u64 cursor_line = ViewCursorLine(view, buffer);
    return (line > cursor_line) ? line - cursor_line : cursor_line - line;
  }
  return line + 1;  // buffer lines are 0-based; the display is not
}

RectS32 EditorPanelTextRect(Editor *ed, const Panel *panel) {
  // Each panel reserves its bottom row for its own status line, and its left
  // columns for the gutter.
  RectS32 rect = panel->rect;
  rect.y1 = Max(rect.y1 - 1, rect.y0);
  rect.x0 = Min(rect.x0 + EditorGutterWidth(ed, panel), rect.x1);
  return rect;
}

i32 EditorPanelTextWidth(Editor *ed, const Panel *panel) {
  return RectWidth(EditorPanelTextRect(ed, panel));
}

i32 EditorPanelTextHeight(Editor *ed, const Panel *panel) {
  return RectHeight(EditorPanelTextRect(ed, panel));
}

void EditorScrollFocusedToCursor(Editor *ed) {
  View *view = EditorFocusedView(ed);
  Buffer *buffer = EditorBufferForView(ed, view);
  if (!view || !buffer || !ed->focused_panel) return;

  ViewScrollToCursor(view, buffer, EditorPanelTextWidth(ed, ed->focused_panel),
                     EditorPanelTextHeight(ed, ed->focused_panel));
}

Panel *EditorSplit(Editor *ed, Axis2 axis) {
  View *current = EditorFocusedView(ed);
  if (!current || !ed->focused_panel) return nullptr;

  // The new window opens on the same buffer at the same place, as vim does.
  View *clone = PushStruct(ed->arena, View);
  *clone = *current;

  Panel *created = PanelSplit(ed->arena, ed->focused_panel, axis, clone);

  // The panel that was split is now an interior node holding no view, so focus
  // must move down to a leaf or every later split would find nothing to split.
  // Vim focuses the newly opened window, so do that.
  ed->focused_panel = created;

  EditorLayout(ed);
  return created;
}

void EditorClosePanel(Editor *ed, Panel *panel) {
  if (!panel) return;
  // Only leaves are closeable; an interior node is an artefact of the layout.
  if (!PanelIsLeaf(panel)) panel = PanelFirstLeaf(panel);
  if (!panel) return;

  // Closing the last window means there is nothing left to edit.
  if (PanelLeafCount(ed->root_panel) <= 1) {
    ed->quit = true;
    return;
  }

  Panel *focus = PanelClose(panel, &ed->root_panel);
  ed->focused_panel = focus ? focus : PanelFirstLeaf(ed->root_panel);
  EditorLayout(ed);
  EditorLspUiInvalidatePopupIfStale(ed->lsp_ui, EditorFocusedBuffer(ed));
}

void EditorFocusPanel(Editor *ed, Panel *panel) {
  if (!panel || !PanelIsLeaf(panel)) return;
  ed->focused_panel = panel;

  Buffer *buffer = EditorFocusedBuffer(ed);
  if (buffer && buffer->hooks.on_activate) {
    buffer->hooks.on_activate(ed, buffer, panel->view);
  }
  EditorLspUiInvalidatePopupIfStale(ed->lsp_ui, buffer);
}

void EditorFocusDir(Editor *ed, Dir2 dir) {
  Panel *target = PanelFocusDir(ed->root_panel, ed->focused_panel, dir);
  if (target) EditorFocusPanel(ed, target);
}

BufferHandle EditorOpenFile(Editor *ed, String8 path) {
  // A directory is not a file to read but a listing to browse, so `:e src/` and
  // a directory on the command line both land in the explorer.
  if (OsDirExists(path)) return ExplorerBufferOpen(ed, path);

  TempArena scratch = ScratchBegin1(ed->arena);
  String8 absolute = OsPathAbsolute(scratch.arena, path);

  // Opening the same file twice shares one buffer, so edits cannot diverge
  // between two windows onto it.
  BufferHandle existing = BufferFromPath(&ed->buffers, absolute);
  if (existing.index != 0) {
    Buffer *existing_buffer = BufferFromHandle(&ed->buffers, existing);
    if (existing_buffer) EditorLspOnFileBufferOpened(ed, existing_buffer);
    ScratchEnd(scratch);
    return existing;
  }

  BufferHandle handle = BufferOpen(&ed->buffers, BufferKind::File, Str8PathBase(absolute));
  Buffer *buffer = BufferFromHandle(&ed->buffers, handle);
  if (!buffer) {
    ScratchEnd(scratch);
    return BufferHandleZero();
  }

  if (!BufferLoadFile(ed, buffer, absolute)) {
    // A path that does not exist yet is a new file, not an error.
    buffer->path = PushStr8Copy(buffer->arena, absolute);
    buffer->name = PushStr8Copy(buffer->arena, Str8PathBase(absolute));
  }

  EditorLspOnFileBufferOpened(ed, buffer);
  SyntaxAttach(buffer, absolute);

  ScratchEnd(scratch);
  return handle;
}

void EditorShowBuffer(Editor *ed, BufferHandle buffer) {
  View *view = EditorFocusedView(ed);
  if (!view || buffer.index == 0) return;

  view->buffer = buffer;
  view->cursor = 0;
  view->preferred_column = 0;
  view->scroll_line = 0;
  view->scroll_column = 0;
  VimStateReset(&view->vim);

  Buffer *b = BufferFromHandle(&ed->buffers, buffer);
  if (b && b->hooks.on_activate) b->hooks.on_activate(ed, b, view);
  EditorLspUiInvalidatePopupIfStale(ed->lsp_ui, b);
}

namespace {

bool JumpBufferAlive(void *ctx, BufferHandle handle) {
  Editor *ed = (Editor *)ctx;
  return BufferFromHandle(&ed->buffers, handle) != nullptr;
}

}  // namespace

void EditorPushJump(Editor *ed, View *view) {
  if (!ed || !view || view == ed->command_view) return;
  JumpListPush(&view->jumps, JumpEntry{view->buffer, view->cursor});
}

void EditorJumpTo(Editor *ed, View *view, JumpEntry entry) {
  if (!ed || !view) return;

  Buffer *buffer = BufferFromHandle(&ed->buffers, entry.buffer);
  if (!buffer) return;

  if (!BufferHandleEqual(view->buffer, entry.buffer)) {
    view->buffer = entry.buffer;
    if (buffer->hooks.on_activate) buffer->hooks.on_activate(ed, buffer, view);
  }

  view->vim.mode = VimMode::Normal;
  VimClearPending(&view->vim);
  ViewSetCursor(view, buffer, entry.offset);
  EditorLspUiInvalidatePopupIfStale(ed->lsp_ui, buffer);
  EditorScrollFocusedToCursor(ed);
}

bool EditorJumpOlder(Editor *ed, View *view, u64 count) {
  if (!ed || !view) return false;

  JumpEntry current = {view->buffer, view->cursor};
  bool moved = false;
  for (u64 i = 0; i < Max(count, (u64)1); i += 1) {
    JumpEntry entry = {};
    if (!JumpListOlder(&view->jumps, current, &entry, JumpBufferAlive, ed)) break;
    EditorJumpTo(ed, view, entry);
    current = {view->buffer, view->cursor};
    moved = true;
  }
  return moved;
}

bool EditorJumpNewer(Editor *ed, View *view, u64 count) {
  if (!ed || !view) return false;

  bool moved = false;
  for (u64 i = 0; i < Max(count, (u64)1); i += 1) {
    JumpEntry entry = {};
    if (!JumpListNewer(&view->jumps, &entry, JumpBufferAlive, ed)) break;
    EditorJumpTo(ed, view, entry);
    moved = true;
  }
  return moved;
}

void EditorAwaitConfirm(Editor *ed, CommandId command, BufferHandle buffer) {
  ed->input.awaiting_confirm = true;
  ed->input.confirm_command = command;
  ed->input.confirm_buffer = buffer;
}

namespace {

// True when `path` is `prefix` itself or something inside it. Comparing the
// separator explicitly stops "/src" from matching "/srcfoo".
bool PathIsUnder(String8 path, String8 prefix) {
  if (Str8Match(path, prefix)) return true;
  if (path.size <= prefix.size) return false;
  if (!Str8StartsWith(path, prefix)) return false;
  return path.str[prefix.size] == '/';
}

}  // namespace

void EditorRetargetBufferPaths(Editor *ed, String8 old_path, String8 new_path) {
  for (BufferHandle h = BufferFirst(&ed->buffers); h.index != 0;
       h = BufferNext(&ed->buffers, h)) {
    Buffer *buffer = BufferFromHandle(&ed->buffers, h);
    if (!buffer || buffer->path.size == 0) continue;
    if (!PathIsUnder(buffer->path, old_path)) continue;

    TempArena scratch = ScratchBegin1(buffer->arena);
    String8 tail = Str8Skip(buffer->path, old_path.size);
    String8 moved = PushStr8Cat(scratch.arena, new_path, tail);

    buffer->path = PushStr8Copy(buffer->arena, moved);
    buffer->name = PushStr8Copy(buffer->arena, Str8PathBase(moved));
    ScratchEnd(scratch);
  }
}

void EditorOrphanBufferPaths(Editor *ed, String8 path) {
  for (BufferHandle h = BufferFirst(&ed->buffers); h.index != 0;
       h = BufferNext(&ed->buffers, h)) {
    Buffer *buffer = BufferFromHandle(&ed->buffers, h);
    if (!buffer || buffer->path.size == 0) continue;
    if (buffer->kind != BufferKind::File) continue;
    if (!PathIsUnder(buffer->path, path)) continue;

    // The file is gone but the text is not. Marking it dirty is what tells the
    // user, and `:w` puts it back.
    buffer->flags |= BufferFlags::Dirty;
  }
}

void EditorSetStatus(Editor *ed, String8 message) {
  ArenaClear(ed->status_arena);
  ed->status_message = PushStr8Copy(ed->status_arena, message);
}

void EditorSetStatusF(Editor *ed, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);

  // PushStr8F is variadic rather than va_list based, so format by hand here.
  // A status line has nowhere useful to put more than a screenful anyway.
  char buffer[1024];
  vsnprintf(buffer, sizeof(buffer), fmt, args);

  ArenaClear(ed->status_arena);
  ed->status_message = PushStr8Copy(ed->status_arena, Str8C(buffer));

  va_end(args);
}

namespace {

void StoreRegisterCache(Editor *ed, u8 name, String8 text, bool linewise) {
  if (!ed->register_arenas[name]) ed->register_arenas[name] = ArenaAlloc(MB(64));
  ArenaClear(ed->register_arenas[name]);
  ed->registers[name].text = PushStr8Copy(ed->register_arenas[name], text);
  ed->registers[name].linewise = linewise;
}

void StoreClipboardCaches(Editor *ed, String8 text, bool linewise) {
  StoreRegisterCache(ed, '+', text, linewise);
  StoreRegisterCache(ed, '*', text, linewise);
}

}  // namespace

void EditorSetRegister(Editor *ed, u8 name, String8 text, bool linewise) {
  if (name >= kRegisterCount) return;

  if (RegisterIsClipboard(name)) {
    if (!ed->clipboard.write) return;
    ed->clipboard.write(text);

    // Keep a copy of exactly what was written, along with its kind. The
    // clipboard itself carries no notion of linewise, so this is the only way a
    // linewise yank can come back linewise rather than being guessed at.
    StoreClipboardCaches(ed, text, linewise);
    return;
  }

  // Clearing first keeps a register's storage to the size of its current
  // contents rather than every value it has ever held.
  StoreRegisterCache(ed, name, text, linewise);
}

Register EditorGetRegister(Editor *ed, u8 name) {
  if (name >= kRegisterCount) return Register{};

  if (RegisterIsClipboard(name)) {
    if (!ed->clipboard.read) return Register{};

    TempArena scratch = ScratchBegin();
    String8 fresh = ed->clipboard.read(scratch.arena);

    // If the clipboard still holds exactly what we last put there, we know how
    // it was captured and do not have to guess.
    bool ours = Str8Match(fresh, ed->registers[name].text);

    String8 text = fresh;
    bool linewise = false;

    if (ours) {
      linewise = ed->registers[name].linewise;
    } else {
      // Text from another program carries no kind, so infer one. A newline
      // anywhere but the very end means it is genuinely several lines. A single
      // line with a trailing newline -- what selecting a line in a browser or a
      // terminal produces -- is charwise, and that stray newline is dropped so
      // pasting does not split the line it lands in.
      u64 first_newline = Str8FindFirstChar(fresh, '\n');
      bool has_interior_newline = (first_newline + 1 < fresh.size);

      linewise = has_interior_newline;
      if (!linewise && fresh.size > 0 && fresh.str[fresh.size - 1] == '\n') {
        text = Str8Chop(fresh, 1);
      }
    }

    // Copy into the register's own storage, which also refreshes the cache.
    StoreClipboardCaches(ed, text, linewise);

    ScratchEnd(scratch);
    return ed->registers[name];
  }

  return ed->registers[name];
}

void EditorSetFontSize(Editor *ed, f32 size) {
  f32 clamped = Clamp(kFontSizeMin, size, kFontSizeMax);
  if (clamped == ed->font_size) return;

  ed->font_size = clamped;
  // The app owns the glyph atlas and watches this flag, so the core never has
  // to know what a font is.
  ed->font_size_changed = true;
}
