#include "editor/panel.h"

namespace {

void PanelPushChild(Panel *parent, Panel *child) {
  child->parent = parent;
  child->prev = parent->last_child;
  child->next = nullptr;
  if (parent->last_child) {
    parent->last_child->next = child;
  } else {
    parent->first_child = child;
  }
  parent->last_child = child;
}

void PanelUnlink(Panel *panel) {
  Panel *parent = panel->parent;
  if (!parent) return;

  if (panel->prev) {
    panel->prev->next = panel->next;
  } else {
    parent->first_child = panel->next;
  }
  if (panel->next) {
    panel->next->prev = panel->prev;
  } else {
    parent->last_child = panel->prev;
  }

  panel->parent = panel->next = panel->prev = nullptr;
}

[[nodiscard]] u64 ChildCount(const Panel *panel) {
  u64 count = 0;
  for (const Panel *child = panel->first_child; child; child = child->next) count += 1;
  return count;
}

[[nodiscard]] i32 RectExtent(RectS32 rect, Axis2 axis) {
  return (axis == Axis2::X) ? RectWidth(rect) : RectHeight(rect);
}

[[nodiscard]] PanelBoundary InvalidBoundary(Axis2 axis) {
  return PanelBoundary{nullptr, nullptr, nullptr, axis, false};
}

[[nodiscard]] bool PanelHasAncestor(const Panel *panel, const Panel *ancestor) {
  for (const Panel *node = panel; node; node = node->parent) {
    if (node == ancestor) return true;
  }
  return false;
}

[[nodiscard]] i32 PanelDepth(const Panel *panel) {
  i32 depth = 0;
  for (const Panel *node = panel; node; node = node->parent) depth += 1;
  return depth;
}

[[nodiscard]] Panel *LowestCommonAncestor(Panel *a, Panel *b) {
  i32 depth_a = PanelDepth(a);
  i32 depth_b = PanelDepth(b);

  while (depth_a > depth_b) {
    a = a->parent;
    depth_a -= 1;
  }
  while (depth_b > depth_a) {
    b = b->parent;
    depth_b -= 1;
  }

  while (a && b && a != b) {
    a = a->parent;
    b = b->parent;
  }
  return (a == b) ? a : nullptr;
}

[[nodiscard]] Panel *ChildUnderAncestor(Panel *ancestor, Panel *leaf) {
  for (Panel *node = leaf; node; node = node->parent) {
    if (node->parent == ancestor) return node;
  }
  return nullptr;
}

[[nodiscard]] bool PanelsShareBoundary(const Panel *before, const Panel *after, Axis2 axis) {
  if (axis == Axis2::X) {
    return before->rect.x1 == after->rect.x0 &&
           Max(before->rect.y0, after->rect.y0) < Min(before->rect.y1, after->rect.y1);
  }
  return before->rect.y1 == after->rect.y0 &&
         Max(before->rect.x0, after->rect.x0) < Min(before->rect.x1, after->rect.x1);
}

[[nodiscard]] f64 PanelChildWeight(const Panel *panel) {
  return Max(panel->size_pct, 0.0);
}

[[nodiscard]] f64 PanelTotalWeight(const Panel *parent) {
  f64 total = 0.0;
  for (const Panel *child = parent->first_child; child; child = child->next) {
    total += PanelChildWeight(child);
  }
  return (total > 0.0) ? total : 1.0;
}

[[nodiscard]] f64 PanelWeightBeforeChild(const Panel *child) {
  f64 total = 0.0;
  for (const Panel *it = child->parent ? child->parent->first_child : nullptr;
       it && it != child; it = it->next) {
    total += PanelChildWeight(it);
  }
  return total;
}

// Centre of an edge, used as the probe point for directional focus.
void EdgeProbe(RectS32 rect, Dir2 dir, i32 *out_x, i32 *out_y) {
  i32 mid_x = rect.x0 + RectWidth(rect) / 2;
  i32 mid_y = rect.y0 + RectHeight(rect) / 2;

  switch (dir) {
    case Dir2::Left:  *out_x = rect.x0 - 1; *out_y = mid_y;      break;
    case Dir2::Right: *out_x = rect.x1;     *out_y = mid_y;      break;
    case Dir2::Up:    *out_x = mid_x;       *out_y = rect.y0 - 1; break;
    case Dir2::Down:  *out_x = mid_x;       *out_y = rect.y1;    break;
    default:          *out_x = mid_x;       *out_y = mid_y;      break;
  }
}

}  // namespace

Panel *PanelAllocLeaf(Arena *arena, View *view) {
  Panel *panel = PushStruct(arena, Panel);
  panel->view = view;
  panel->size_pct = 1.0;
  return panel;
}

