/*
 * libuiscale - run a fixed-resolution SDL program on a larger panel without
 *              changing the framebuffer mode
 *
 * MainUI is closed source and hardcoded to 640x480. It uses SDL 1.2's fbcon
 * backend, which issues FBIOPUT_VSCREENINFO for the geometry the program asks
 * for - so launching it drags the framebuffer from the panel's native 752x560
 * back to 640x480. Measured on a Mini Flip: the framebuffer held 752x560 for as
 * long as an already-running MainUI stayed up, and snapped back to 640x480 the
 * instant MainUI restarted. Every one of those switches is a visible GOP scaler
 * reconfiguration, which is the artifact this whole change exists to remove.
 *
 * So: give MainUI the 640x480 surface it expects, keep the real video surface
 * at the panel's resolution, and upscale one into the other on present. The
 * framebuffer mode is never touched, and MainUI looks exactly as it did before
 * - previously the GOP was doing this same upscale in hardware.
 *
 * Interposing SDL rather than the framebuffer is deliberate. Faking things at
 * the ioctl level would mean also faking mmap and finfo.line_length, and SDL's
 * FB_CheckMode compares the geometry it reads back against what it asked for -
 * so a rewritten mode set makes SDL reject every mode and fail video init
 * outright. Staying above SDL leaves buffer management and panning exactly as
 * they are.
 *
 * It does not leave the panel's 180-degree rotation alone, though - writing the
 * video surface's pixels directly steps around the SDL present path that
 * applies it, so the scaler below has to rotate itself. See scale_argb32().
 *
 * Inert unless it is needed: if /tmp/fb_target_res is missing, or already
 * matches what the program asked for, every call passes straight through. That
 * is what makes this a no-op on 640x480 devices.
 */

#define _GNU_SOURCE

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <SDL/SDL.h>

#include "utils/gfx_present.h"

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define UISCALE_NEON 1
#endif

#define TARGET_FILE "/tmp/fb_target_res"
#define DEBUG_FLAG "/mnt/SDCARD/.fbpin_debug"
#define PROFILE_LOG "/tmp/uiscale.log"

static SDL_Surface *(*real_SetVideoMode)(int, int, int, Uint32) = NULL;
static int (*real_Flip)(SDL_Surface *) = NULL;
static void (*real_UpdateRects)(SDL_Surface *, int, SDL_Rect *) = NULL;
static SDL_Surface *(*real_GetVideoSurface)(void) = NULL;

/* The scaler calls these itself rather than interposing them, but they go
   through pointers for the same reason the Makefile does not pass -lSDL: a
   direct call leaves an undefined SDL_* symbol that the dynamic linker will
   happily bind to whatever SDL happens to be in the process. Resolved lazily,
   so a process with no SDL 1.2 in it never touches them. */
static SDL_Surface *(*real_CreateRGBSurface)(Uint32, int, int, int, Uint32, Uint32,
                                             Uint32, Uint32) = NULL;
static int (*real_FillRect)(SDL_Surface *, SDL_Rect *, Uint32) = NULL;
static Uint32 (*real_MapRGB)(const SDL_PixelFormat *, Uint8, Uint8, Uint8) = NULL;
static int (*real_LockSurface)(SDL_Surface *) = NULL;
static void (*real_UnlockSurface)(SDL_Surface *) = NULL;
static int (*real_UpperBlit)(SDL_Surface *, SDL_Rect *, SDL_Surface *, SDL_Rect *) = NULL;

/* The surface the panel actually scans out, at its native resolution. */
static SDL_Surface *g_real = NULL;
/* The surface handed to the program, at the resolution it asked for. NULL when
   no scaling is in effect and everything passes through. */
static SDL_Surface *g_fake = NULL;
/* Whether g_fake is hardware-backed. Miyoo's SDL applies the panel's
   180-degree rotation inside its accelerated blit path, so when the program's
   blits land in a hardware surface the content arrives already rotated and the
   scaler must not rotate it again. A software shadow gets no such treatment
   and the scaler has to do it. */
static int g_fake_hw = 0;

