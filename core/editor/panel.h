#pragma once

#include "base/base_arena.h"
#include "base/base_math.h"
#include "editor/view.h"

// The window split tree.
//
// Leaves hold a view; interior nodes hold children laid out along one axis.
// Splitting turns a leaf into an interior node with two leaves, so arbitrarily
// nested vertical and horizontal splits fall out of one operation.
//
// Rects are in cells and recomputed from the root each frame, which keeps
// layout a pure function of the tree and the window size -- and testable
// without a window.

struct Panel {
  Panel *first_child;
  Panel *last_child;
  Panel *next;
  Panel *prev;
  Panel *parent;

  Axis2 split_axis;  // meaningful on interior nodes
  f32 size_pct;      // share of the parent's extent, relative to siblings

  View *view;        // leaves only
  RectS32 rect;      // computed by PanelLayout
};

[[nodiscard]] inline bool PanelIsLeaf(const Panel *panel) {
  return panel->first_child == nullptr;
}

[[nodiscard]] Panel *PanelAllocLeaf(Arena *arena, View *view);

// Splits a leaf, returning the newly created sibling panel. The original view
// stays in the first child; the new panel receives `new_view`, which the caller
// typically clones from the original so the split shows the same place in the
// same buffer.
//
// Splitting an interior panel is not meaningful; pass a leaf.
Panel *PanelSplit(Arena *arena, Panel *panel, Axis2 axis, View *new_view);

// Removes a leaf. When its parent is left with a single child, the parent
// collapses into that child so the tree never keeps a pointless interior node.
// Returns the panel that should receive focus, or null if the root was closed.
Panel *PanelClose(Panel *panel, Panel **root);

// Assigns rects to every panel in the tree. Children divide their parent's
// extent along its axis in proportion to size_pct, with the last child taking
// the remainder so rounding never leaves a gap.
void PanelLayout(Panel *root, RectS32 rect);

[[nodiscard]] Panel *PanelFirstLeaf(Panel *root);
[[nodiscard]] Panel *PanelLastLeaf(Panel *root);
// Leaf order for cycling focus with a wrap.
[[nodiscard]] Panel *PanelNextLeaf(Panel *root, Panel *panel);
[[nodiscard]] Panel *PanelPrevLeaf(Panel *root, Panel *panel);
[[nodiscard]] u64 PanelLeafCount(Panel *root);

// The leaf adjacent to `panel` in a direction, or null at the edge. Requires
// PanelLayout to have run, since adjacency is decided geometrically -- which is
// what makes it behave sensibly across nested splits.
[[nodiscard]] Panel *PanelFocusDir(Panel *root, Panel *panel, Dir2 dir);

// The leaf containing a cell, for click-to-focus.
[[nodiscard]] Panel *PanelFromPoint(Panel *root, i32 x, i32 y);

// Grows or shrinks a panel along its parent's axis, taking the difference from
// its sibling.
void PanelResize(Panel *panel, f32 delta_pct);
// Restores every panel under `root` to an even share.
void PanelEqualize(Panel *root);
