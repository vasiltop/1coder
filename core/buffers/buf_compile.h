#pragma once

#include "base/base_string.h"
#include "editor/buffer.h"

struct Editor;
struct View;

// Shell command output as a read-only buffer, in the shape of Emacs' compile
// buffer / nvim's compile-mode. :compile runs any shell command; :recompile
// and <leader>rc rerun the last one. Output streams in via EditorTick.
//
// <leader>ne / <leader>pe walk GNU-style diagnostics (file:line:col:) in a
// circle and jump to each locus. <CR> in the compile buffer opens the error
// under the cursor.

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

// Circular next/prev error navigation. Opens the matching file at line:col in
// a non-compile window and pans the compile window onto that diagnostic.
// Returns false when there is no compile buffer or no parseable errors.
[[nodiscard]] bool CompileNextError(Editor *ed, View *origin);
[[nodiscard]] bool CompilePrevError(Editor *ed, View *origin);

// The compile-buffer line currently marked as the active error, for painting.
// False when no next/prev/Enter jump has set one yet.
[[nodiscard]] bool CompileHighlightLine(const Buffer *buffer, u64 *out_line);
