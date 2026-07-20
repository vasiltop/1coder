#pragma once

#include "base/base_math.h"
#include "editor/lsp_ui.h"

struct Buffer;
struct RenderContext;
struct View;

void RenderLspDiagnostics(RenderContext *ctx, EditorLspUi *ui, Buffer *buffer, const View *view,
                          LspPositionEncoding position_encoding, RectF32 content_rect,
                          RectS32 text_cells, RangeU64 visible_lines, i32 gutter_columns);

void RenderLspPopup(RenderContext *ctx, const EditorLspUiPopupView *popup, Buffer *buffer,
                    const View *view, RectF32 content_rect, RectS32 text_cells,
                    RangeU64 visible_lines);
