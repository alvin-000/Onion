#ifndef RENDER_MENU_H__
#define RENDER_MENU_H__

#include <SDL/SDL_rotozoom.h>

#include "components/list.h"
#include "system/display.h"
#include "theme/background.h"
#include "theme/config.h"
#include "theme/resources.h"

// static SDL_Color color_black = {0, 0, 0};

void theme_renderListLabel(SDL_Surface *screen, const char *label, SDL_Color fg,
                           int offset_x, int center_y, bool is_active,
                           int label_end, bool disabled)
{
    TTF_Font *list_font = resource_getFont(LIST);

    SDL_Surface *item_label = TTF_RenderUTF8_Blended(list_font, label, fg);
    SDL_Rect item_label_rect = {offset_x, center_y - item_label->h / 2};

    SDL_Rect label_crop = {0, 0, label_end - theme_scaleX(30), item_label->h};

    /* Maybe shadows will be an option in the future
    SDL_Rect item_shadow_rect = {item_label_rect.x + 1, item_label_rect.y + 2};

    if (is_active) {
        SDL_Surface *item_shadow =
            TTF_RenderUTF8_Blended(list_font, label, color_black);
        SDL_BlitSurface(item_shadow, &label_crop, screen, &item_shadow_rect);
        SDL_FreeSurface(item_shadow);
    }*/

    if (disabled) {
        surfaceSetAlpha(item_label, HIDDEN_ITEM_ALPHA);
    }

    SDL_BlitSurface(item_label, &label_crop, screen, &item_label_rect);
    SDL_FreeSurface(item_label);
}

typedef struct {
    SDL_Surface *background;
    SDL_Rect dim;
    SDL_Rect pos;
    bool show_dividers;
    bool stretch_y;
    bool preview_bg;
    bool preview_stretch;
    int preview_width;
    bool preview_smoothing;
} ListRenderParams_s;

