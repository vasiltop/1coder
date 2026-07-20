#pragma once

#include "base/base_string.h"
#include "editor/buffer.h"

struct Editor;

// Shell command output as a read-only buffer, in the shape of Emacs' compile
// buffer / nvim's compile-mode. :compile runs any shell command; :recompile
// and <leader>rc rerun the last one. Output streams in via EditorTick.

// Prefill for an empty :compile prompt: the last command, or "make -k".
[[nodiscard]] String8 CompilePrefillCommand(const Editor *ed);

// Remembers `command` as the last compile command.
void CompileRememberCommand(Editor *ed, String8 command);

// Starts (or restarts) `command` in the [compile] buffer and returns its
// handle. The focused window is not switched; the caller shows the buffer.
[[nodiscard]] BufferHandle CompileBufferRun(Editor *ed, String8 command);

// Appends any queued output. Returns true when the buffer changed.
[[nodiscard]] bool CompileBufferTick(Editor *ed);

// Stops any in-flight compile process. Safe to call when none is running.
void CompileBufferShutdown(Editor *ed);