Panel *PanelSplit(Arena *arena, Panel *panel, Axis2 axis, View *new_view) {
  Assert(PanelIsLeaf(panel) && "only leaves can be split");

  // The panel being split becomes the interior node, and its view moves down
  // into a new first child. Doing it this way keeps the caller's pointer valid
  // and keeps the root stable.
  Panel *existing = PushStruct(arena, Panel);
  existing->view = panel->view;
  existing->size_pct = 1.0;

  Panel *created = PushStruct(arena, Panel);
  created->view = new_view;
  created->size_pct = 1.0;

  panel->view = nullptr;
  panel->split_axis = axis;
  PanelPushChild(panel, existing);
  PanelPushChild(panel, created);

  return created;
}

Panel *PanelClose(Panel *panel, Panel **root) {
  Panel *parent = panel->parent;

  // Closing the last panel leaves nothing to focus.
  if (!parent) {
    *root = nullptr;
    return nullptr;
  }

  Panel *focus = panel->next ? panel->next : panel->prev;
  PanelUnlink(panel);

  if (ChildCount(parent) == 1) {
    // An interior node with one child carries no information, so fold the
    // child's contents up into it rather than leaving a redundant level.
    Panel *survivor = parent->first_child;

    parent->first_child = survivor->first_child;
    parent->last_child = survivor->last_child;
    parent->view = survivor->view;
    parent->split_axis = survivor->split_axis;

    for (Panel *child = parent->first_child; child; child = child->next) {
      child->parent = parent;
    }

    focus = PanelIsLeaf(parent) ? parent : PanelFirstLeaf(parent);
  } else if (focus && !PanelIsLeaf(focus)) {
    focus = PanelFirstLeaf(focus);
  }

  return focus;
}

void PanelLayout(Panel *root, RectS32 rect) {
  if (!root) return;

  root->rect = rect;
  if (PanelIsLeaf(root)) return;

  f64 total = PanelTotalWeight(root);

  bool horizontal = (root->split_axis == Axis2::X);
  i32 extent = RectExtent(rect, root->split_axis);
  i32 offset = horizontal ? rect.x0 : rect.y0;
  i32 end = horizontal ? rect.x1 : rect.y1;
  f64 cumulative = 0.0;
  constexpr f64 kLayoutEdgeBias = 1e-12;

  for (Panel *child = root->first_child; child; child = child->next) {
    cumulative += PanelChildWeight(child);
    // Cumulative edges keep later boundaries stable; the tiny bias cancels exact-boundary underflow.
    i32 next_offset =
        child->next ? ((horizontal ? rect.x0 : rect.y0) +
                       (i32)(((f64)extent * Min(cumulative / total, 1.0)) + kLayoutEdgeBias))
                    : end;
    next_offset = Clamp(offset, next_offset, end);
    i32 size = next_offset - offset;

    RectS32 child_rect = horizontal ? RectS32{offset, rect.y0, offset + size, rect.y1}
                                    : RectS32{rect.x0, offset, rect.x1, offset + size};
    PanelLayout(child, child_rect);
    offset += size;
  }
}

Panel *PanelFirstLeaf(Panel *root) {
  Panel *panel = root;
  while (panel && !PanelIsLeaf(panel)) panel = panel->first_child;
  return panel;
}

Panel *PanelLastLeaf(Panel *root) {
  Panel *panel = root;
  while (panel && !PanelIsLeaf(panel)) panel = panel->last_child;
  return panel;
}

Panel *PanelNextLeaf(Panel *root, Panel *panel) {
  if (!panel) return PanelFirstLeaf(root);

  // Climb until there is a sibling to descend into, then take its first leaf.
  for (Panel *node = panel; node && node != root;) {
    if (node->next) return PanelFirstLeaf(node->next);
    node = node->parent;
  }
  return PanelFirstLeaf(root);  // wrap
}

Panel *PanelPrevLeaf(Panel *root, Panel *panel) {
  if (!panel) return PanelLastLeaf(root);

  for (Panel *node = panel; node && node != root;) {
    if (node->prev) return PanelLastLeaf(node->prev);
    node = node->parent;
  }
  return PanelLastLeaf(root);  // wrap
}

u64 PanelLeafCount(Panel *root) {
  if (!root) return 0;
  if (PanelIsLeaf(root)) return 1;

  u64 count = 0;
  for (Panel *child = root->first_child; child; child = child->next) {
    count += PanelLeafCount(child);
  }
  return count;
}

Panel *PanelFromPoint(Panel *root, i32 x, i32 y) {
  if (!root || !RectContains(root->rect, x, y)) return nullptr;
  if (PanelIsLeaf(root)) return root;

  for (Panel *child = root->first_child; child; child = child->next) {
    Panel *hit = PanelFromPoint(child, x, y);
    if (hit) return hit;
  }
  return nullptr;
}

Panel *PanelFocusDir(Panel *root, Panel *panel, Dir2 dir) {
  if (!root || !panel) return nullptr;

  // Adjacency is geometric rather than structural: probe just outside the
  // panel's edge and take whichever leaf covers that cell. This does the right
  // thing across nested splits, where the tree shape does not match what the
  // user sees.
  i32 x = 0, y = 0;
  EdgeProbe(panel->rect, dir, &x, &y);

  Panel *found = PanelFromPoint(root, x, y);
  return (found == panel) ? nullptr : found;
}