void theme_renderListCustom(SDL_Surface *screen, List *list, ListRenderParams_s params)
{
    if (params.background)
        SDL_BlitSurface(params.background, &params.dim, screen, &params.pos);

    bool list_small = list->list_type == LIST_SMALL;

    TTF_Font *list_font = resource_getFont(LIST);

    // Screen edges come from the panel, not from the design space: the two only
    // agree on a 640x480 device. Design-space sizes still scale.
    const int screen_right = g_display.width;
    const int list_bottom = g_display.height - theme_scaleY(THEME_FOOTER_HEIGHT);

    SDL_Rect item_bg_rect = {0, theme_scaleY(THEME_HEADER_HEIGHT)},
             item_div_size = {0, 0, params.dim.w, theme_scaleY(4)},
             item_bg_size = {0, 0, params.dim.w, theme_scaleY(list_small ? 56.0 : 90.0)},
             toggle_rect = {0, 0};

    int list_height = params.stretch_y ? params.dim.h - theme_scaleY(10) : list_bottom;
    int item_padding = list_small && params.show_dividers ? theme_scaleY(4) : 0;
    int item_height = params.stretch_y ? list_height / list->scroll_height : theme_scaleY(list_small ? 60.0 : 90.0);
    int label_y = theme_scaleY(list_small ? 27.0 : 37.0);
    SDL_Surface *item_bg = resource_getSurface(list_small ? BG_LIST_S : BG_LIST_L);

    int menu_pos_y = params.stretch_y ? params.pos.y + theme_scaleY(4) + (list_height - list->scroll_height * item_height) / 2
                                      : list_bottom - list->scroll_height * item_height;
    int last_item = list->scroll_pos + list->scroll_height;

    if (last_item > list->item_count)
        last_item = list->item_count;

    ListItem *active_preview = NULL;

    // Check if there are hidden items
    bool has_disabled_items = false;
    for (int i = list->scroll_pos; i < last_item; i++) {
        if (list->items[i].disabled && !list->items[i].show_opaque) {
            has_disabled_items = true;
            break;
        }
    }

    // Load hidden items if needed
    if (has_disabled_items) {
        theme_loadHiddenItems();
    }

    if (list->has_sticky) {
        ListItem *active_item = &list->items[list->active_pos];
        if (params.show_dividers)
            SDL_BlitSurface(resource_getSurface(HORIZONTAL_DIVIDER), &item_div_size, screen, &item_bg_rect);
        theme_renderListLabel(screen, active_item->sticky_note, theme()->list.color, theme_scaleX(20), item_bg_rect.y + label_y, false, screen_right, true);
    }

    for (int i = list->scroll_pos; i < last_item; i++) {
        ListItem *item = &list->items[i];
        bool show_disabled = item->disabled && !item->show_opaque;

        item_bg_rect.y = menu_pos_y + (i - list->scroll_pos) * item_height;

        if (params.stretch_y) {
            item_bg_rect.y += (item_height - item_bg_size.h) / 2;
        }

        if (params.show_dividers)
            SDL_BlitSurface(resource_getSurface(HORIZONTAL_DIVIDER), &item_div_size, screen, &item_bg_rect);
        item_bg_rect.y += item_padding;

        if (i == list->active_pos) {
            SDL_BlitSurface(item_bg, &item_bg_size, screen, &item_bg_rect);

            if (item->preview_ptr == NULL && strlen(item->preview_path) > 0 && is_file(item->preview_path)) {
                item->preview_ptr = (void *)IMG_Load(item->preview_path);
            }

            if (item->preview_ptr != NULL)
                active_preview = item;
        }

        const int item_center_y = item_bg_rect.y + item_bg_size.h / 2;
        const int multivalue_width = theme_scaleX(226);
        int label_end = screen_right;
        int offset_x = theme_scaleX(20);

        if (item->icon_ptr != NULL) {
            SDL_Surface *icon = (SDL_Surface *)item->icon_ptr;
            if (icon->w > 1) {
                SDL_Rect icon_rect = {offset_x, item_center_y - icon->h / 2};
                SDL_BlitSurface(icon, NULL, screen, &icon_rect);
                offset_x += icon->w + theme_scaleX(17);
            }
        }

        if (item->item_type == TOGGLE) {
            SDL_Surface *toggle = show_disabled ? (item->value == 1 ? g_hidden_items.toggle_on : g_hidden_items.toggle_off) : (resource_getSurface(item->value == 1 ? TOGGLE_ON : TOGGLE_OFF));
            toggle_rect.x = screen_right - theme_scaleX(20) - toggle->w;
            toggle_rect.y = item_center_y - toggle->h / 2;
            label_end = toggle_rect.x;
            SDL_BlitSurface(toggle, NULL, screen, &toggle_rect);
        }
        else if (item->item_type == MULTIVALUE) {
            SDL_Surface *arrow_left = show_disabled ? g_hidden_items.arrow_left : resource_getSurface(LEFT_ARROW);
            SDL_Surface *arrow_right = show_disabled ? g_hidden_items.arrow_right : resource_getSurface(RIGHT_ARROW);
            SDL_Rect arrow_left_pos = {
                screen_right - theme_scaleX(20) - arrow_right->w - multivalue_width - arrow_left->w,
                item_center_y - arrow_left->h / 2};
            SDL_Rect arrow_right_pos = {
                screen_right - theme_scaleX(20) - arrow_right->w,
                item_center_y - arrow_right->h / 2};
            SDL_BlitSurface(arrow_left, NULL, screen, &arrow_left_pos);
            SDL_BlitSurface(arrow_right, NULL, screen, &arrow_right_pos);
            label_end = arrow_left_pos.x;

            char value_str[STR_MAX];
            list_getItemValueLabel(item, value_str);
            SDL_Surface *value_label = TTF_RenderUTF8_Blended(list_font, value_str, theme()->list.color);
            if (show_disabled) {
                surfaceSetAlpha(value_label, HIDDEN_ITEM_ALPHA);
            }
            SDL_Rect value_size = {0, 0, multivalue_width, value_label->h};
            int label_width = value_label->w > value_size.w ? value_size.w : value_label->w;
            SDL_Rect value_pos = {
                screen_right - theme_scaleX(20) - arrow_right->w - multivalue_width / 2 - label_width / 2,
                item_center_y - value_size.h / 2};
            SDL_BlitSurface(value_label, &value_size, screen, &value_pos);
        }

        theme_renderListLabel(screen, item->label, theme()->list.color,
                              offset_x, item_bg_rect.y + label_y,
                              list->active_pos == i, label_end, show_disabled);

        if (!list_small && strlen(item->description)) {
            theme_renderListLabel(
                screen, item->description, theme()->grid.color, offset_x,
                item_bg_rect.y + theme_scaleY(62), list->active_pos == i, label_end, show_disabled);
        }
    }

    if (active_preview != NULL) {
        int preview_width = theme_scaleX(params.preview_width);
        int preview_x = screen_right;

        if (params.preview_bg) {
            SDL_Surface *preview_bg = resource_getSurface(PREVIEW_BG);
            SDL_Rect preview_bg_rect = {screen_right - preview_bg->w, theme_scaleY(THEME_HEADER_HEIGHT)};
            SDL_BlitSurface(preview_bg, NULL, screen, &preview_bg_rect);
            preview_x -= preview_bg->w;
        }
        else {
            preview_x -= preview_width;
        }

        SDL_Surface *preview = (SDL_Surface *)active_preview->preview_ptr;
        if (preview) {
            bool free_after = false;

            if (preview->w > preview_width || (params.preview_stretch && preview->w < preview_width)) {
                double scale = (double)preview_width / (double)preview->w;
                preview = zoomSurface(preview, scale, scale, params.preview_smoothing ? SMOOTHING_ON : SMOOTHING_OFF);
                free_after = true;
            }

            SDL_Rect preview_rect = {preview_x + (preview_width - preview->w) / 2,
                                     g_display.height / 2 - preview->h / 2};
            SDL_BlitSurface(preview, NULL, screen, &preview_rect);

            if (free_after)
                SDL_FreeSurface(preview);
        }
    }
}

void theme_renderList(SDL_Surface *screen, List *list)
{
    ListRenderParams_s params = {
        .background = theme_background(),
        .dim = {0, theme_scaleY(THEME_HEADER_HEIGHT), g_display.width,
                g_display.height - theme_scaleY(THEME_HEADER_HEIGHT + THEME_FOOTER_HEIGHT)},
        .pos = {0, theme_scaleY(THEME_HEADER_HEIGHT)},
        .show_dividers = true,
        .stretch_y = false,
        .preview_bg = true,
        .preview_stretch = false,
        .preview_width = 250,
        .preview_smoothing = false,
    };

    theme_renderListCustom(screen, list, params);
}

#endif // RENDER_MENU_H__
