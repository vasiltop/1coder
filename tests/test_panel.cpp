#include "editor/panel.h"
#include "test.h"

namespace {

// Panels only need a distinct View pointer per leaf for these tests; the views
// themselves stay empty.
struct Fixture {
  Arena *arena;
  Panel *root;
  View *views;
  u64 view_count;
};

Fixture MakeFixture() {
  Fixture f = {};
  f.arena = ArenaAlloc(MB(4));
  f.views = PushArray(f.arena, View, 32);
  f.root = PanelAllocLeaf(f.arena, &f.views[0]);
  f.view_count = 1;
  return f;
}

View *NextView(Fixture *f) { return &f->views[f->view_count++]; }

void Destroy(Fixture *f) { ArenaRelease(f->arena); }

constexpr RectS32 kScreen = {0, 0, 80, 24};

}  // namespace

TEST(panel_starts_as_single_leaf) {
  Fixture f = MakeFixture();

  CHECK(PanelIsLeaf(f.root));
  CHECK_EQ(PanelLeafCount(f.root), 1);

  PanelLayout(f.root, kScreen);
  CHECK_EQ(RectWidth(f.root->rect), 80);
  CHECK_EQ(RectHeight(f.root->rect), 24);

  Destroy(&f);
}

TEST(panel_vertical_split_divides_width) {
  Fixture f = MakeFixture();

  Panel *right = PanelSplit(f.arena, f.root, Axis2::X, NextView(&f));
  PanelLayout(f.root, kScreen);

  CHECK(!PanelIsLeaf(f.root));  // the root became an interior node
  CHECK_EQ(PanelLeafCount(f.root), 2);

  Panel *left = f.root->first_child;
  CHECK_EQ(left->rect.x0, 0);
  CHECK_EQ(left->rect.x1, 40);
  CHECK_EQ(right->rect.x0, 40);
  CHECK_EQ(right->rect.x1, 80);

  // Height is untouched by a vertical split.
  CHECK_EQ(RectHeight(left->rect), 24);
  CHECK_EQ(RectHeight(right->rect), 24);

  Destroy(&f);
}

TEST(panel_horizontal_split_divides_height) {
  Fixture f = MakeFixture();

  Panel *bottom = PanelSplit(f.arena, f.root, Axis2::Y, NextView(&f));
  PanelLayout(f.root, kScreen);

  Panel *top = f.root->first_child;
  CHECK_EQ(top->rect.y0, 0);
  CHECK_EQ(top->rect.y1, 12);
  CHECK_EQ(bottom->rect.y0, 12);
  CHECK_EQ(bottom->rect.y1, 24);
  CHECK_EQ(RectWidth(top->rect), 80);

  Destroy(&f);
}

TEST(panel_split_keeps_original_view_in_first_child) {
  Fixture f = MakeFixture();
  View *original = f.views;
  View *added = NextView(&f);

  Panel *created = PanelSplit(f.arena, f.root, Axis2::X, added);

  CHECK(f.root->first_child->view == original);
  CHECK(created->view == added);
  CHECK(f.root->view == nullptr);  // interior nodes hold no view

  Destroy(&f);
}

TEST(panel_nested_splits_tile_exactly) {
  Fixture f = MakeFixture();

  // Split vertically, then split the right half horizontally.
  Panel *right = PanelSplit(f.arena, f.root, Axis2::X, NextView(&f));
  Panel *right_bottom = PanelSplit(f.arena, right, Axis2::Y, NextView(&f));
  PanelLayout(f.root, kScreen);

  CHECK_EQ(PanelLeafCount(f.root), 3);

  Panel *left = f.root->first_child;
  Panel *right_top = right->first_child;

  CHECK_EQ(RectWidth(left->rect), 40);
  CHECK_EQ(RectHeight(left->rect), 24);

  CHECK_EQ(right_top->rect.x0, 40);
  CHECK_EQ(right_top->rect.y0, 0);
  CHECK_EQ(right_top->rect.y1, 12);

  CHECK_EQ(right_bottom->rect.x0, 40);
  CHECK_EQ(right_bottom->rect.y0, 12);
  CHECK_EQ(right_bottom->rect.y1, 24);

  // The children exactly cover the parent, with no gap or overlap.
  CHECK_EQ(right_top->rect.y1, right_bottom->rect.y0);
  CHECK_EQ(left->rect.x1, right_top->rect.x0);

  Destroy(&f);
}

