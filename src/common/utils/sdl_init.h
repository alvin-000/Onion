#ifndef UTILS_SDL_INIT_H__
#define UTILS_SDL_INIT_H__

#include <SDL/SDL.h>
#include <SDL/SDL_image.h>
#include <SDL/SDL_rotozoom.h>
#include <SDL/SDL_ttf.h>

#ifdef HAS_AUDIO
#include <SDL/SDL_mixer.h>
#endif

#include "system/display.h"
#include "theme/load.h"

static SDL_Surface *video;
static SDL_Surface *screen;

static bool _SDL_InitCommon(void)
{
    display_getRenderResolution();

#ifdef HAS_AUDIO
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO);
#else
    SDL_Init(SDL_INIT_VIDEO);
#endif
    SDL_ShowCursor(SDL_DISABLE);
    SDL_EnableKeyRepeat(300, 50);
    TTF_Init();

    video = SDL_SetVideoMode(g_display.width, g_display.height, 32, SDL_HWSURFACE);
    return video != NULL;
}

//
//    Native canvas: the app draws at the panel's real resolution and the theme
//    layer scales its 640x480-authored assets and geometry up to match.
//
bool SDL_InitDefault(ScaleSurfaceFunc scaleSurface)
{
    if (!_SDL_InitCommon())
        return false;

    // Take the size from the surface we actually got, not from what we asked
    // for - if SDL substituted a mode, the theme scale must follow it.
    theme_initScaling((double)video->w / THEME_DESIGN_WIDTH,
                      (double)video->h / THEME_DESIGN_HEIGHT, scaleSurface);

    screen = SDL_CreateRGBSurface(SDL_HWSURFACE, video->w, video->h, 32, 0, 0, 0, 0);

#ifdef HAS_AUDIO
    if (Mix_OpenAudio(48000, 32784, 2, 4096) < 0)
        return false;
#endif

    return true;
}


#endif // UTILS_SDL_INIT_H__