/*
 * A handle on the SDL 1.2 the program is actually using.
 *
 * It cannot be assumed to be in the global symbol scope. MainUI, st and the
 * standalone emulators list libSDL-1.2 in their own DT_NEEDED, so for them it
 * is. A pygame app does not: CPU Overclock runs on the bundled python2.7, which
 * dlopens pygame's display.so into a *local* scope, and display.so's SDL comes
 * with it. SDL 1.2 is then loaded in the process yet invisible to both
 * RTLD_DEFAULT and RTLD_NEXT. Measured on a Flip from inside such a process:
 * every real SDL_* lookup returned NULL, while SDL_SetVideoMode resolved to
 * 0xb6f99088 - this shim's own export, which is why the hook still fired and
 * the scaler then had nothing to call.
 *
 * dlopen by soname returns the copy already loaded rather than mapping a second
 * one, so this is the same SDL instance the program is calling, with the same
 * state. RTLD_LOCAL keeps it out of the global scope: putting SDL 1.2 there is
 * exactly what broke SDL2 apps and why this file no longer links it.
 *
 * Opened on the first hook call, which can only happen in a program that calls
 * SDL_SetVideoMode - an SDL 1.2 program. An SDL2 app never reaches this.
 */
static void *sdl12_handle(void)
{
    static void *handle = NULL;
    static int tried = 0;

    if (!tried) {
        tried = 1;
        handle = dlopen("libSDL-1.2.so.0", RTLD_LAZY | RTLD_LOCAL);
    }
    return handle;
}

/* Prefer the handle, fall back to the search order. RTLD_NEXT for the entry
   points this shim interposes, so a fallback can never find our own exports and
   recurse; RTLD_DEFAULT for the rest, which this shim does not define. */
static void *sym_interposed(void *sdl, const char *name)
{
    void *p = sdl ? dlsym(sdl, name) : NULL;
    return p ? p : dlsym(RTLD_NEXT, name);
}

static void *sym_helper(void *sdl, const char *name)
{
    void *p = sdl ? dlsym(sdl, name) : NULL;
    return p ? p : dlsym(RTLD_DEFAULT, name);
}

static void resolve_symbols(void)
{
    void *sdl;

    if (real_SetVideoMode)
        return;

    sdl = sdl12_handle();

    real_SetVideoMode = sym_interposed(sdl, "SDL_SetVideoMode");
    real_Flip = sym_interposed(sdl, "SDL_Flip");
    real_UpdateRects = sym_interposed(sdl, "SDL_UpdateRects");
    real_GetVideoSurface = sym_interposed(sdl, "SDL_GetVideoSurface");

    real_CreateRGBSurface = sym_helper(sdl, "SDL_CreateRGBSurface");
    real_FillRect = sym_helper(sdl, "SDL_FillRect");
    real_MapRGB = sym_helper(sdl, "SDL_MapRGB");
    real_LockSurface = sym_helper(sdl, "SDL_LockSurface");
    real_UnlockSurface = sym_helper(sdl, "SDL_UnlockSurface");
    real_UpperBlit = sym_helper(sdl, "SDL_UpperBlit");
}

/* Whether every helper above resolved. Checked once, where scaling is set up;
   past that point the scaler can call them unguarded. */
static int helpers_ready(void)
{
    return real_CreateRGBSurface && real_FillRect && real_MapRGB &&
           real_LockSurface && real_UnlockSurface && real_UpperBlit;
}

static int target_mode(int *w, int *h)
{
    FILE *f = fopen(TARGET_FILE, "r");
    int tw = 0, th = 0, ok = 0;

    if (!f)
        return 0;
    if (fscanf(f, "%dx%d", &tw, &th) == 2 && tw > 0 && th > 0) {
        *w = tw;
        *h = th;
        ok = 1;
    }
    fclose(f);
    return ok;
}

/*
 * Profiling, inert unless /mnt/SDCARD/.fbpin_debug exists.
 *
 * The question this answers: is the software upscale actually the cost, and
 * does the program present whole frames or small dirty rects? Whole-frame
 * rescaling on a tiny dirty rect would be the thing to fix first, and that is
 * not knowable from the source of a closed-source program.
 *
 * Writing the log every present would itself dominate the measurement, so the
 * counters accumulate and are flushed once every REPORT_EVERY presents.
 */
#define REPORT_EVERY 120