TEST(panel_layout_leaves_no_gap_on_odd_sizes) {
  Fixture f = MakeFixture();

  Panel *right = PanelSplit(f.arena, f.root, Axis2::X, NextView(&f));
  // An odd width cannot divide evenly; the last child must absorb the
  // remainder rather than leaving a one-cell hole.
  PanelLayout(f.root, RectS32{0, 0, 81, 25});

  Panel *left = f.root->first_child;
  CHECK_EQ(left->rect.x0, 0);
  CHECK_EQ(right->rect.x1, 81);
  CHECK_EQ(left->rect.x1, right->rect.x0);
  CHECK_EQ(RectWidth(left->rect) + RectWidth(right->rect), 81);

  Destroy(&f);
}

TEST(panel_three_way_split) {
  Fixture f = MakeFixture();

  Panel *b = PanelSplit(f.arena, f.root, Axis2::X, NextView(&f));
  Panel *c = PanelSplit(f.arena, b, Axis2::X, NextView(&f));
  PanelLayout(f.root, kScreen);

  CHECK_EQ(PanelLeafCount(f.root), 3);

  // Nested rather than flat, so the halves subdivide: 40 / 20 / 20.
  Panel *a = f.root->first_child;
  CHECK_EQ(RectWidth(a->rect), 40);
  CHECK_EQ(RectWidth(b->first_child->rect), 20);
  CHECK_EQ(RectWidth(c->rect), 20);

  Destroy(&f);
}

TEST(panel_focus_direction) {
  Fixture f = MakeFixture();

  Panel *right = PanelSplit(f.arena, f.root, Axis2::X, NextView(&f));
  PanelLayout(f.root, kScreen);
  Panel *left = f.root->first_child;

  CHECK(PanelFocusDir(f.root, left, Dir2::Right) == right);
  CHECK(PanelFocusDir(f.root, right, Dir2::Left) == left);

  // Nothing beyond the outer edges.
  CHECK(PanelFocusDir(f.root, left, Dir2::Left) == nullptr);
  CHECK(PanelFocusDir(f.root, right, Dir2::Right) == nullptr);
  CHECK(PanelFocusDir(f.root, left, Dir2::Up) == nullptr);
  CHECK(PanelFocusDir(f.root, left, Dir2::Down) == nullptr);

  Destroy(&f);
}

TEST(panel_focus_direction_across_nested_splits) {
  Fixture f = MakeFixture();

  // Left half, and a right half split into top and bottom.
  Panel *right = PanelSplit(f.arena, f.root, Axis2::X, NextView(&f));
  Panel *right_bottom = PanelSplit(f.arena, right, Axis2::Y, NextView(&f));
  PanelLayout(f.root, kScreen);

  Panel *left = f.root->first_child;
  Panel *right_top = right->first_child;

  // Adjacency is geometric, so moving right from the tall left panel lands in
  // whichever right-hand panel covers its vertical middle.
  CHECK(PanelFocusDir(f.root, left, Dir2::Right) == right_bottom);

  CHECK(PanelFocusDir(f.root, right_top, Dir2::Down) == right_bottom);
  CHECK(PanelFocusDir(f.root, right_bottom, Dir2::Up) == right_top);
  CHECK(PanelFocusDir(f.root, right_top, Dir2::Left) == left);
  CHECK(PanelFocusDir(f.root, right_bottom, Dir2::Left) == left);

  Destroy(&f);
}

TEST(panel_from_point) {
  Fixture f = MakeFixture();

  Panel *right = PanelSplit(f.arena, f.root, Axis2::X, NextView(&f));
  PanelLayout(f.root, kScreen);
  Panel *left = f.root->first_child;

  CHECK(PanelFromPoint(f.root, 0, 0) == left);
  CHECK(PanelFromPoint(f.root, 39, 23) == left);
  CHECK(PanelFromPoint(f.root, 40, 0) == right);
  CHECK(PanelFromPoint(f.root, 79, 23) == right);
  // Outside the screen entirely.
  CHECK(PanelFromPoint(f.root, 80, 0) == nullptr);
  CHECK(PanelFromPoint(f.root, 0, 24) == nullptr);

  Destroy(&f);
}

