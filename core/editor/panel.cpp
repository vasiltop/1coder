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
  panel->size_pct = 1.0f;
  return panel;
}

Panel *PanelSplit(Arena *arena, Panel *panel, Axis2 axis, View *new_view) {
  Assert(PanelIsLeaf(panel) && "only leaves can be split");

  // The panel being split becomes the interior node, and its view moves down
  // into a new first child. Doing it this way keeps the caller's pointer valid
  // and keeps the root stable.
  Panel *existing = PushStruct(arena, Panel);
  existing->view = panel->view;
  existing->size_pct = 1.0f;

  Panel *created = PushStruct(arena, Panel);
  created->view = new_view;
  created->size_pct = 1.0f;

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

  f32 total = 0.0f;
  for (Panel *child = root->first_child; child; child = child->next) {
    total += Max(child->size_pct, 0.0f);
  }
  if (total <= 0.0f) total = 1.0f;

  bool horizontal = (root->split_axis == Axis2::X);
  i32 extent = RectExtent(rect, root->split_axis);
  i32 offset = horizontal ? rect.x0 : rect.y0;

  for (Panel *child = root->first_child; child; child = child->next) {
    // The last child absorbs the rounding remainder, so the children always
    // exactly tile the parent.
    i32 size = child->next ? (i32)((f32)extent * (Max(child->size_pct, 0.0f) / total))
                           : ((horizontal ? rect.x1 : rect.y1) - offset);

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

void PanelResize(Panel *panel, f32 delta_pct) {
  if (!panel || !panel->parent) return;

  Panel *sibling = panel->next ? panel->next : panel->prev;
  if (!sibling) return;

  // Take from the neighbour so the parent's children still sum to the same
  // total and no space appears or vanishes.
  f32 available = panel->size_pct + sibling->size_pct;
  f32 target = Clamp(0.05f, panel->size_pct + delta_pct, available - 0.05f);

  panel->size_pct = target;
  sibling->size_pct = available - target;
}

void PanelEqualize(Panel *root) {
  if (!root || PanelIsLeaf(root)) return;

  for (Panel *child = root->first_child; child; child = child->next) {
    child->size_pct = 1.0f;
    PanelEqualize(child);
  }
}
