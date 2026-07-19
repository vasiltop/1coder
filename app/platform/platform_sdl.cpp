#include "platform/platform_sdl.h"

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
