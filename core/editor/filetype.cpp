#include "editor/filetype.h"

#include "buffers/buf_explorer.h"
#include "editor/editor.h"
#include "os/os_file.h"

void FiletypeRegister(Editor *ed, String8 ext, BufferHandle (*open)(Editor *ed, String8 path)) {
  for (u64 i = 0; i < ed->filetypes.count; i += 1) {
    if (!Str8Match(ed->filetypes.handlers[i].ext, ext, StringMatch::CaseInsensitive)) continue;
    ed->filetypes.handlers[i].open = open;
    return;
  }

  if (ed->filetypes.count >= kMaxFiletypeHandlers) return;

  FiletypeHandler *handler = &ed->filetypes.handlers[ed->filetypes.count++];
  handler->ext = PushStr8Copy(ed->arena, ext);
  handler->open = open;
}

BufferHandle FiletypeOpen(Editor *ed, String8 path) {
  if (OsDirExists(path)) return ExplorerBufferOpen(ed, path);

  String8 ext = Str8PathExt(path);
  if (ext.size != 0) {
    for (u64 i = 0; i < ed->filetypes.count; i += 1) {
      FiletypeHandler *handler = &ed->filetypes.handlers[i];
      if (!Str8Match(handler->ext, ext, StringMatch::CaseInsensitive)) continue;
      if (handler->open) return handler->open(ed, path);
    }
  }

  // EditorOpenFile is the fallback rather than the entry point: routing it back
  // through here would recurse.
  return EditorOpenFile(ed, path);
}