static int profile_enabled(void)
{
    static int state = -1;
    if (state < 0)
        state = access(DEBUG_FLAG, F_OK) == 0 ? 1 : 0;
    return state;
}

static double now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
}

/* Whether the MI_GFX hardware path is in use: -1 not yet probed, 0 unavailable
   (CPU scaler), 1 live. Declared here because the profiler reports it. */
static int g_gfx_ok = -1;

static unsigned long p_flip, p_updrect, p_updrects; /* entry point counts */
static unsigned long p_scales;                      /* upscales actually run */
static unsigned long p_skipped; /* presents whose source was unchanged */
static double p_scale_us, p_scale_us_max;
static double p_phase_scale_us, p_phase_copy_us; /* split of the total above */
static double p_phase_mirror_us;  /* uncached read of the hardware shadow */
static double p_window_start;
static unsigned long long p_dirty_px; /* summed area asked for by UpdateRects */
static unsigned long p_dirty_n;

static void profile_report(void)
{
    FILE *f;
    double wall;

    if (p_scales == 0 || (p_scales % REPORT_EVERY) != 0)
        return;

    wall = now_us() - p_window_start;
    f = fopen(PROFILE_LOG, "a");
    if (f) {
        fprintf(f,
                "presents=%lu in %.0fms (%.1f/s) | total avg=%.2fms max=%.2fms "
                "[mirror=%.2fms scale=%.2fms copy=%.2fms] (%.0f%% of wall) | "
                "flip=%lu updrect=%lu updrects=%lu skipped=%lu gfx=%d | avg_dirty=%llupx of %d\n",
                p_scales, wall / 1000.0,
                wall > 0 ? p_scales * 1e6 / wall : 0.0,
                p_scale_us / p_scales / 1000.0, p_scale_us_max / 1000.0,
                p_phase_mirror_us / p_scales / 1000.0,
                p_phase_scale_us / p_scales / 1000.0,
                p_phase_copy_us / p_scales / 1000.0,
                wall > 0 ? p_scale_us * 100.0 / wall : 0.0,
                p_flip, p_updrect, p_updrects, p_skipped, g_gfx_ok,
                p_dirty_n ? p_dirty_px / p_dirty_n : 0ULL,
                g_real ? g_real->w * g_real->h : 0);
        fclose(f);
    }

    p_flip = p_updrect = p_updrects = p_skipped = 0;
    p_scale_us = p_scale_us_max = 0;
    p_phase_scale_us = p_phase_copy_us = p_phase_mirror_us = 0;
    p_dirty_px = 0;
    p_dirty_n = 0;
    p_scales = 0;
    p_window_start = now_us();
}

/*
 * Bilinear ARGB8888 upscale, with a 180-degree rotation applied on output.
 *
 * The rotation is not optional. The panel is mounted upside down and scans the
 * framebuffer out rotated, so everything that reaches the framebuffer has to be
 * stored pre-rotated. SDL's fbcon present path does that for programs that blit
 * their canvas onto the video surface and flip - which is every one of Onion's
 * own apps. This scaler writes into the video surface's pixels directly and so
 * bypasses it entirely, which left MainUI as the one thing on the system
 * rendered upside down. Measured on a Mini Flip: dumping /dev/fb0 while MainUI
 * was on screen showed its content stored right way up, while the OSD's
 * hand-rotated volume meter (osd.h _print_bar, which writes the framebuffer
 * directly and rotates by hand for this same reason) was stored rotated.
 *
 * Costs nothing: it is a reversed index on the destination, not a second pass.
 *
 * Nearest-neighbour was rejected: 640->752 is a ratio of 1.175, so whole
 * columns would be duplicated at an uneven cadence and menu text would shimmer.
 * The GOP was doing a smooth scale before this shim existed, and the result has
 * to be no worse.
 *
 * The horizontal map is the same for every row and every frame, so it is built
 * once. If this ever turns out to be too slow for the menu to feel responsive,
 * the replacement is MI_GFX_BitBlit (see src/pngScale/pngScale.c) - that needs
 * physically-addressed buffers on both sides, which is why it is not the first
 * thing tried.
 */
#define FP_SHIFT 12
#define FP_ONE (1 << FP_SHIFT)

