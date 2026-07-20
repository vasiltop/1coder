# Neovim-style mouse support design

## Problem

`app/main.cpp` currently handles mouse input directly: clicks only move focus between panels, and wheel motion scrolls the already-focused panel. That keeps mouse behavior trapped in SDL-facing code, which means it cannot be tested with the rest of the editor core and cannot grow into Neovim-style interaction without piling more policy into `app/`.

## Goals

- Move all mouse policy into the core so it is testable without SDL or a window.
- Keep mouse behavior Neovim-style for the exact supported gestures defined in this spec.
- Support cursor placement, live charwise drag, double-click word or matching-bracket selection, triple-click line selection, and right-click nearest-endpoint extension.
- Support middle-click system-clipboard put in Normal, Visual, Insert, Replace, and command-line modes.
- Route wheel scrolling to the hovered pane, not the focused pane, with vertical, horizontal, and `Shift`-page behavior.
- Preserve fractional wheel deltas so high-DPI trackpads scroll smoothly.
- Support command-line cursor placement, panel status-line focus, and split resizing from vertical edges and status-line boundaries.
- Cancel transient mouse state cleanly on release, focus loss, shutdown, or leaving an active capture.
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
- pixel and cell coordinates in a normalized form,
- hit-region identity,
- and a transient `MouseState` used while a button is down or wheel motion is being accumulated.

`MouseState` is intentionally short-lived. It records only the current capture, the pointer anchor, the last hovered pane, the pre-selection mode, accumulated fractional wheel deltas, and any resize target. Nothing in it is serialized or persisted.

### Core behavior

Add `core/editor/mouse.cpp` and route every mouse event through a single `EditorProcessMouse(Editor *, const MouseEvent &)` entry point. This function owns:

- hit testing against explicit regions,
- dispatch to mode-specific selection and paste logic,
- hover-based wheel routing,
- split-boundary resize logic,
- cancellation and cleanup,
- and restoration of the prior mode after transient selections.

### SDL translation

`app/main.cpp` and the SDL platform layer only translate SDL events into `MouseEvent` values. They also perform high-DPI pixel-to-cell conversion using the current render metrics, but they do not decide what a click means.

Mouse capture in SDL is short-lived: it is acquired only while a drag or resize is active and released as soon as the core says the gesture is complete or canceled.

SDL translation explicitly handles mouse button, wheel, motion, window enter/leave, and focus-loss events:

- motion updates an active drag or resize;
- wheel routing uses each wheel event's own `mouse_x` / `mouse_y`, so persistent hover state is not required for scrolling;
- window leave clears hover only when no capture is active;
- focus loss always cancels any active capture.

### Hit regions

Use explicit hit regions instead of implicit whole-window behavior:

- buffer content,
- panel gutter,
- panel status line,
- global bottom command-line/status row,
- split borders,
- and non-interactive background.

The core resolves the region first, then decides the behavior. Nested split boundary discovery walks the split tree so the correct vertical-edge or status-line boundary is chosen even inside multi-level layouts.
At any shared boundary, vertical split-edge hit testing takes precedence over the gutter.
At a cell where a vertical split edge intersects a panel status line, the vertical split edge wins; a plain click focuses the pane on the owning side only if no drag occurs, and no horizontal status-line resize is armed at that corner.

## Behavior

### Cursor placement and selection

- Left-click in a text surface moves the cursor to the clicked cell, clamping to the end of short or empty lines.
- A drag from a text surface starts charwise selection and updates live on every pointer move.
- Double-click selects the word under the pointer; if the click lands on a bracket delimiter, it expands to the matching bracketed range instead of treating the delimiter as an isolated word.
- Triple-click selects the full logical line as a linewise range.
- Right-click extends the nearest endpoint of the current selection to the clicked position. If no selection exists, it behaves like a simple cursor placement.

Selections always resolve through UTF-8-safe byte offsets; pointer placement never splits a codepoint or lands inside an invalid byte sequence.

### Panel gutter

