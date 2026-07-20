#include "platform/platform_sdl.h"

#include <math.h>

namespace {

[[nodiscard]] MouseButton MouseButtonFromSDLButton(Uint8 button) {
  switch (button) {
    case SDL_BUTTON_LEFT:   return MouseButton::Left;
    case SDL_BUTTON_MIDDLE: return MouseButton::Middle;
    case SDL_BUTTON_RIGHT:  return MouseButton::Right;
    default:                return MouseButton::None;
  }
}

[[nodiscard]] MouseButton MouseButtonFromSDLState(SDL_MouseButtonFlags state,
                                                  MouseButton captured_button) {
  if (captured_button != MouseButton::None) return captured_button;
  if (state & SDL_BUTTON_LMASK) return MouseButton::Left;
  if (state & SDL_BUTTON_MMASK) return MouseButton::Middle;
  if (state & SDL_BUTTON_RMASK) return MouseButton::Right;
  return MouseButton::None;
}

[[nodiscard]] bool RenderCoordsFromWindowPoint(const SDLMouseTranslateContext &context, float window_x,
                                               float window_y, float *render_x, float *render_y) {
  if (context.renderer &&
      SDL_RenderCoordinatesFromWindow(context.renderer, window_x, window_y, render_x, render_y)) {
    return true;
  }

  if (!context.window) return false;

  int window_width = 0, window_height = 0;
  int pixel_width = 0, pixel_height = 0;
  if (!SDL_GetWindowSize(context.window, &window_width, &window_height) ||
      !SDL_GetWindowSizeInPixels(context.window, &pixel_width, &pixel_height)) {
    return false;
  }
  if (window_width <= 0 || window_height <= 0 || pixel_width <= 0 || pixel_height <= 0) {
    return false;
  }

  *render_x = window_x * ((float)pixel_width / (float)window_width);
  *render_y = window_y * ((float)pixel_height / (float)window_height);
  return true;
}

[[nodiscard]] bool PopulateMousePosition(MouseEvent *result, float window_x, float window_y,
                                         const SDLMouseTranslateContext &context) {
  if (!result || context.cell_width <= 0.0f || context.cell_height <= 0.0f) return false;

  float render_x = 0.0f;
  float render_y = 0.0f;
  if (!RenderCoordsFromWindowPoint(context, window_x, window_y, &render_x, &render_y)) return false;

  result->grid_x = render_x / context.cell_width;
  result->grid_y = render_y / context.cell_height;
  result->x = (i32)floorf(result->grid_x);
  result->y = (i32)floorf(result->grid_y);
  return true;
}

}  // namespace

Key KeyFromSDLKeycode(SDL_Keycode keycode) {
  if (keycode >= SDLK_A && keycode <= SDLK_Z) {
    return (Key)((u16)Key::A + (keycode - SDLK_A));
  }
  if (keycode >= SDLK_0 && keycode <= SDLK_9) {
    return (Key)((u16)Key::Num0 + (keycode - SDLK_0));
  }
  if (keycode >= SDLK_F1 && keycode <= SDLK_F12) {
    return (Key)((u16)Key::F1 + (keycode - SDLK_F1));
  }

  switch (keycode) {
    case SDLK_ESCAPE:       return Key::Escape;
    case SDLK_RETURN:       return Key::Return;
    case SDLK_KP_ENTER:     return Key::Return;
    case SDLK_TAB:          return Key::Tab;
    case SDLK_SPACE:        return Key::Space;
    case SDLK_BACKSPACE:    return Key::Backspace;
    case SDLK_DELETE:       return Key::Delete;
    case SDLK_INSERT:       return Key::Insert;
    case SDLK_LEFT:         return Key::Left;
    case SDLK_RIGHT:        return Key::Right;
    case SDLK_UP:           return Key::Up;
    case SDLK_DOWN:         return Key::Down;
    case SDLK_HOME:         return Key::Home;
    case SDLK_END:          return Key::End;
    case SDLK_PAGEUP:       return Key::PageUp;
    case SDLK_PAGEDOWN:     return Key::PageDown;
    case SDLK_MINUS:        return Key::Minus;
    case SDLK_EQUALS:       return Key::Equal;
    case SDLK_LEFTBRACKET:  return Key::LeftBracket;
    case SDLK_RIGHTBRACKET: return Key::RightBracket;
    case SDLK_BACKSLASH:    return Key::Backslash;
    case SDLK_SEMICOLON:    return Key::Semicolon;
    case SDLK_APOSTROPHE:   return Key::Apostrophe;
    case SDLK_GRAVE:        return Key::Grave;
    case SDLK_COMMA:        return Key::Comma;
    case SDLK_PERIOD:       return Key::Period;
    case SDLK_SLASH:        return Key::Slash;
    default:                return Key::Unknown;
  }
}