static int *map_x = NULL;    /* source column index per destination column */
static int32_t *frac_x = NULL; /* interpolation weight, 0..FP_ONE */
static int map_w = 0, map_sw = 0;

static int build_x_map(int sw, int dw)
{
    int x;

    if (map_x && map_w == dw && map_sw == sw)
        return 1;

    free(map_x);
    free(frac_x);
    map_x = malloc(sizeof(*map_x) * dw);
    frac_x = malloc(sizeof(*frac_x) * dw);
    if (!map_x || !frac_x) {
        free(map_x);
        free(frac_x);
        map_x = NULL;
        frac_x = NULL;
        map_w = 0;
        return 0;
    }

    for (x = 0; x < dw; x++) {
        /* Corner-aligned mapping: destination x samples source x*sw/dw. The
           half-pixel offset a centre-aligned map would add is well under a
           pixel at these ratios, and this needs no clamping at the edges. */
        int64_t pos = ((int64_t)x * sw << FP_SHIFT) / dw;
        int xi = (int)(pos >> FP_SHIFT);
        if (xi > sw - 2)
            xi = sw - 2;
        if (xi < 0)
            xi = 0;
        map_x[x] = xi;
        frac_x[x] = (int32_t)(pos - ((int64_t)xi << FP_SHIFT));
    }

    map_w = dw;
    map_sw = sw;
    return 1;
}

static void scale_argb32(const uint8_t *src, int spitch, int sw, int sh,
                         uint8_t *dst, int dpitch, int dw, int dh, int rotate)
{
    int y;

    if (sw < 2 || sh < 2 || !build_x_map(sw, dw))
        return;

    for (y = 0; y < dh; y++) {
        int64_t pos = ((int64_t)y * sh << FP_SHIFT) / dh;
        int yi = (int)(pos >> FP_SHIFT);
        int32_t fy, ify;
        const uint32_t *row0, *row1;
        uint32_t *out;
        int x;

        if (yi > sh - 2)
            yi = sh - 2;
        if (yi < 0)
            yi = 0;
        fy = (int32_t)(pos - ((int64_t)yi << FP_SHIFT));
        ify = FP_ONE - fy;

        row0 = (const uint32_t *)(src + (size_t)yi * spitch);
        row1 = (const uint32_t *)(src + (size_t)(yi + 1) * spitch);
        out = (uint32_t *)(dst + (size_t)(rotate ? (dh - 1 - y) : y) * dpitch);

        for (x = 0; x < dw; x++) {
            int xi = map_x[x];
            uint32_t fx = (uint32_t)frac_x[x];
            uint32_t ifx = (uint32_t)FP_ONE - fx;
            uint32_t p00 = row0[xi], p01 = row0[xi + 1];
            uint32_t p10 = row1[xi], p11 = row1[xi + 1];
            uint32_t res = 0;

#ifdef UISCALE_NEON
            /* The four channels of one pixel, one per 32-bit lane. Identical
               arithmetic to the scalar path below - four channels at once
               instead of a four-iteration shift loop.

               Widening a pixel places its bytes in lanes in memory order,
               which on this little-endian target is B,G,R,A: the same order
               the scalar loop walks with shift 0,8,16,24, so narrowing the
               result reassembles the pixel correctly. */
            {
                uint32x4_t v00 = vmovl_u16(vget_low_u16(
                    vmovl_u8(vreinterpret_u8_u32(vdup_n_u32(p00)))));
                uint32x4_t v01 = vmovl_u16(vget_low_u16(
                    vmovl_u8(vreinterpret_u8_u32(vdup_n_u32(p01)))));
                uint32x4_t v10 = vmovl_u16(vget_low_u16(
                    vmovl_u8(vreinterpret_u8_u32(vdup_n_u32(p10)))));
                uint32x4_t v11 = vmovl_u16(vget_low_u16(
                    vmovl_u8(vreinterpret_u8_u32(vdup_n_u32(p11)))));

                uint32x4_t top = vmlaq_n_u32(vmulq_n_u32(v00, (uint32_t)ifx),
                                             v01, (uint32_t)fx);
                uint32x4_t bot = vmlaq_n_u32(vmulq_n_u32(v10, (uint32_t)ifx),
                                             v11, (uint32_t)fx);
                uint32x4_t acc = vmlaq_n_u32(vmulq_n_u32(top, (uint32_t)ify),
                                             bot, (uint32_t)fy);
                uint16x4_t n16 = vmovn_u32(vshrq_n_u32(acc, FP_SHIFT * 2));
                res = vget_lane_u32(
                    vreinterpret_u32_u8(vmovn_u16(vcombine_u16(n16, n16))), 0);
            }
#else
            {
                int shift;
                /* Each channel independently; weights sum to FP_ONE^2. All
                   32-bit: a channel is at most 255, so top and bot are at most
                   255*FP_ONE, and top*ify + bot*fy is at most 255*FP_ONE^2 =
                   0xFF000000 - it cannot overflow. 64-bit multiplies here cost
                   real time on a Cortex-A7 and buy nothing. */
                for (shift = 0; shift < 32; shift += 8) {
                    uint32_t c00 = (p00 >> shift) & 0xff;
                    uint32_t c01 = (p01 >> shift) & 0xff;
                    uint32_t c10 = (p10 >> shift) & 0xff;
                    uint32_t c11 = (p11 >> shift) & 0xff;
                    uint32_t top = c00 * ifx + c01 * fx;
                    uint32_t bot = c10 * ifx + c11 * fx;
                    uint32_t v = (top * (uint32_t)ify + bot * (uint32_t)fy) >>
                                 (FP_SHIFT * 2);
                    res |= (v & 0xff) << shift;
                }
            }
#endif
            out[rotate ? (dw - 1 - x) : x] = res;
        }
    }
}

