# Neovim-style mouse support design

## Problem

`app/main.cpp` currently handles mouse input directly: clicks only move focus between panels, and wheel motion scrolls the already-focused panel. That keeps mouse behavior trapped in SDL-facing code, which means it cannot be tested with the rest of the editor core and cannot grow into Neovim-style interaction without piling more policy into `app/`.

## Goals

- Move all mouse policy into the core so it is testable without SDL or a window.
- Keep mouse behavior Neovim-style for the exact supported gestures defined in this spec.
- Support cursor placement, live charwise drag, double-click word or matching-bracket selection, triple-click line selection, and right-click nearest-endpoint extension.
- Support middle-click system-clipboard put in Normal, Visual, Insert, Replace, and command-line modes.
- Route wheel scrolling from each wheel event's own coordinates, not stored hover or focused-pane state, with vertical, horizontal, and `Shift`-page behavior.
- Preserve fractional wheel deltas so high-DPI trackpads scroll smoothly.
- Support command-line cursor placement, panel status-line focus, and split resizing from vertical edges and status-line boundaries.
- Cancel transient mouse state according to the lifecycle defined below: capture end finalizes a gesture, while focus loss and shutdown can cancel it.
- Support high-DPI pixel-to-cell translation in SDL without letting SDL own any editing rules.

## Non-goals

- Configurable `mouse` or `mousemodel` settings.
- Mouse key mappings or user-defined gesture remapping.
- Tag/search modifier gestures.
- Tabs, popups, folds, scrollbars, X1/X2 buttons, or any other unsupported Neovim mouse surfaces.
- Explorer-specific double-click behavior.
- Quadruple-click selection.
- `Alt`-right block selection, because VisualBlock is still incomplete.

The behavior sections below are authoritative for supported gestures. Anything not described there is unsupported, even if Neovim can do something adjacent.

## Architecture

### Core model

Add `core/input/mouse.h` as the platform-neutral event and state definition. It owns:

- mouse button and wheel enums,
- click count,
- modifier snapshot,
- continuous grid coordinates in cell units (`grid_x`, `grid_y`) plus derived integer cell coordinates,
- hit-region identity,
- and a transient `MouseState` used while a button is down or wheel motion is being accumulated.

`MouseState` is intentionally short-lived. It records only the current capture, the pointer anchor, the last hovered pane for visual hover or drag-adjacent state, the pre-selection mode, accumulated fractional wheel deltas, and any resize target. Nothing in it is serialized or persisted.
For wheel handling, it also stores the current routed wheel target pane, independent X/Y remainders, and the current per-axis scroll semantics (line or page).

Hit testing and resize handles use the continuous grid coordinates; derived integer cell coordinates are used for buffer text placement and other cell-snapped gestures.

### Core behavior

Add `core/editor/mouse.cpp` and route every mouse event through a single `EditorProcessMouse(Editor *, const MouseEvent &)` entry point. This function owns:

- hit testing against explicit regions,
- dispatch to mode-specific selection and paste logic,
- split-boundary resize logic,
- cancellation and cleanup,
- and restoration of the prior mode after transient selections.

### SDL translation

`app/main.cpp` and the SDL platform layer only translate SDL events into `MouseEvent` values. They also perform high-DPI pixel-to-cell conversion using the current render metrics, but they do not decide what a click means.
SDL computes `grid_x` / `grid_y` from window pixels using the current glyph width and line height; core tests synthesize those values directly.

Mouse capture in SDL is short-lived: it is acquired only while a drag or resize is active and released as soon as the core says the gesture is complete or canceled.

SDL translation explicitly handles mouse button, wheel, motion, window enter/leave, and focus-loss events:

- motion updates an active drag or resize;
- wheel routing uses each wheel event's own `mouse_x` / `mouse_y` and never falls back to stored hover;
- window leave may clear visual hover or drag-adjacent state only when no capture is active;
- focus loss always cancels any active capture.
There is no separate pixel slop or platform-dependent fallback.

