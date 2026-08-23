#ifndef UTILS_SDL_LEGACY_H__
#define UTILS_SDL_LEGACY_H__

#include <SDL/SDL.h>
#include <SDL/SDL_rotozoom.h>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define LEGACY_NEON 1
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "system/display.h"

//
//    Fixed-design-space rendering on a larger panel.
//
//    Some apps have dense absolute-pixel layouts and their own 640x480 artwork
//    (packageManager's sprite set, themeSwitcher's 480x360 previews, and so
//    on). Converting those to lay out natively means re-authoring the art, and
//    none of it can happen before the framebuffer is pinned to the panel's
//    native mode - which has to happen for every app at once, since the whole
//    point is that the mode never changes.
//
//    So these apps keep drawing exactly as they always have, into a canvas of
//    their own fixed size, and the result is upscaled on present. That is
//    precisely what the GOP scaler was doing for them before, so they look
//    unchanged. Each can be promoted to a native layout later, independently.
//
//    On a 640x480 device the canvas and the screen are the same size and this
//    is a plain blit - byte-for-byte the old behaviour.
//

//
//    Open a video surface at the panel's resolution, plus a drawing canvas at
//    the app's design size. Returns the video surface, or NULL on failure;
//    *canvas receives the surface to draw into.
//
static inline SDL_Surface *legacy_openDisplay(int design_w, int design_h,
                                              Uint32 flags, SDL_Surface **canvas)
{
    SDL_Surface *vid;

    display_getRenderResolution();

    vid = SDL_SetVideoMode(g_display.width, g_display.height, 32, flags);

    // Inert unless /mnt/SDCARD/.fbpin_debug exists. SDL's fbcon backend does
    // not always hand back the geometry it was asked for - it can settle on a
    // smaller surface and centre it in the video mode - and the difference is
    // invisible from inside the app. Record what was asked versus what landed.
    if (access("/mnt/SDCARD/.fbpin_debug", F_OK) == 0) {
        FILE *lf = fopen("/tmp/legacy.log", "a");
        if (lf) {
            fprintf(lf, "legacy_openDisplay: fb=%dx%d asked=%dx%d got=%dx%d%s canvas=%dx%d\n",
                    g_display.width, g_display.height, g_display.width, g_display.height,
                    vid ? vid->w : -1, vid ? vid->h : -1,
                    (vid && (vid->w != g_display.width || vid->h != g_display.height))
                        ? " *** MISMATCH ***"
                        : "",
                    design_w, design_h);
            fclose(lf);
        }
    }

    if (vid == NULL) {
        if (canvas)
            *canvas = NULL;
        return NULL;
    }

    if (canvas)
        *canvas = SDL_CreateRGBSurface(flags, design_w, design_h, 32, 0, 0, 0, 0);

    return vid;
}

//
//    Blit the canvas onto the video surface, scaling it if the two differ, then
//    flip. Replaces the
//
//        SDL_BlitSurface(screen, NULL, video, NULL); SDL_Flip(video);
//
//    pair these apps repeat at every redraw.
//
//
//    The panel is mounted upside down. Miyoo's libSDL compensates when it
//    presents - but only for 640x480 surfaces, the Mini's own size, which the
//    vendor patch has hardcoded. A surface of any other size reaches the panel
//    unrotated and appears inverted.
//
//    Measured on a Mini Flip, with the GOP at 1:1 passthrough in both cases:
//    a 640x480 surface centred in the framebuffer came out the right way up,
//    and a native 752x560 surface came out upside down. So the rotation has to
//    be applied here for any surface the vendor's SDL will not handle - and
//    must NOT be applied at 640x480, where SDL still does it and a second
//    rotation would put it back. That is what keeps 640x480 devices correct.
//
static inline bool legacy_needsRotation(SDL_Surface *vid)
{
    return !(vid->w == 640 && vid->h == 480);
}

//
//    180-degree copy. Both surfaces must be 32bpp and the same size.
//
static inline void legacy_blit180(SDL_Surface *src, SDL_Surface *dst)
{
    int y, x, w, h;

    if (SDL_MUSTLOCK(dst) && SDL_LockSurface(dst) < 0)
        return;

    w = src->w < dst->w ? src->w : dst->w;
    h = src->h < dst->h ? src->h : dst->h;

    for (y = 0; y < h; y++) {
        const uint32_t *s =
            (const uint32_t *)((const uint8_t *)src->pixels + (size_t)y * src->pitch);
        uint32_t *d =
            (uint32_t *)((uint8_t *)dst->pixels + (size_t)(dst->h - 1 - y) * dst->pitch);
        for (x = 0; x < w; x++)
            d[dst->w - 1 - x] = s[x];
    }

    if (SDL_MUSTLOCK(dst))
        SDL_UnlockSurface(dst);
}