/*
 * Staging buffer, in ordinary cached RAM.
 *
 * The scaler must not write the framebuffer pixel by pixel. g_real->pixels is
 * the mmap'd framebuffer - uncached, write-combining memory - and the rotation
 * writes each row descending, which defeats write combining outright: every
 * pixel becomes its own bus transaction. Measured on a Mini Flip, scaling
 * straight into the framebuffer cost 226ms per present (~0.5us per pixel), and
 * MainUI presents whole frames, so that was the entire input-to-photon latency.
 *
 * So the scaler renders into cached RAM, where scattered writes are ordinary
 * cache traffic, and the result reaches the framebuffer as one linear ascending
 * memcpy per row - the access pattern write combining is built for.
 */
static uint32_t *g_stage = NULL;
static int g_stage_w = 0, g_stage_h = 0;

/* RAM mirror of the hardware shadow surface.
 *
 * g_fake lives in GFX memory, which is uncached. Bilinear sampling reads four
 * source pixels per destination pixel, so scaling straight out of it means
 * ~1.7M uncached reads per present - the same penalty as writing the
 * framebuffer pixel-by-pixel, in the other direction. One linear memcpy per
 * row pulls the frame into cached RAM first, which is what a burst read is
 * for, and the scaler then runs entirely out of cache. */
static uint32_t *g_srcmirror = NULL;
static int g_srcmirror_w = 0, g_srcmirror_h = 0;

static int ensure_srcmirror(int w, int h)
{
    if (g_srcmirror && g_srcmirror_w == w && g_srcmirror_h == h)
        return 1;
    free(g_srcmirror);
    g_srcmirror = malloc((size_t)w * h * sizeof(uint32_t));
    if (!g_srcmirror) {
        g_srcmirror_w = g_srcmirror_h = 0;
        return 0;
    }
    g_srcmirror_w = w;
    g_srcmirror_h = h;
    return 1;
}

/* Hash of the source frame that produced the current contents of g_stage.
 *
 * MainUI presents every frame twice. The framebuffer is double-buffered
 * (752x1120, two 560-row buffers), and MainUI redraws the same content into
 * whichever buffer it just flipped away from, so the second present of a pair
 * scales pixels that are byte-identical to the ones already staged. Measured on
 * a Mini Flip: 2.0 presents per input event, unchanged by disabling the preview
 * pane or by sending key-down edges only, and the two framebuffer buffers
 * compare byte-identical once the screen settles.
 *
 * The copy still has to run - the two presents write different buffers - but
 * the scale does not. Hashing the already-mirrored source runs out of cache at
 * about 2ms; the scale it skips costs 37ms. */