PanelBoundary PanelBoundaryBetween(Panel *root, Panel *before_leaf, Panel *after_leaf,
                                   Axis2 axis) {
  if (!root || !before_leaf || !after_leaf || before_leaf == after_leaf) {
    return InvalidBoundary(axis);
  }
  if (!PanelIsLeaf(before_leaf) || !PanelIsLeaf(after_leaf)) return InvalidBoundary(axis);
  if (!PanelHasAncestor(before_leaf, root) || !PanelHasAncestor(after_leaf, root)) {
    return InvalidBoundary(axis);
  }

  Panel *parent = LowestCommonAncestor(before_leaf, after_leaf);
  if (!parent || !PanelHasAncestor(parent, root) || parent->split_axis != axis) {
    return InvalidBoundary(axis);
  }

  Panel *before = ChildUnderAncestor(parent, before_leaf);
  Panel *after = ChildUnderAncestor(parent, after_leaf);
  if (!before || !after || before == after || before->next != after) {
    return InvalidBoundary(axis);
  }
  if (!PanelsShareBoundary(before_leaf, after_leaf, axis)) {
    return InvalidBoundary(axis);
  }

  return PanelBoundary{parent, before, after, axis, true};
}

PanelBoundary PanelBoundaryAt(Panel *root, i32 x, i32 y, Axis2 axis) {
  if (!root) return InvalidBoundary(axis);

  Panel *before = nullptr;
  Panel *after = nullptr;
  if (axis == Axis2::X) {
    before = PanelFromPoint(root, x - 1, y);
    after = PanelFromPoint(root, x, y);
  } else {
    before = PanelFromPoint(root, x, y);
    after = PanelFromPoint(root, x, y + 1);
  }
  return PanelBoundaryBetween(root, before, after, axis);
}

void PanelResize(Panel *panel, f32 delta_pct) {
  if (!panel || !panel->parent) return;

  Panel *sibling = panel->next ? panel->next : panel->prev;
  if (!sibling) return;

  // Take from the neighbour so the parent's children still sum to the same
  // total and no space appears or vanishes.
  f64 available = panel->size_pct + sibling->size_pct;
  f64 target = Clamp(0.05, panel->size_pct + (f64)delta_pct, available - 0.05);

  panel->size_pct = target;
  sibling->size_pct = available - target;
}

void PanelResizeBoundary(PanelBoundary boundary, i32 delta_cells) {
  if (!boundary.valid || !boundary.parent || !boundary.before || !boundary.after) return;
  if (boundary.parent->split_axis != boundary.axis) return;
  if (boundary.before->parent != boundary.parent || boundary.after->parent != boundary.parent) {
    return;
  }
  if (boundary.before->next != boundary.after) return;
  if (delta_cells == 0) return;

  i32 parent_extent = RectExtent(boundary.parent->rect, boundary.axis);
  if (parent_extent <= 0) return;

  constexpr i32 kMinChildExtent = 2;
  i32 before_extent = RectExtent(boundary.before->rect, boundary.axis);
  i32 after_extent = RectExtent(boundary.after->rect, boundary.axis);
  i32 combined_extent = before_extent + after_extent;
  if (combined_extent < 2 * kMinChildExtent) return;

  i32 target_before =
      Clamp(kMinChildExtent, before_extent + delta_cells, combined_extent - kMinChildExtent);
  if (target_before == before_extent) return;

  i32 parent_start =
      (boundary.axis == Axis2::X) ? boundary.parent->rect.x0 : boundary.parent->rect.y0;
  i32 pair_start = ((boundary.axis == Axis2::X) ? boundary.before->rect.x0 : boundary.before->rect.y0) -
                   parent_start;
  i32 target_edge = pair_start + target_before;

  f64 pair_total = boundary.before->size_pct + boundary.after->size_pct;
  if (pair_total <= 0.0) return;

  f64 sibling_total = PanelTotalWeight(boundary.parent);
  f64 prefix_weight = PanelWeightBeforeChild(boundary.before);
  f64 low = ((f64)target_edge * sibling_total) / (f64)parent_extent - prefix_weight;
  f64 high = (((f64)target_edge + 1.0) * sibling_total) / (f64)parent_extent - prefix_weight;
  low = Clamp(0.0, low, pair_total);
  high = Clamp(0.0, high, pair_total);

  f64 before_weight = (high > low) ? (low + (high - low) * 0.5) : low;
  boundary.before->size_pct = before_weight;
  boundary.after->size_pct = pair_total - before_weight;
}

void PanelEqualize(Panel *root) {
  if (!root || PanelIsLeaf(root)) return;

  for (Panel *child = root->first_child; child; child = child->next) {
    child->size_pct = 1.0;
    PanelEqualize(child);
  }
}
