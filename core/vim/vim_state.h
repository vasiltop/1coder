#pragma once

#include "base/base_math.h"
#include "base/base_string.h"
#include "base/base_types.h"

// Per-view modal state. Data only -- the logic lives in vim_motions.cpp and
// vim_operators.cpp -- so a View can embed it without dragging in the whole vim
// layer.

enum class VimMode : u8 {
  Normal = 0,
  Insert,
  Visual,
  VisualLine,
  VisualBlock,
  Replace,
  OperatorPending,
  COUNT
};

enum class OperatorKind : u8 {
  None = 0,
  Delete,
  Change,
  Yank,
  Indent,
  Dedent,
  COUNT
};

// How a motion's span is turned into a range for an operator to act on.
enum class MotionKind : u8 {
  Exclusive = 0,  // the character at the far end is not included (w, 0)
  Inclusive,      // it is (e, f)
  Linewise,       // whole lines regardless of column (j, k, G)
};

struct MotionResult {
  u64 target;
  MotionKind kind;
  bool valid;  // false when the motion could not move, which aborts an operator
};

// A yanked or deleted span. Linewise registers paste onto their own lines,
// which is why the distinction has to survive the yank.
struct Register {
  String8 text;
  bool linewise;
};

inline constexpr u64 kRegisterCount = 128;  // indexed by ASCII register name

struct VimState {
  VimMode mode;

  // Count typed before a command ("3" in "3dw"). A separate flag distinguishes
  // an explicit "1" from no count at all.
  u64 count;
  bool has_count;

  // Operator waiting for a motion, and the count typed against the operator
  // itself ("2" in "2d3w" -- vim multiplies the two).
  OperatorKind pending_operator;
  u64 operator_count;
  bool has_operator_count;

  // Anchor for visual mode; the selection runs between this and the cursor.
  u64 visual_anchor;

  // Last f/F/t/T, so `;` and `,` can repeat it.
  u32 last_find_char;
  bool last_find_forward;
  bool last_find_till;

  // Mode to return to when insert mode ends, and where the session began, so
  // the whole insert is one undo step.
  u64 insert_start;
};

[[nodiscard]] inline bool VimModeIsVisual(VimMode mode) {
  return mode == VimMode::Visual || mode == VimMode::VisualLine || mode == VimMode::VisualBlock;
}

[[nodiscard]] inline bool VimModeIsInsert(VimMode mode) {
  return mode == VimMode::Insert || mode == VimMode::Replace;
}

// Effective count for a command: the operator count and motion count multiply,
// and an absent count means one.
[[nodiscard]] inline u64 VimEffectiveCount(const VimState *vim) {
  u64 motion = vim->has_count ? Max(vim->count, (u64)1) : 1;
  u64 op = vim->has_operator_count ? Max(vim->operator_count, (u64)1) : 1;
  return motion * op;
}

void VimStateReset(VimState *vim);
// Clears the pending count and operator after a command runs.
void VimClearPending(VimState *vim);

[[nodiscard]] String8 VimModeName(VimMode mode);