KeyMod KeyModFromSDLMod(SDL_Keymod mods) {
  KeyMod result = KeyMod::None;
  if (mods & SDL_KMOD_CTRL) result |= KeyMod::Ctrl;
  if (mods & SDL_KMOD_SHIFT) result |= KeyMod::Shift;
  if (mods & SDL_KMOD_ALT) result |= KeyMod::Alt;
  if (mods & SDL_KMOD_GUI) result |= KeyMod::Super;
  return result;
}

KeyChord KeyChordFromSDLKeyEvent(const SDL_KeyboardEvent *event) {
  Key key = KeyFromSDLKeycode(event->key);
  if (key == Key::Unknown) return KeyChord{};

  KeyMod mods = KeyModFromSDLMod(event->mod);

  // Keys that produce text are handled by SDL_EVENT_TEXT_INPUT instead, so the
  // platform resolves keyboard layout, shift and dead keys for us. Only the
  // keys that never produce text, or that are held under a modifier, become
  // key chords here.
  bool has_command_modifier = HasAny(mods, KeyMod::Ctrl | KeyMod::Alt | KeyMod::Super);

  bool produces_text = false;
  switch (key) {
    case Key::Escape:
    case Key::Return:
    case Key::Tab:
    case Key::Backspace:
    case Key::Delete:
    case Key::Insert:
    case Key::Left:
    case Key::Right:
    case Key::Up:
    case Key::Down:
    case Key::Home:
    case Key::End:
    case Key::PageUp:
    case Key::PageDown:
      produces_text = false;
      break;
    default:
      // Letters, digits, punctuation and space all arrive as text events.
      produces_text = true;
      break;
  }

  if (produces_text && !has_command_modifier) return KeyChord{};

  return KeyChordKey(key, mods);
}

MouseEvent MouseEventFromSDLEvent(const SDL_Event *event, const SDLMouseTranslateContext &context) {
  MouseEvent result = {};
  if (!event) return result;

  switch (event->type) {
    case SDL_EVENT_WINDOW_FOCUS_LOST:
      result.action = MouseAction::Cancel;
      return result;

    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
      result.button = MouseButtonFromSDLButton(event->button.button);
      if (result.button == MouseButton::None) return MouseEvent{};
      if (!PopulateMousePosition(&result, event->button.x, event->button.y, context)) return MouseEvent{};
      result.action = event->type == SDL_EVENT_MOUSE_BUTTON_DOWN ? MouseAction::Press
                                                                 : MouseAction::Release;
      result.modifiers = KeyModFromSDLMod(SDL_GetModState());
      result.click_count = event->button.clicks;
      return result;

    case SDL_EVENT_MOUSE_MOTION:
      result.button = MouseButtonFromSDLState(event->motion.state, context.captured_button);
      if (result.button == MouseButton::None) return MouseEvent{};
      if (!PopulateMousePosition(&result, event->motion.x, event->motion.y, context)) return MouseEvent{};
      result.action = MouseAction::Drag;
      result.modifiers = KeyModFromSDLMod(SDL_GetModState());
      return result;

    case SDL_EVENT_MOUSE_WHEEL: {
      if (!PopulateMousePosition(&result, event->wheel.mouse_x, event->wheel.mouse_y, context)) {
        return MouseEvent{};
      }
      result.action = MouseAction::Wheel;
      result.modifiers = KeyModFromSDLMod(SDL_GetModState());
      result.wheel_x = event->wheel.x;
      result.wheel_y = event->wheel.y;
      if (event->wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
        result.wheel_x = -result.wheel_x;
        result.wheel_y = -result.wheel_y;
      }
      return result;
    }

    default:
      return result;
  }
}