static uint64_t g_stage_hash;
static int g_stage_valid;

static uint64_t frame_hash(const uint8_t *src, int pitch, int w, int h)
{
    uint32_t h1 = 2166136261u, h2 = 0x9e3779b9u;
    int y, x;

    for (y = 0; y < h; y++) {
        const uint32_t *row = (const uint32_t *)(src + (size_t)y * pitch);
        for (x = 0; x < w; x++) {
            uint32_t v = row[x];
            h1 = (h1 ^ v) * 16777619u;
            h2 = (h2 + v) * 2654435761u + (h2 >> 15);
        }
    }
    return ((uint64_t)h1 << 32) | h2;
}

static int ensure_stage(int w, int h)
{
    if (g_stage && g_stage_w == w && g_stage_h == h)
        return 1;
    g_stage_valid = 0;
    free(g_stage);
    g_stage = malloc((size_t)w * h * sizeof(*g_stage));
    if (!g_stage) {
        g_stage_w = g_stage_h = 0;
        return 0;
    }
    g_stage_w = w;
    g_stage_h = h;
    return 1;
}


/* The hardware scaler lives in ../common/utils/gfx_present.h, shared with
   legacy_present() so both present paths use one implementation. g_gfx_ok
   mirrors gfxp_ready() for the profiler. */

/* Copy the program's canvas onto the real video surface, scaling it up and
   rotating it 180 degrees for the panel. */
