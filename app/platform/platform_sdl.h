#pragma once

#include <SDL3/SDL.h>

#include "input/keys.h"
#include "input/mouse.h"

// The SDL boundary for input.
//
// This is the only place SDL's key enums appear anywhere in the program.
// Everything below it speaks the editor's own Key and KeyChord types, which is
// what lets the whole editing core be driven from tests with no window.

[[nodiscard]] Key KeyFromSDLKeycode(SDL_Keycode keycode);
[[nodiscard]] KeyMod KeyModFromSDLMod(SDL_Keymod mods);

// Translates a key-down event. Returns an invalid chord for presses that should
// be ignored -- bare modifier keys, and unmodified printable keys, which arrive
// separately as text events so that keyboard layout and dead keys are the
// platform's problem rather than ours.
[[nodiscard]] KeyChord KeyChordFromSDLKeyEvent(const SDL_KeyboardEvent *event);

struct SDLMouseTranslateContext {
  SDL_Window *window;
  SDL_Renderer *renderer;
  f32 cell_width;
  f32 cell_height;
  KeyMod modifiers;
  MouseButton captured_button;
};

[[nodiscard]] MouseEvent MouseEventFromSDLEvent(const SDL_Event *event,
                                                const SDLMouseTranslateContext &context);