### Hit regions

Use explicit hit regions instead of implicit whole-window behavior:

- buffer content,
- panel gutter,
- panel status line,
- global bottom command-line/status row,
- split borders,
- and non-interactive background.

The core resolves the region first, then decides the behavior. Nested split boundary discovery walks the split tree so the correct vertical-edge or status-line boundary is chosen even inside multi-level layouts.
At any shared boundary, the vertical split-edge handle takes precedence over buffer, gutter, and status-line regions.
The vertical split-edge handle is the strip within 0.125 cell of an internal X boundary (`abs(grid_x - boundary_x) <= 0.125`) across that boundary's span.
A no-drag press on that handle belongs to the pane whose rect starts at `boundary_x` (the right/after pane).
At a cell where a vertical split edge intersects a panel status line, the vertical split edge wins; a plain click focuses the pane on the owning side only if no drag occurs, and no horizontal status-line resize is armed at that corner.

## Behavior

### Cursor placement and selection

- Every pane-local mouse button press in buffer text or gutter first focuses that pane, then performs its gesture.
- Left-click in buffer text moves the cursor to the clicked cell, clamping to the end of short or empty lines.
- A drag from buffer text starts charwise selection and updates live on every pointer move.
- Double-click selects the word under the pointer; if the click lands on a bracket delimiter, it expands to the matching bracketed range instead of treating the delimiter as an isolated word.
- Triple-click selects the full logical line as a linewise range.
- Right-click extends the nearest endpoint of the current selection to the clicked position. If no selection exists, it behaves like a simple cursor placement.

Selections always resolve through UTF-8-safe byte offsets; pointer placement never splits a codepoint or lands inside an invalid byte sequence.

### Drag threshold

A pending drag becomes an active selection on the first motion event whose resolved buffer offset differs from the press anchor offset, or whose resolved command-line offset differs for command-line selection.
Continuous motion within the same resolved character or cell does not start a drag.
Gutter presses resolve their anchor to line-start before comparison.
Once active, later motion updates the selection even when clamped to the same endpoint.
No pixel-distance or platform-dependent slop is used.

### Panel gutter

- Clicking the panel gutter focuses that pane and places the cursor at column zero of the corresponding visible buffer line.
- Dragging from the panel gutter starts a charwise selection at that line start and extends it live as the pointer moves.
  Both gutter gestures focus the pane before acting.

### Mode semantics

- **Normal mode:** left-click places the cursor, drag creates visual selection, right-click extends selection, and middle-click pastes clipboard text.
- **Visual mode:** mouse motion changes the active selection live; right-click extends the nearest edge; middle-click replaces the selection with clipboard text.
- **Insert mode:** clicking moves the insertion cursor, middle-click inserts clipboard text at the cursor, and mouse gestures that create a non-empty selection follow the temporary Visual rules below.
- **Replace mode:** same as Insert for mouse semantics, but the remembered mode restored by the temporary Visual rules below is Replace rather than Insert.
- **Command-line mode:** only the global bottom command-line/status row is active; clicking places the command-line cursor there, drag selection is supported there, and middle-click inserts clipboard text into the command line.

In Insert and Replace, any mouse gesture that creates a non-empty selection enters temporary Visual immediately or on drag threshold crossing. Double-click word or matching-bracket selection and triple-click line selection enter Visual immediately; drag selection enters Visual when the threshold defined above is crossed. Those selections remain visible after release just like a completed drag.
A single click or press-release with no selection stays in the original Insert or Replace mode.
The remembered Insert or Replace mode is restored only when that Visual selection is later ended or canceled, including a plain click that collapses it, a replacement or paste that consumes it, keyboard Visual exit, focus-loss cancellation, or shutdown.

### Clipboard put

