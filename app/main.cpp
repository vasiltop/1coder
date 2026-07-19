#include <SDL3/SDL.h>

#include "base/base_arena.h"
#include "base/base_string.h"
#include "editor/command.h"
#include "editor/editor.h"
#include "platform/platform_sdl.h"
#include "render/draw.h"
#include "render/glyph_atlas.h"
#include "render/render_editor.h"

#include <stdio.h>

namespace {

constexpr i32 kInitialWidth = 1200;
constexpr i32 kInitialHeight = 800;

// Renders one frame to a file and exits. Useful for checking the renderer
// without a display, and for capturing what the editor actually looks like.
bool SaveScreenshot(SDL_Renderer *renderer, const char *path) {
  SDL_Surface *surface = SDL_RenderReadPixels(renderer, nullptr);
  if (!surface) {
    fprintf(stderr, "SDL_RenderReadPixels failed: %s\n", SDL_GetError());
    return false;
  }
  bool ok = SDL_SaveBMP(surface, path);
  if (!ok) fprintf(stderr, "SDL_SaveBMP failed: %s\n", SDL_GetError());
  SDL_DestroySurface(surface);
  return ok;
}

struct App {
  Arena *arena;
  // The atlas is rebuilt whenever the font size changes, so it gets an arena of
  // its own that can be cleared wholesale rather than growing on every zoom.
  Arena *font_arena;

  SDL_Window *window;
  SDL_Renderer *renderer;

  GlyphAtlas atlas;
  DrawList draw;
  RenderContext render;
  Editor editor;

  String8 font_path;
  String8 font_face;
};

// ---------------------------------------------------------------------------
// Clipboard
//
// Installed into the editor as function pointers, so nothing in core/ has to
// know that SDL exists.
// ---------------------------------------------------------------------------

String8 ClipboardRead(Arena *arena) {
  if (!SDL_HasClipboardText()) return String8{nullptr, 0};

  char *text = SDL_GetClipboardText();
  if (!text) return String8{nullptr, 0};

  String8 result = PushStr8Copy(arena, Str8C(text));
  SDL_free(text);
  return result;
}

void ClipboardWrite(String8 text) {
  TempArena scratch = ScratchBegin();
  SDL_SetClipboardText(PushCStr(scratch.arena, text));
  ScratchEnd(scratch);
}

// Feeds a text-input event to the editor, one codepoint at a time. SDL has
// already resolved layout and dead keys by this point.
void HandleTextInput(Editor *ed, const char *text) {
  String8 utf8 = Str8C(text);
  for (u64 i = 0; i < utf8.size;) {
    DecodedCodepoint decoded = Utf8Decode(utf8, i);
    if (decoded.codepoint != 0) {
      EditorProcessChord(ed, KeyChordChar(decoded.codepoint));
    }
    i += Max(decoded.advance, (u32)1);
  }
}

void SyncScreenSize(App *app) {
  i32 width = 0, height = 0;
  SDL_GetWindowSizeInPixels(app->window, &width, &height);
  EditorSetScreen(&app->editor, RenderScreenCells(&app->render, width, height));
}

// Rebuilds the glyph atlas at the editor's current font size. Because layout is
// in cells, resizing the grid afterwards is the same path a window resize
// takes.
bool RebuildFont(App *app) {
  GlyphAtlasDestroy(&app->atlas);
  ArenaClear(app->font_arena);

  if (!GlyphAtlasInit(&app->atlas, app->font_arena, app->renderer, app->font_path,
                      app->editor.font_size, app->font_face)) {
    return false;
  }

  RenderContextInit(&app->render, &app->draw, &app->atlas);
  SyncScreenSize(app);
  return true;
}

}  // namespace

