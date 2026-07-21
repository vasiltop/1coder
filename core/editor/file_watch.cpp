#include "editor/file_watch.h"

#include <chrono>

#include "command/command_id.h"
#include "editor/buffer.h"
#include "editor/buffer_registry.h"
#include "editor/editor.h"
#include "editor/panel.h"
#include "editor/view.h"
#include "os/os_file.h"

namespace {

// How often the throttled tick actually stats the open files. Fast enough that
// an external edit shows up as "live", slow enough that a handful of buffers on
// a network filesystem cost nothing noticeable.
constexpr u64 kWatchIntervalNs = 750ull * 1000ull * 1000ull;

u64 NowNanos() {
  return (u64)std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

bool IsWatchableFile(const Buffer *buffer) {
  return buffer && buffer->kind == BufferKind::File && buffer->path.size > 0;
}

// After the text under a view is replaced, its saved cursor and scroll offsets
// can point past the new end. Pull them back in for every view showing this
// buffer so a reload never lands the cursor out of bounds.
void ReclampViews(Editor *ed, const Buffer *buffer) {
  // PanelNextLeaf wraps back to the first leaf rather than stopping, so walk one
  // full cycle and break when we return to where we started.
  Panel *first = PanelFirstLeaf(ed->root_panel);
  for (Panel *panel = first; panel;) {
    View *view = panel->view;
    if (view && BufferHandleEqual(view->buffer, buffer->handle)) {
      ViewSetCursor(view, buffer, view->cursor);
      u64 last_line = BufferLineCount(buffer);
      last_line = last_line > 0 ? last_line - 1 : 0;
      if (view->scroll_line > last_line) view->scroll_line = last_line;
    }
    panel = PanelNextLeaf(ed->root_panel, panel);
    if (panel == first) break;
  }
}

// Ask, once, whether to throw away unsaved edits in favour of what is now on
// disk. Reuses the editor's single yes/no confirmation, which the input layer
// runs against the focused buffer -- so this is only worth asking when `buffer`
// is that buffer and nothing else is already being confirmed.
void PromptReload(Editor *ed, Buffer *buffer) {
  EditorSetStatusF(ed, "%.*s changed on disk -- reload and discard changes? [y/N]",
                   (int)buffer->name.size, buffer->name.str);
  EditorAwaitConfirm(ed, CommandId::revert, buffer->handle);
}

}  // namespace

bool EditorReloadFileBuffer(Editor *ed, Buffer *buffer) {
  if (!buffer || buffer->path.size == 0) return false;
  if (!BufferLoadFile(ed, buffer, buffer->path)) return false;
  ReclampViews(ed, buffer);
  return true;
}

void EditorFileWatchScan(Editor *ed) {
  Buffer *focused = EditorFocusedBuffer(ed);
  BufferHandle focused_handle = focused ? focused->handle : BufferHandleZero();

  for (BufferHandle h = BufferFirst(&ed->buffers); h.index != 0;
       h = BufferNext(&ed->buffers, h)) {
    Buffer *buffer = BufferFromHandle(&ed->buffers, h);
    if (!IsWatchableFile(buffer)) continue;

    u64 disk = OsFileModTime(buffer->path);
    if (disk == buffer->disk_mtime) continue;  // in sync, nothing to do

    // Re-baseline so this same change is reconciled exactly once, whatever we
    // decide to do about it below.
    buffer->disk_mtime = disk;

    bool is_focused = BufferHandleEqual(h, focused_handle);

    if (disk == 0) {
      // The file was removed out from under us. The text is still ours, so mark
      // it dirty -- `:w` puts the file back -- and let the user know.
      buffer->flags |= BufferFlags::Dirty | BufferFlags::DiskConflict;
      EditorSetStatusF(ed, "%.*s deleted on disk", (int)buffer->name.size, buffer->name.str);
      continue;
    }

    if (!BufferIsDirty(buffer)) {
      // No local edits to lose: adopt the new contents silently. The reload
      // re-baselines disk_mtime against what it reads and clears DiskConflict.
      if (EditorReloadFileBuffer(ed, buffer)) {
        EditorSetStatusF(ed, "%.*s reloaded from disk", (int)buffer->name.size,
                         buffer->name.str);
      }
      continue;
    }

    // Unsaved edits collide with an external change: never clobber silently.
    buffer->flags |= BufferFlags::DiskConflict;
    if (is_focused && !ed->input.awaiting_confirm && !ed->command_line_active) {
      PromptReload(ed, buffer);
    } else {
      // Not the buffer in front, or a prompt is already up: leave a passive
      // marker. `:revert` reloads it, or the prompt appears on next change.
      EditorSetStatusF(ed, "%.*s changed on disk (unsaved changes) -- :revert to reload",
                       (int)buffer->name.size, buffer->name.str);
    }
  }
}

void EditorFileWatchTick(Editor *ed) {
  u64 now = NowNanos();
  if (ed->file_watch_next_ns != 0 && now < ed->file_watch_next_ns) return;
  ed->file_watch_next_ns = now + kWatchIntervalNs;
  EditorFileWatchScan(ed);
}