Middle-click uses the existing clipboard abstraction. If clipboard text is available, it is inserted or replaces the current selection according to the active mode. If the clipboard is unavailable, the gesture is a no-op.

### Wheel routing

Wheel motion routes solely from the wheel event's own `mouse_x` / `mouse_y` coordinates. Wheel never changes focus.

- Vertical wheel scrolls vertically.
- Horizontal wheel scrolls horizontally.
- `Shift` plus vertical wheel pages instead of line-scrolls.
- `Shift` plus horizontal wheel pages horizontally if the platform reports that axis.

Before accumulating a wheel event, the core hit-tests its coordinates to determine the routed pane and axis semantics.
If the routed pane differs from the stored wheel target pane, the event is a no-op region, or `Shift` changes an affected axis between line and page semantics, the affected remainders are cleared before processing.
A no-op event only clears and does not accumulate.
Fractional deltas accumulate in the routed pane's own X/Y remainders in `MouseState`, keyed by the current scroll semantics for each axis, so deltas from one pane or unit cannot complete in another.
Wheel events over the global command-line/status row or other no-op regions are always clears-only and never fall back to the focused pane.

### Command line and status line

- The global bottom command-line/status row is command-line only; it never resizes panes.
- Clicking the command line places the command-line cursor at the clicked cell on that global row.
- A panel status line focuses the pane attached to that line.
- A panel status-line horizontal resize uses the entire status-line cell row only where a geometrically adjacent pane begins on the next row; otherwise it is focus only.
- Existing vertical-edge precedence resolves any intersection with a split edge.
- Dragging on a vertical edge resizes the left/right split.
- Dragging on a status-line boundary resizes the top/bottom split only when the hit region is a panel status line with a panel below.

Split resizing clamps each adjacent direct child to at least 2 full panel cells along the resized axis before leaf chrome is subtracted. Horizontal-width splits keep at least 2 columns per child; vertical-height splits keep at least 2 rows per child, meaning one panel status row plus at least one content row for leaves. If the parent extent cannot satisfy both minima, resizing is a no-op while capture remains active until release. Nested descendants retain their existing proportions; only the two direct children participating in the resize are constrained.

## Edge handling

- **Outside events:** pointer motion or clicks outside any supported region do nothing except update visual hover if the app tracks it.
- **Motion without press:** pointer motion without an active press never changes focus.
- **Captured drags:** once a drag or resize begins, movement remains captured until release, cancellation, or loss of focus.
- **Release:** ending the active button finalizes a drag selection or resize but does not exit temporary Visual; the pending Insert or Replace mode stays restored only when that selection later ends or is canceled.
- **Window enter/leave:** enter may update visual hover or drag-adjacent state; leave may clear that state only when no capture is active.
- **Focus loss or shutdown:** cancel any active capture, restore the prior mode if a transient Visual state was in progress, and leave no half-finished resize behind.
- **Short or empty lines:** cursor placement and selection clamp to legal line bounds, including the empty-line case.
- **UTF-8:** hit testing uses line-index translation and byte-boundary snapping so multi-byte text remains valid.
- **Read-only buffers:** selection and focus changes still work; editing gestures such as middle-click put become no-ops.
- **Missing clipboard:** middle-click put is a no-op and does not disturb the current selection.
- **Resize minimums:** the 2-cell direct-child constraint defined above is the only resize minimum.

## Testing and documentation strategy

- Add core unit tests for `EditorProcessMouse` and the supporting mouse helpers in `tests/`, using synthetic events and editor state only.
- Cover click-to-cursor, live drag, double and triple click selection, nearest-endpoint extension, middle-click paste, wheel routing, fractional delta accumulation, resize clamping, and cancel-on-focus-loss behavior.
- Keep SDL translation thin enough that it can be smoke-tested separately, but do not require SDL in core tests.
- Document any new mouse behavior in the existing user-facing docs only if it changes bindings or visible semantics; otherwise keep the spec as the source of truth for implementation.