static void blit_upscaled(void)
{
    if (!g_fake || !g_real)
        return;

    if (g_fake->format->BytesPerPixel != 4 || g_real->format->BytesPerPixel != 4) {
        /* Should not happen - g_fake is created from g_real's own format. This
           lands unscaled and unrotated, but a wrong-looking screen beats a
           crash, and it is reached only if that invariant is already broken. */
        real_UpperBlit(g_fake, NULL, g_real, NULL);
        return;
    }

    /* g_fake is a hardware surface now, so its pixels are only guaranteed to be
       addressable between lock and unlock. */
    if (SDL_MUSTLOCK(g_fake) && real_LockSurface(g_fake) < 0)
        return;

    if (!ensure_stage(g_real->w, g_real->h)) {
        /* No staging memory: fall back to writing the framebuffer directly.
           Slow, but correct, and it keeps the menu on screen. */
        if (SDL_MUSTLOCK(g_real) && real_LockSurface(g_real) >= 0) {
            scale_argb32((const uint8_t *)g_fake->pixels, g_fake->pitch, g_fake->w,
                         g_fake->h, (uint8_t *)g_real->pixels, g_real->pitch,
                         g_real->w, g_real->h, !g_fake_hw);
            if (SDL_MUSTLOCK(g_real))
                real_UnlockSurface(g_real);
        }
        if (SDL_MUSTLOCK(g_fake))
            real_UnlockSurface(g_fake);
        return;
    }

    {
        double t0 = 0, t1 = 0, tm = 0;
        int prof = profile_enabled();
        int y, row_bytes = g_real->w * 4;
        uint8_t *dst;
        const uint8_t *src = (const uint8_t *)g_fake->pixels;
        int spitch = g_fake->pitch;

        if (prof)
            t0 = now_us();

        /* Phase 0: pull the hardware shadow into cached RAM with linear row
           copies, so the scaler's four taps per pixel do not each fault out to
           uncached GFX memory. */
        {
            /* The mirror lands in the MMA buffer when MI_GFX is live, so the
               same pass that pulls the frame out of uncached memory also puts
               it somewhere the 2D engine can read. Same cost either way. */
            int use_gfx = gfxp_init(g_fake->w, g_fake->h, g_real->w, g_real->h);
            g_gfx_ok = gfxp_ready();

            if (g_fake_hw && (use_gfx || ensure_srcmirror(g_fake->w, g_fake->h))) {
                /* Read g_srcmirror only after ensure_srcmirror() has run: on
                   the first present it is still NULL before that call, and
                   reading it early made this a NULL memcpy whenever the 2D
                   engine was unavailable. Masked for as long as the engine
                   never failed; found the first time it did. */
                void *mirror_into = use_gfx ? gfxp_src() : (void *)g_srcmirror;
                const int srow = g_fake->w * 4;
                for (y = 0; y < g_fake->h; y++)
                    memcpy((uint8_t *)mirror_into + (size_t)y * srow,
                           (const uint8_t *)g_fake->pixels + (size_t)y * g_fake->pitch,
                           srow);
                src = (const uint8_t *)mirror_into;
                spitch = srow;
            }
            else if (use_gfx) {
                /* software shadow: still has to reach MMA memory to be blitted */
                for (y = 0; y < g_fake->h; y++)
                    memcpy((uint8_t *)gfxp_src() + (size_t)y * g_fake->w * 4,
                           (const uint8_t *)g_fake->pixels + (size_t)y * g_fake->pitch,
                           g_fake->w * 4);
            }
        }

        /* Phase 1: scale + rotate into cached RAM - but only if the source
           actually changed. See g_stage_hash: MainUI presents each frame twice
           to fill both framebuffer buffers, and the second of the pair is the
           same pixels. The copy below still has to run, because that present
           targets the other buffer. */
        {
            uint64_t hash = frame_hash(src, spitch, g_fake->w, g_fake->h);
            int unchanged = g_stage_valid && hash == g_stage_hash;

            if (prof)
                tm = now_us();

            if (!unchanged) {
                int done = 0;

                if (gfxp_ready()) {
                    done = gfxp_blit(!g_fake_hw);
                    if (done)
                        memcpy(g_stage, gfxp_dst(),
                               (size_t)g_real->w * g_real->h * 4);
                    else
                        gfxp_teardown(); /* one failure retires the path */
                    g_gfx_ok = gfxp_ready();
                }
                if (!done)
                    scale_argb32(src, spitch, g_fake->w, g_fake->h,
                                 (uint8_t *)g_stage, g_real->w * 4, g_real->w,
                                 g_real->h, !g_fake_hw);
                g_stage_hash = hash;
                g_stage_valid = 1;
            }
            else if (prof) {
                p_skipped++;
            }
        }

        if (prof)
            t1 = now_us();

        /* Phase 2: one linear ascending copy per row into the framebuffer. */
        if (SDL_MUSTLOCK(g_real) && real_LockSurface(g_real) < 0) {
            if (SDL_MUSTLOCK(g_fake))
                real_UnlockSurface(g_fake);
            return;
        }
        dst = (uint8_t *)g_real->pixels;
        for (y = 0; y < g_real->h; y++)
            memcpy(dst + (size_t)y * g_real->pitch, g_stage + (size_t)y * g_real->w,
                   row_bytes);
        if (SDL_MUSTLOCK(g_real))
            real_UnlockSurface(g_real);

        if (prof) {
            double t2 = now_us(), dt = t2 - t0;
            if (p_window_start == 0)
                p_window_start = t0;
            p_scale_us += dt;
            p_phase_mirror_us += tm - t0;
            p_phase_scale_us += t1 - tm;
            p_phase_copy_us += t2 - t1;
            if (dt > p_scale_us_max)
                p_scale_us_max = dt;
            p_scales++;
        }

        if (SDL_MUSTLOCK(g_fake))
            real_UnlockSurface(g_fake);
    }
}