//
//    Combined bilinear scale + 180-degree rotate, straight from the canvas into
//    a staging buffer in ordinary RAM.
//
//    This replaces zoomSurface() plus a separate rotation pass. Both were
//    expensive for the same reason the libuiscale scaler was: the shipped
//    libSDL_rotozoom.so is built with no optimisation at all (see
//    include/SDL/Makefile), so every present paid for an unoptimised bilinear
//    pass over 421k pixels, and the rotation then wrote those pixels into the
//    framebuffer one at a time in descending order - the worst possible pattern
//    for uncached write-combining memory.
//
//    Routing the result through SDL_BlitSurface instead was worse still: a
//    large *software* source blitted to the display surface sends themeSwitcher
//    into a syscall-bound spin it never comes out of (measured: 100% CPU, two
//    thirds of it kernel time, no allocation). Only hardware surfaces take the
//    SStar_CheckHWBlit path. So the framebuffer write here is a plain linear
//    per-row copy, which is what write combining is built for.
//
//    Compiled at -O2 in the apps that use it, this is the same shape that took
//    libuiscale from 226ms to 47ms per present.
//
#include "utils/gfx_present.h"

#define LEGACY_FP_SHIFT 12
#define LEGACY_FP_ONE (1 << LEGACY_FP_SHIFT)

static uint32_t *_legacy_stage = NULL;
static int _legacy_stage_w = 0, _legacy_stage_h = 0;

// RAM mirror of the canvas.
//
// These apps ask for their canvas with SDL_HWSURFACE, and Miyoo's SDL honours
// it (FB_AllocHWSurface), so the pixels live in uncached GFX memory. Bilinear
// sampling reads four source pixels per destination pixel, which means well
// over a million uncached reads per present. One linear memcpy per row pulls
// the frame into cached RAM first - a burst read, which that memory is good at
// - and the scaler then runs entirely out of cache. Measured equivalent in
// libuiscale: 20ms of every frame.
static uint32_t *_legacy_srcmirror = NULL;
static int _legacy_srcmirror_w = 0, _legacy_srcmirror_h = 0;

static inline int _legacy_ensureMirror(int w, int h)
{
    if (_legacy_srcmirror && _legacy_srcmirror_w == w && _legacy_srcmirror_h == h)
        return 1;
    free(_legacy_srcmirror);
    _legacy_srcmirror = (uint32_t *)malloc((size_t)w * h * sizeof(uint32_t));
    if (!_legacy_srcmirror) {
        _legacy_srcmirror_w = _legacy_srcmirror_h = 0;
        return 0;
    }
    _legacy_srcmirror_w = w;
    _legacy_srcmirror_h = h;
    return 1;
}

// Horizontal mapping is identical for every row and every frame, so it is built
// once per size. Computing it inline cost a 64-bit multiply and a 64-bit divide
// per destination pixel - 421k of each, every present.
static int *_legacy_mapx = NULL;
static uint32_t *_legacy_fracx = NULL;
static int _legacy_map_dw = 0, _legacy_map_sw = 0;

static inline int _legacy_ensureStage(int w, int h)
{
    if (_legacy_stage && _legacy_stage_w == w && _legacy_stage_h == h)
        return 1;
    free(_legacy_stage);
    _legacy_stage = (uint32_t *)malloc((size_t)w * h * sizeof(uint32_t));
    if (!_legacy_stage) {
        _legacy_stage_w = _legacy_stage_h = 0;
        return 0;
    }
    _legacy_stage_w = w;
    _legacy_stage_h = h;
    return 1;
}