TEST(panel_close_collapses_redundant_parent) {
  Fixture f = MakeFixture();

  Panel *right = PanelSplit(f.arena, f.root, Axis2::X, NextView(&f));
  CHECK_EQ(PanelLeafCount(f.root), 2);

  Panel *root = f.root;
  Panel *focus = PanelClose(right, &root);

  // With one child left the interior node carries no information, so the tree
  // folds back to a single leaf rather than keeping a pointless level.
  CHECK_EQ(PanelLeafCount(root), 1);
  CHECK(PanelIsLeaf(root));
  CHECK(focus == root);
  CHECK(root->view == f.views);  // the surviving view came up with it

  PanelLayout(root, kScreen);
  CHECK_EQ(RectWidth(root->rect), 80);

  Destroy(&f);
}

TEST(panel_close_in_nested_tree) {
  Fixture f = MakeFixture();

  Panel *right = PanelSplit(f.arena, f.root, Axis2::X, NextView(&f));
  Panel *right_bottom = PanelSplit(f.arena, right, Axis2::Y, NextView(&f));
  CHECK_EQ(PanelLeafCount(f.root), 3);

  Panel *root = f.root;
  Panel *focus = PanelClose(right_bottom, &root);

  CHECK_EQ(PanelLeafCount(root), 2);
  CHECK(focus != nullptr);

  // The right side collapses back to a full-height panel.
  PanelLayout(root, kScreen);
  CHECK_EQ(RectHeight(root->last_child->rect), 24);

  Destroy(&f);
}

TEST(panel_close_last_panel) {
  Fixture f = MakeFixture();

  Panel *root = f.root;
  Panel *focus = PanelClose(root, &root);

  // Closing the only window leaves nothing to focus, which the caller reads as
  // "quit".
  CHECK(root == nullptr);
  CHECK(focus == nullptr);

  Destroy(&f);
}

TEST(panel_leaf_iteration_wraps) {
  Fixture f = MakeFixture();

  Panel *right = PanelSplit(f.arena, f.root, Axis2::X, NextView(&f));
  Panel *right_bottom = PanelSplit(f.arena, right, Axis2::Y, NextView(&f));

  Panel *left = f.root->first_child;
  Panel *right_top = right->first_child;

  CHECK(PanelFirstLeaf(f.root) == left);
  CHECK(PanelLastLeaf(f.root) == right_bottom);

  CHECK(PanelNextLeaf(f.root, left) == right_top);
  CHECK(PanelNextLeaf(f.root, right_top) == right_bottom);
  CHECK(PanelNextLeaf(f.root, right_bottom) == left);  // wraps

  CHECK(PanelPrevLeaf(f.root, right_bottom) == right_top);
  CHECK(PanelPrevLeaf(f.root, left) == right_bottom);  // wraps

  Destroy(&f);
}

TEST(panel_resize_takes_from_sibling) {
  Fixture f = MakeFixture();

  Panel *right = PanelSplit(f.arena, f.root, Axis2::X, NextView(&f));
  Panel *left = f.root->first_child;

  PanelResize(left, 0.5f);
  PanelLayout(f.root, kScreen);

  // The pair still exactly covers the parent.
  CHECK_EQ(RectWidth(left->rect) + RectWidth(right->rect), 80);
  CHECK(RectWidth(left->rect) > 40);
  CHECK_EQ(left->rect.x1, right->rect.x0);

  // A panel cannot be squeezed out of existence.
  PanelResize(left, -100.0f);
  PanelLayout(f.root, kScreen);
  CHECK(RectWidth(left->rect) > 0);
  CHECK(RectWidth(right->rect) > 0);

  Destroy(&f);
}

TEST(panel_equalize) {
  Fixture f = MakeFixture();

  Panel *right = PanelSplit(f.arena, f.root, Axis2::X, NextView(&f));
  Panel *left = f.root->first_child;

  PanelResize(left, 0.6f);
  PanelEqualize(f.root);
  PanelLayout(f.root, kScreen);

  CHECK_EQ(RectWidth(left->rect), 40);
  CHECK_EQ(RectWidth(right->rect), 40);

  Destroy(&f);
}

TEST(panel_relayout_on_resize) {
  Fixture f = MakeFixture();

  Panel *right = PanelSplit(f.arena, f.root, Axis2::X, NextView(&f));
  Panel *left = f.root->first_child;

  PanelLayout(f.root, kScreen);
  CHECK_EQ(RectWidth(left->rect), 40);

  // Layout is a pure function of the tree and the window, so a resize is just
  // another call.
  PanelLayout(f.root, RectS32{0, 0, 200, 50});
  CHECK_EQ(RectWidth(left->rect), 100);
  CHECK_EQ(RectWidth(right->rect), 100);
  CHECK_EQ(RectHeight(left->rect), 50);

  Destroy(&f);
}