SDL_Surface *SDL_SetVideoMode(int width, int height, int bpp, Uint32 flags)
{
    int nw = 0, nh = 0;

    resolve_symbols();
    if (!real_SetVideoMode)
        return NULL;

    g_fake = NULL;
    g_real = NULL;

    /* Nothing to do if we don't know the panel mode, if it is what the program
       already wants, if this isn't a format the scaler handles, or if the SDL
       helpers the scaler needs are not all present - pass through rather than
       hand back a surface we cannot then present. */
    if (!target_mode(&nw, &nh) || (nw == width && nh == height) || bpp != 32 ||
        !helpers_ready())
        return real_SetVideoMode(width, height, bpp, flags);

    g_real = real_SetVideoMode(nw, nh, bpp, flags);
    if (!g_real)
        return NULL;

    /* Match the real surface's pixel format so the upscale is a straight
       channel-wise interpolation with no conversion.
     *
     * SDL_HWSURFACE, not SDL_SWSURFACE. Miyoo's SDL accelerates blits through
     * MI_GFX (SStar_CheckHWBlit), and that path needs a physically-addressed
     * destination. Handed a software surface, blits that take it silently do
     * nothing - MainUI drew no pixels at all with some themes, showing either
     * uninitialised memory or, once this surface was cleared, plain black.
     * A hardware surface is what a real video surface would have been.
     *
     * Falls back to software if the GFX heap cannot satisfy it, which is worse
     * than nothing only for the themes that need the accelerated path. */
    g_fake = real_CreateRGBSurface(SDL_HWSURFACE, width, height,
                                   g_real->format->BitsPerPixel,
                                   g_real->format->Rmask, g_real->format->Gmask,
                                   g_real->format->Bmask, g_real->format->Amask);
    g_fake_hw = (g_fake != NULL);
    if (!g_fake)
        g_fake = real_CreateRGBSurface(SDL_SWSURFACE, width, height,
                                       g_real->format->BitsPerPixel,
                                       g_real->format->Rmask, g_real->format->Gmask,
                                       g_real->format->Bmask, g_real->format->Amask);
    if (!g_fake)
        return g_real; /* fail open: unscaled, but running */

    /* Clear it. A real fbcon video surface is a framebuffer the driver has
       already zeroed; this one is a fresh allocation whose contents are
       undefined. A program that only repaints dirty regions - relying on the
       rest of the surface persisting between frames, which is exactly what a
       video surface does - will never write the areas it considers unchanged,
       and whatever was in that memory is what gets scaled out to the panel.
       Seen on a Mini Flip as horizontal streaks of noise over an otherwise
       correct menu, but only with themes that do not repaint a full opaque
       background every frame. */
    real_FillRect(g_fake, NULL, real_MapRGB(g_fake->format, 0, 0, 0));

    return g_fake;
}

int SDL_Flip(SDL_Surface *surface)
{
    resolve_symbols();
    if (!real_Flip)
        return -1;

    if (surface != NULL && surface == g_fake && g_real != NULL) {
        int r;
        if (profile_enabled())
            p_flip++;
        blit_upscaled();
        r = real_Flip(g_real);
        if (profile_enabled())
            profile_report();
        return r;
    }
    return real_Flip(surface);
}

void SDL_UpdateRects(SDL_Surface *surface, int numrects, SDL_Rect *rects)
{
    resolve_symbols();
    if (!real_UpdateRects)
        return;

    if (surface != NULL && surface == g_fake && g_real != NULL) {
        /* Partial updates are upscaled as a whole frame: a destination pixel
           can draw on source pixels just outside the dirty rect, so mapping the
           rect across would leave seams. Whole-frame is also what SDL_Flip
           does, and the menu redraws in full anyway. */
        SDL_Rect full = {0, 0, (Uint16)g_real->w, (Uint16)g_real->h};
        if (profile_enabled()) {
            int i;
            p_updrects++;
            for (i = 0; i < numrects; i++)
                p_dirty_px += (unsigned long long)rects[i].w * rects[i].h;
            p_dirty_n += numrects > 0 ? 1 : 0;
        }
        blit_upscaled();
        real_UpdateRects(g_real, 1, &full);
        if (profile_enabled())
            profile_report();
        return;
    }
    real_UpdateRects(surface, numrects, rects);
}

void SDL_UpdateRect(SDL_Surface *surface, Sint32 x, Sint32 y, Uint32 w, Uint32 h)
{
    SDL_Rect rect;

    /* Hooked separately from SDL_UpdateRects: inside libSDL these call each
       other directly, so interposing one does not catch the other. */
    if (surface == NULL)
        return;

    if (profile_enabled() && surface == g_fake)
        p_updrect++; /* forwards to SDL_UpdateRects below, which does the work */

    if (w == 0)
        w = surface->w;
    if (h == 0)
        h = surface->h;

    rect.x = (Sint16)x;
    rect.y = (Sint16)y;
    rect.w = (Uint16)w;
    rect.h = (Uint16)h;
    SDL_UpdateRects(surface, 1, &rect);
}

SDL_Surface *SDL_GetVideoSurface(void)
{
    resolve_symbols();

    if (g_fake)
        return g_fake;
    return real_GetVideoSurface ? real_GetVideoSurface() : NULL;
}
