#ifndef THEME_RENDER_BOOTSCREEN_H__
#define THEME_RENDER_BOOTSCREEN_H__

#include "theme/resources.h"

#include "battery.h"
#include "header.h"

void theme_renderBootScreen(SDL_Surface *screen, ThemeImages background, const char *version_str, const char *message_str, int battery_percentage)
{
    SDL_Surface *bg = resource_getSurface(background);
    if (bg) {
        SDL_BlitSurface(bg, NULL, screen, NULL);
    }

    TTF_Font *font = theme_loadFont(theme()->path, theme()->hint.font, 18);
    SDL_Color color = theme()->total.color;

    // Same baseline the hint bar uses (design-space y=450).
    const int hint_center_y =
        g_display.height - theme_scaleY(THEME_FOOTER_HEIGHT) + theme_scaleY(30);

    if (strlen(version_str) > 0) {
        SDL_Surface *version = TTF_RenderUTF8_Blended(font, version_str, color);
        if (version) {
            SDL_Rect rect = {theme_scaleX(20), hint_center_y - version->h / 2};
            SDL_BlitSurface(version, NULL, screen, &rect);
            SDL_FreeSurface(version);
        }
    }

    if (strlen(message_str) > 0) {
        SDL_Surface *message = TTF_RenderUTF8_Blended(font, message_str, color);
        if (message) {
            SDL_Rect rect = {g_display.width - theme_scaleX(20) - message->w,
                             hint_center_y - message->h / 2};
            SDL_BlitSurface(message, NULL, screen, &rect);
            SDL_FreeSurface(message);
        }
    }

    if (battery_percentage >= 0) {
        theme_renderHeaderBattery(screen, battery_percentage);
    }
}

#endif // THEME_RENDER_BOOTSCREEN_H__