static inline int _legacy_ensureMap(int sw, int dw)
{
    int x;

    if (_legacy_mapx && _legacy_map_dw == dw && _legacy_map_sw == sw)
        return 1;

    free(_legacy_mapx);
    free(_legacy_fracx);
    _legacy_mapx = (int *)malloc(sizeof(int) * dw);
    _legacy_fracx = (uint32_t *)malloc(sizeof(uint32_t) * dw);
    if (!_legacy_mapx || !_legacy_fracx) {
        free(_legacy_mapx);
        free(_legacy_fracx);
        _legacy_mapx = NULL;
        _legacy_fracx = NULL;
        _legacy_map_dw = 0;
        return 0;
    }

    for (x = 0; x < dw; x++) {
        int pos = (int)(((int64_t)x * sw << LEGACY_FP_SHIFT) / dw);
        int xi = pos >> LEGACY_FP_SHIFT;
        if (xi > sw - 2)
            xi = sw - 2;
        if (xi < 0)
            xi = 0;
        _legacy_mapx[x] = xi;
        _legacy_fracx[x] = (uint32_t)(pos - (xi << LEGACY_FP_SHIFT));
    }

    _legacy_map_dw = dw;
    _legacy_map_sw = sw;
    return 1;
}

//
//    Scale src (32bpp) to dw x dh, rotated 180 degrees, into dst.
//    All arithmetic is 32-bit: a channel is at most 255, so the accumulator
//    tops out at 255 * FP_ONE^2 = 0xFF000000 and cannot overflow.
//
static inline void _legacy_scaleRotate(const SDL_Surface *src, uint32_t *dst,
                                       int dw, int dh)
{
    int sw = src->w, sh = src->h, x, y;
    const uint8_t *sp = (const uint8_t *)src->pixels;

    if (sw < 2 || sh < 2 || dw < 1 || dh < 1 || !_legacy_ensureMap(sw, dw))
        return;

    for (y = 0; y < dh; y++) {
        int pos_y = (int)(((int64_t)y * sh << LEGACY_FP_SHIFT) / dh);
        int yi = pos_y >> LEGACY_FP_SHIFT;
        uint32_t fy, ify;
        const uint32_t *row0, *row1;
        uint32_t *out;

        if (yi > sh - 2)
            yi = sh - 2;
        if (yi < 0)
            yi = 0;
        fy = (uint32_t)(pos_y - (yi << LEGACY_FP_SHIFT));
        ify = (uint32_t)LEGACY_FP_ONE - fy;

        row0 = (const uint32_t *)(sp + (size_t)yi * src->pitch);
        row1 = (const uint32_t *)(sp + (size_t)(yi + 1) * src->pitch);
        out = dst + (size_t)(dh - 1 - y) * dw; // rotated: reversed row

        // Walk the destination backwards so the rotation is a straight
        // sequential write rather than a scattered one.
        out += dw - 1;

        for (x = 0; x < dw; x++) {
            int xi = _legacy_mapx[x];
            uint32_t fx = _legacy_fracx[x];
            uint32_t ifx = (uint32_t)LEGACY_FP_ONE - fx;
            uint32_t p00 = row0[xi], p01 = row0[xi + 1];
            uint32_t p10 = row1[xi], p11 = row1[xi + 1];
            uint32_t res = 0;

#ifdef LEGACY_NEON
            /* Four channels of one pixel, one per 32-bit lane - the same
               arithmetic as the scalar path, without the shift loop. Widening
               places the bytes in lanes in memory order (B,G,R,A here), which
               is the order the scalar loop walks with shift 0,8,16,24, so the
               narrowed result reassembles correctly. */
            {
                uint32x4_t v00 = vmovl_u16(vget_low_u16(
                    vmovl_u8(vreinterpret_u8_u32(vdup_n_u32(p00)))));
                uint32x4_t v01 = vmovl_u16(vget_low_u16(
                    vmovl_u8(vreinterpret_u8_u32(vdup_n_u32(p01)))));
                uint32x4_t v10 = vmovl_u16(vget_low_u16(
                    vmovl_u8(vreinterpret_u8_u32(vdup_n_u32(p10)))));
                uint32x4_t v11 = vmovl_u16(vget_low_u16(
                    vmovl_u8(vreinterpret_u8_u32(vdup_n_u32(p11)))));

                uint32x4_t top = vmlaq_n_u32(vmulq_n_u32(v00, ifx), v01, fx);
                uint32x4_t bot = vmlaq_n_u32(vmulq_n_u32(v10, ifx), v11, fx);
                uint32x4_t acc = vmlaq_n_u32(vmulq_n_u32(top, ify), bot, fy);
                uint16x4_t n16 = vmovn_u32(vshrq_n_u32(acc, LEGACY_FP_SHIFT * 2));
                res = vget_lane_u32(
                    vreinterpret_u32_u8(vmovn_u16(vcombine_u16(n16, n16))), 0);
            }
#else
            {
                int shift;
                for (shift = 0; shift < 32; shift += 8) {
                    uint32_t c00 = (p00 >> shift) & 0xff;
                    uint32_t c01 = (p01 >> shift) & 0xff;
                    uint32_t c10 = (p10 >> shift) & 0xff;
                    uint32_t c11 = (p11 >> shift) & 0xff;
                    uint32_t top = c00 * ifx + c01 * fx;
                    uint32_t bot = c10 * ifx + c11 * fx;
                    uint32_t v = (top * ify + bot * fy) >> (LEGACY_FP_SHIFT * 2);
                    res |= (v & 0xff) << shift;
                }
            }
#endif
            *out-- = res; // rotated: reversed column, written sequentially
        }
    }
}

