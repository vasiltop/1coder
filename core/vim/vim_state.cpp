#include "vim/vim_state.h"

void VimStateReset(VimState *vim) {
  *vim = VimState{};
  vim->mode = VimMode::Normal;
}

void VimClearPending(VimState *vim) {
  vim->count = 0;
  vim->has_count = false;
  vim->pending_operator = OperatorKind::None;
  vim->operator_count = 0;
  vim->has_operator_count = false;
}

String8 VimModeName(VimMode mode) {
  switch (mode) {
    case VimMode::Normal: return Str8Lit("NORMAL");
    case VimMode::Insert: return Str8Lit("INSERT");
    case VimMode::Visual: return Str8Lit("VISUAL");
    case VimMode::VisualLine: return Str8Lit("V-LINE");
    case VimMode::VisualBlock: return Str8Lit("V-BLOCK");
    case VimMode::Replace: return Str8Lit("REPLACE");
    case VimMode::OperatorPending: return Str8Lit("O-PENDING");
    default: return Str8Lit("?");
  }
}