- Clicking the panel gutter focuses that pane and places the cursor at column zero of the corresponding visible buffer line.
- Dragging from the panel gutter starts a charwise selection at that line start and extends it live as the pointer moves.

### Mode semantics

- **Normal mode:** left-click places the cursor, drag creates visual selection, right-click extends selection, and middle-click pastes clipboard text.
- **Visual mode:** mouse motion changes the active selection live; right-click extends the nearest edge; middle-click replaces the selection with clipboard text.
- **Insert mode:** clicking moves the insertion cursor, middle-click inserts clipboard text at the cursor, and any mouse-started selection temporarily enters Visual mode.
- **Replace mode:** same as Insert for mouse semantics, but the restored mode after a mouse selection is Replace rather than Insert.
- **Command-line mode:** only the global bottom command-line/status row is active; clicking places the command-line cursor there, drag selection is supported there, and middle-click inserts clipboard text into the command line.

When a selection starts in Insert or Replace, the editor snapshots the prior mode, enters Visual for the drag, and restores the exact prior insert mode when Visual ends or is canceled.

### Clipboard put

Middle-click uses the existing clipboard abstraction. If clipboard text is available, it is inserted or replaces the current selection according to the active mode. If the clipboard is unavailable, the gesture is a no-op.

### Wheel routing

Wheel motion targets the pane under the pointer, even if another pane is focused.

- Vertical wheel scrolls vertically.
- Horizontal wheel scrolls horizontally.
- `Shift` plus vertical wheel pages instead of line-scrolls.
- `Shift` plus horizontal wheel pages horizontally if the platform reports that axis.

Fractional deltas are accumulated per axis in `MouseState`. That keeps touchpads and other high-resolution devices smooth without losing partial motion.
Wheel events over the global command-line/status row are always no-ops; they never fall back to the focused pane.

### Command line and status line

- The global bottom command-line/status row is command-line only; it never resizes panes.
- Clicking the command line places the command-line cursor at the clicked cell on that global row.
- A panel status line focuses the pane attached to that line.
- A panel status line only arms the resize between the panes above and below it when there is a geometrically adjacent panel below; otherwise it behaves as focus only.
- Dragging on a vertical edge resizes the left/right split.
- Dragging on a status-line boundary resizes the top/bottom split only when the hit region is a panel status line with a panel below.

Split resizing clamps to each pane's minimum size. If the requested delta would violate the minimum, the resize stops at the clamp point and the capture stays active until release.

## Edge handling

- **Outside events:** pointer motion or clicks outside any supported region do nothing except preserve hover data for wheel routing.
- **Captured drags:** once a drag or resize begins, movement remains captured until release, cancellation, or loss of focus.
- **Release:** ending the active button finalizes selection or resize and clears transient state.
- **Window enter/leave:** enter may update hover; leave clears hover only when no capture is active.
- **Focus loss or shutdown:** cancel any active capture, restore the prior mode if a transient Visual state was in progress, and leave no half-finished resize behind.
- **Short or empty lines:** cursor placement and selection clamp to legal line bounds, including the empty-line case.
- **UTF-8:** hit testing uses line-index translation and byte-boundary snapping so multi-byte text remains valid.
- **Read-only buffers:** selection and focus changes still work; editing gestures such as middle-click put become no-ops.
- **Missing clipboard:** middle-click put is a no-op and does not disturb the current selection.
- **Resize minimums:** resizing clamps rather than crossing the minimum cell size for a pane.

## Testing and documentation strategy

- Add core unit tests for `EditorProcessMouse` and the supporting mouse helpers in `tests/`, using synthetic events and editor state only.
- Cover click-to-cursor, live drag, double and triple click selection, nearest-endpoint extension, middle-click paste, hover-wheel routing, fractional delta accumulation, resize clamping, and cancel-on-focus-loss behavior.
- Keep SDL translation thin enough that it can be smoke-tested separately, but do not require SDL in core tests.
- Document any new mouse behavior in the existing user-facing docs only if it changes bindings or visible semantics; otherwise keep the spec as the source of truth for implementation.
