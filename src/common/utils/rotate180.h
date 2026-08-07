#ifndef ROTATE_180_H__
#define ROTATE_180_H__

#include <SDL/SDL.h>
#include <stdint.h>

//
//    Rotate a surface 180 degrees, in place.
//
//    Theme `background.png` files are stored inverted - a convention the theme
//    ecosystem has been authored against since 2022 - so this is what makes a
//    themed background read the right way up. It is not compensating for the
//    panel; do not remove it.
//
//    Previously this went through rotozoomSurface(), which was doing a general
//    affine rotation for what is an exact pixel permutation, and carried two
//    hazards:
//
//      - it filled the surface red before blitting the result back, so any
//        region the blit failed to cover showed up as a red seam;
//      - rotozoom pads its output by a pixel or two, which the old code cropped
//        by blitting at a hardcoded (-2, -2). That offset is only correct for
//        whatever padding the shipped SDL_rotozoom happens to produce.
//
//    A 180-degree rotation is just dst[h-1-y][w-1-x] = src[y][x], which is
//    exact, allocation-free for the common 32bpp case, and has no padding or
//    fill to get wrong.
//
static inline SDL_Surface *rotate180(SDL_Surface *original)
{
    int y, x, w, h;

    if (original == NULL)
        return NULL;

    w = original->w;
    h = original->h;

    if (SDL_MUSTLOCK(original) && SDL_LockSurface(original) < 0)
        return original;

    if (original->format->BytesPerPixel == 4) {
        // Swap pixel pairs from both ends towards the middle, so the whole
        // rotation happens in place with no second buffer.
        for (y = 0; y < (h + 1) / 2; y++) {
            uint32_t *top =
                (uint32_t *)((uint8_t *)original->pixels + (size_t)y * original->pitch);
            uint32_t *bot =
                (uint32_t *)((uint8_t *)original->pixels + (size_t)(h - 1 - y) * original->pitch);
            // On the middle row of an odd height, top == bot; stop halfway so
            // the row is not swapped back onto itself.
            int last = (top == bot) ? (w / 2) : w;
            for (x = 0; x < last; x++) {
                uint32_t tmp = top[x];
                top[x] = bot[w - 1 - x];
                bot[w - 1 - x] = tmp;
            }
        }
    }
    else {
        // Any other depth: byte-wise, using the surface's own pixel size.
        const int bpp = original->format->BytesPerPixel;
        for (y = 0; y < (h + 1) / 2; y++) {
            uint8_t *top = (uint8_t *)original->pixels + (size_t)y * original->pitch;
            uint8_t *bot = (uint8_t *)original->pixels + (size_t)(h - 1 - y) * original->pitch;
            int last = (top == bot) ? (w / 2) : w;
            for (x = 0; x < last; x++) {
                uint8_t *a = top + (size_t)x * bpp;
                uint8_t *b = bot + (size_t)(w - 1 - x) * bpp;
                for (int i = 0; i < bpp; i++) {
                    uint8_t tmp = a[i];
                    a[i] = b[i];
                    b[i] = tmp;
                }
            }
        }
    }

    if (SDL_MUSTLOCK(original))
        SDL_UnlockSurface(original);

    return original;
}

#endif // ROTATE_180_H__