int main(int argc, char **argv) {
  Arena *arena = ArenaAlloc(GB(1));

  // --screenshot <path>: draw one frame, save it, exit. Everything else on the
  // command line is a file to open.
  const char *screenshot_path = nullptr;
  const char *startup_keys = nullptr;
  int file_argc = 0;
  char *file_argv[64];
  for (int i = 1; i < argc; i += 1) {
    if (SDL_strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
      screenshot_path = argv[i + 1];
      i += 1;
      continue;
    }
    // --keys replays a binding spec at startup, which is how the renderer gets
    // exercised in a particular state without a human at the keyboard.
    if (SDL_strcmp(argv[i], "--keys") == 0 && i + 1 < argc) {
      startup_keys = argv[i + 1];
      i += 1;
      continue;
    }
    if (file_argc < (int)ArrayCount(file_argv)) file_argv[file_argc++] = argv[i];
  }

  // ---- font ----
  String8 font_face = String8{nullptr, 0};
  String8 font_path = GlyphAtlasFindMonospaceFont(arena, &font_face);
  f32 font_size = kFontSizeDefault;

  if (const char *env_font = SDL_getenv("EDITOR_FONT")) {
    font_path = Str8C(env_font);
    font_face = String8{nullptr, 0};
  }
  // Which face to take out of a .ttc collection.
  if (const char *env_face = SDL_getenv("EDITOR_FONT_FACE")) {
    font_face = Str8C(env_face);
  }
  if (const char *env_size = SDL_getenv("EDITOR_FONT_SIZE")) {
    f32 parsed = (f32)SDL_atof(env_size);
    if (parsed > 4.0f) font_size = parsed;
  }
  if (font_path.size == 0) {
    fprintf(stderr,
            "No monospace font found. Set EDITOR_FONT to a .ttf path.\n");
    return 1;
  }

  // ---- SDL ----
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    return 1;
  }

  App *app = PushStruct(arena, App);
  app->arena = arena;
  app->font_arena = ArenaAlloc(MB(64));
  app->font_path = PushStr8Copy(arena, font_path);
  app->font_face = PushStr8Copy(arena, font_face);

  app->window = SDL_CreateWindow("1code", kInitialWidth, kInitialHeight,
                                 SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (!app->window) {
    fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
    return 1;
  }

  app->renderer = SDL_CreateRenderer(app->window, nullptr);
  if (!app->renderer) {
    fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
    return 1;
  }
  SDL_SetRenderVSync(app->renderer, 1);

  if (!GlyphAtlasInit(&app->atlas, app->font_arena, app->renderer, app->font_path, font_size,
                      app->font_face)) {
    fprintf(stderr, "Could not load font: %.*s\n", (int)font_path.size, (char *)font_path.str);
    return 1;
  }

  DrawInit(&app->draw, arena, app->renderer, &app->atlas);
  RenderContextInit(&app->render, &app->draw, &app->atlas);

  // ---- editor ----
  i32 width = 0, height = 0;
  SDL_GetWindowSizeInPixels(app->window, &width, &height);
  EditorInit(&app->editor, arena, RenderScreenCells(&app->render, width, height));

  app->editor.font_size = font_size;
  app->editor.clipboard.read = ClipboardRead;
  app->editor.clipboard.write = ClipboardWrite;

  // Files named on the command line, each in its own split after the first.
  for (int i = 0; i < file_argc; i += 1) {
    BufferHandle handle = EditorOpenFile(&app->editor, Str8C(file_argv[i]));
    if (handle.index == 0) continue;
    if (i > 0) (void)EditorSplit(&app->editor, Axis2::X);
    EditorShowBuffer(&app->editor, handle);
  }

  if (startup_keys) EditorProcessSpec(&app->editor, Str8C(startup_keys));

  // A startup key sequence may have zoomed, so settle the font before drawing.
  if (app->editor.font_size_changed) {
    app->editor.font_size_changed = false;
    if (!RebuildFont(app)) return 1;
  }

  if (screenshot_path) {
    SDL_GetWindowSizeInPixels(app->window, &width, &height);
    SDL_SetRenderDrawColor(app->renderer, 0, 0, 0, 255);
    SDL_RenderClear(app->renderer);
    RenderEditor(&app->render, &app->editor, width, height);

    bool ok = SaveScreenshot(app->renderer, screenshot_path);
    SDL_RenderPresent(app->renderer);

    EditorDestroy(&app->editor);
    GlyphAtlasDestroy(&app->atlas);
    SDL_DestroyRenderer(app->renderer);
    SDL_DestroyWindow(app->window);
    SDL_Quit();
    return ok ? 0 : 1;
  }

  SDL_StartTextInput(app->window);

  // ---- main loop ----
  while (!app->editor.quit) {
    SDL_Event event;
    // Block until something happens: an editor is idle almost all the time, and
    // spinning a render loop at the refresh rate to redraw identical pixels
    // would burn a core for nothing.
    if (!SDL_WaitEvent(&event)) break;

    do {
      switch (event.type) {
        case SDL_EVENT_QUIT:
          app->editor.quit = true;
          break;

        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
          SyncScreenSize(app);
          break;

        case SDL_EVENT_KEY_DOWN: {
          KeyChord chord = KeyChordFromSDLKeyEvent(&event.key);
          if (KeyChordValid(chord)) EditorProcessChord(&app->editor, chord);
          break;
        }

        case SDL_EVENT_TEXT_INPUT:
          HandleTextInput(&app->editor, event.text.text);
          break;

        case SDL_EVENT_MOUSE_BUTTON_DOWN: {
          // Click to focus a window.
          i32 cell_x = (i32)(event.button.x / app->render.cell_width);
          i32 cell_y = (i32)(event.button.y / app->render.cell_height);
          Panel *panel = PanelFromPoint(app->editor.root_panel, cell_x, cell_y);
          if (panel) EditorFocusPanel(&app->editor, panel);
          break;
        }

        case SDL_EVENT_MOUSE_WHEEL: {
          View *view = EditorFocusedView(&app->editor);
          Buffer *buffer = EditorFocusedBuffer(&app->editor);
          if (view && buffer && app->editor.focused_panel) {
            i32 rows = EditorPanelTextHeight(&app->editor, app->editor.focused_panel);
            ViewScrollLines(view, buffer, -(i64)(event.wheel.y * 3.0f), rows);
          }
          break;
        }

        default:
          break;
      }
      // Drain anything else already queued before drawing, so a burst of
      // keystrokes produces one frame rather than one frame each.
    } while (!app->editor.quit && SDL_PollEvent(&event));

    if (app->editor.quit) break;

    // Zoom asks for a new font size; rebuilding the atlas is the app's job.
    if (app->editor.font_size_changed) {
      app->editor.font_size_changed = false;
      if (!RebuildFont(app)) {
        fprintf(stderr, "Could not rebuild the font at size %.1f\n",
                (double)app->editor.font_size);
        break;
      }
    }

    SDL_GetWindowSizeInPixels(app->window, &width, &height);
    SDL_SetRenderDrawColor(app->renderer, 0, 0, 0, 255);
    SDL_RenderClear(app->renderer);

    RenderEditor(&app->render, &app->editor, width, height);

    SDL_RenderPresent(app->renderer);
  }

  SDL_StopTextInput(app->window);
  EditorDestroy(&app->editor);
  GlyphAtlasDestroy(&app->atlas);
  SDL_DestroyRenderer(app->renderer);
  SDL_DestroyWindow(app->window);
  SDL_Quit();

  return 0;
}
