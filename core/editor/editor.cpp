#include "editor/editor.h"

#include "editor/command.h"
#include "os/os_file.h"

// Provided by core/buffers/buf_command.cpp.
BufferHandle CommandLineBufferOpen(Editor *ed);

#include <stdarg.h>

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

  // Start on an empty scratch buffer so there is always something focused.
  BufferHandle scratch = BufferOpen(&ed->buffers, BufferKind::Scratch, Str8Lit("[scratch]"));

  View *view = PushStruct(arena, View);
  ViewInit(view, scratch);

  ed->root_panel = PanelAllocLeaf(arena, view);
  ed->focused_panel = ed->root_panel;

  ed->command_buffer = CommandLineBufferOpen(ed);

  EditorInstallDefaultBindings(ed);
  EditorLayout(ed);
}

void EditorDestroy(Editor *ed) { BufferRegistryDestroy(&ed->buffers); }

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

RectS32 EditorPanelTextRect(const Editor *ed, const Panel *panel) {
  // Each panel reserves its bottom row for its own status line.
  RectS32 rect = panel->rect;
  rect.y1 = Max(rect.y1 - 1, rect.y0);
  return rect;
}

i32 EditorPanelTextWidth(const Editor *ed, const Panel *panel) {
  return RectWidth(EditorPanelTextRect(ed, panel));
}

i32 EditorPanelTextHeight(const Editor *ed, const Panel *panel) {
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
}

void EditorFocusPanel(Editor *ed, Panel *panel) {
  if (!panel || !PanelIsLeaf(panel)) return;
  ed->focused_panel = panel;

  Buffer *buffer = EditorFocusedBuffer(ed);
  if (buffer && buffer->hooks.on_activate) {
    buffer->hooks.on_activate(ed, buffer, panel->view);
  }
}

void EditorFocusDir(Editor *ed, Dir2 dir) {
  Panel *target = PanelFocusDir(ed->root_panel, ed->focused_panel, dir);
  if (target) EditorFocusPanel(ed, target);
}

BufferHandle EditorOpenFile(Editor *ed, String8 path) {
  TempArena scratch = ScratchBegin1(ed->arena);
  String8 absolute = OsPathAbsolute(scratch.arena, path);

  // Opening the same file twice shares one buffer, so edits cannot diverge
  // between two windows onto it.
  BufferHandle existing = BufferFromPath(&ed->buffers, absolute);
  if (existing.index != 0) {
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
}

void EditorSetStatus(Editor *ed, String8 message) {
  ed->status_message = PushStr8Copy(ed->arena, message);
}

void EditorSetStatusF(Editor *ed, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);

  TempArena scratch = ScratchBegin1(ed->arena);
  // PushStr8F is variadic rather than va_list based, so format into scratch by
  // hand here.
  char stack_buffer[1024];
  vsnprintf(stack_buffer, sizeof(stack_buffer), fmt, args);
  ed->status_message = PushStr8Copy(ed->arena, Str8C(stack_buffer));
  ScratchEnd(scratch);

  va_end(args);
}

void EditorSetRegister(Editor *ed, u8 name, String8 text, bool linewise) {
  if (name >= kRegisterCount) return;
  ed->registers[name].text = PushStr8Copy(ed->arena, text);
  ed->registers[name].linewise = linewise;
}

Register EditorGetRegister(Editor *ed, u8 name) {
  if (name >= kRegisterCount) return Register{};
  return ed->registers[name];
}