static inline void legacy_present(SDL_Surface *canvas, SDL_Surface *vid)
{
    if (canvas == NULL || vid == NULL)
        return;

    // 640x480 device: canvas and screen are the same size and Miyoo's SDL
    // still applies the rotation itself on this path. Plain blit, byte-for-byte
    // the old behaviour.
    if (!legacy_needsRotation(vid) || canvas->format->BytesPerPixel != 4 ||
        vid->format->BytesPerPixel != 4) {
        SDL_BlitSurface(canvas, NULL, vid, NULL);
        SDL_Flip(vid);
        return;
    }

    if (_legacy_ensureStage(vid->w, vid->h)) {
        int y, row_bytes = vid->w * 4;
        uint8_t *d;
        SDL_Surface *src = canvas;
        SDL_Surface mirror;

        // Pull the canvas into cached RAM first when it is hardware-backed.
        if ((canvas->flags & SDL_HWSURFACE) && _legacy_ensureMirror(canvas->w, canvas->h)) {
            const int srow = canvas->w * 4;
            if (!(SDL_MUSTLOCK(canvas) && SDL_LockSurface(canvas) < 0)) {
                for (y = 0; y < canvas->h; y++)
                    memcpy((uint8_t *)_legacy_srcmirror + (size_t)y * srow,
                           (const uint8_t *)canvas->pixels + (size_t)y * canvas->pitch,
                           srow);
                if (SDL_MUSTLOCK(canvas))
                    SDL_UnlockSurface(canvas);
                // A stand-in surface describing the mirror, so the scaler needs
                // no second code path.
                mirror = *canvas;
                mirror.pixels = _legacy_srcmirror;
                mirror.pitch = srow;
                src = &mirror;
            }
        }

        /* Hardware scale when the 2D engine is available, CPU otherwise. The
           mirror above already put the frame in cached RAM; when MI_GFX is live
           it goes into the MMA buffer instead, at the same cost, and the blit
           replaces the bilinear loop. Rotation is unconditional here because
           this path writes vid->pixels directly and so never gets the vendor
           SDL's rotating blit - exactly what _legacy_scaleRotate() assumes. */
        {
            int done = 0;
            if (gfxp_init(src->w, src->h, vid->w, vid->h)) {
                int yy;
                const int srow = src->w * 4;
                for (yy = 0; yy < src->h; yy++)
                    memcpy((uint8_t *)gfxp_src() + (size_t)yy * srow,
                           (const uint8_t *)src->pixels + (size_t)yy * src->pitch,
                           srow);
                done = gfxp_blit(1);
                if (done)
                    memcpy(_legacy_stage, gfxp_dst(),
                           (size_t)vid->w * vid->h * 4);
                else
                    gfxp_teardown();
            }
            if (!done)
                _legacy_scaleRotate(src, _legacy_stage, vid->w, vid->h);
        }

        if (SDL_MUSTLOCK(vid) && SDL_LockSurface(vid) < 0)
            return;
        d = (uint8_t *)vid->pixels;
        for (y = 0; y < vid->h; y++)
            memcpy(d + (size_t)y * vid->pitch, _legacy_stage + (size_t)y * vid->w,
                   row_bytes);
        if (SDL_MUSTLOCK(vid))
            SDL_UnlockSurface(vid);
    }
    else {
        // No staging memory. Correct but slow: straight into the framebuffer.
        SDL_Surface *scaled = zoomSurface(canvas, (double)vid->w / canvas->w,
                                          (double)vid->h / canvas->h, SMOOTHING_ON);
        legacy_blit180(scaled ? scaled : canvas, vid);
        if (scaled)
            SDL_FreeSurface(scaled);
    }

    SDL_Flip(vid);
}

#endif // UTILS_SDL_LEGACY_H__
