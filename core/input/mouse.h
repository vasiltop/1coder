#pragma once

#include "editor/panel.h"
#include "input/keys.h"

struct Buffer;
struct Editor;

enum class MouseAction : u8 {
  None = 0,
  Press,
  Drag,
  Release,
  Wheel,
  Cancel,
};

enum class MouseButton : u8 {
  None = 0,
  Left,
  Middle,
  Right,
};

enum class MouseHitKind : u8 {
  Outside = 0,
  Text,
  Gutter,
  StatusLine,
  VerticalBoundary,
  CommandLine,
  Image,
};

enum class MouseCaptureKind : u8 {
  None = 0,
  Panel,
  CommandLine,
  Boundary,
};

enum class MouseSelectionKind : u8 {
  None = 0,
  Character,
  Word,
  Line,
};

enum class MouseWheelUnit : u8 {
  None = 0,
  Line,
  Page,
};

struct MouseEvent {
  MouseAction action;
  MouseButton button;
  KeyMod modifiers;
  u8 click_count;
  f32 grid_x;
  f32 grid_y;
  i32 x;
  i32 y;
  f32 wheel_x;
  f32 wheel_y;
};

struct MouseHit {
  MouseHitKind kind;
  Panel *panel;
  View *view;
  Buffer *buffer;
  PanelBoundary boundary;
  u64 offset;
  bool has_offset;
};

struct MouseCapture {
  MouseCaptureKind kind;
  Panel *panel;
  View *view;
  Buffer *buffer;
  PanelBoundary boundary;
  MouseButton button;
};

struct MouseState {
  MouseCapture capture;
  MouseHit pointer_anchor;
  MouseHit selection_anchor;
  MouseHit latest_hit;
  MouseSelectionKind selection_kind;
  f32 latest_grid_x;
  f32 latest_grid_y;
  i32 latest_x;
  i32 latest_y;
  Panel *wheel_panel;
  View *wheel_view;
  Buffer *wheel_buffer;
  f32 wheel_x_remainder;
  f32 wheel_y_remainder;
  MouseWheelUnit wheel_x_unit;
  MouseWheelUnit wheel_y_unit;
};
