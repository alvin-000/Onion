#ifndef UTILS_GFX_PRESENT_H__
#define UTILS_GFX_PRESENT_H__

/*
 * Scale-and-rotate on the SigmaStar 2D engine, shared by the two present paths
 * that need it: libuiscale (for MainUI and anything run_scaled.sh wraps) and
 * legacy_present() (for this project's own SDL apps).
 *
 * One MI_GFX_BitBlit does the scale and the 180-degree rotation together. On a
 * 752x560 panel that is ~3ms against ~37ms for the NEON bilinear loop it
 * replaces.
 *
 * Both buffers are MMA allocations of our own, because the engine needs
 * physically-addressed contiguous memory and an SDL surface cannot supply it:
 * the vendor SDL leaves SDL_Surface.unused1 at zero, /proc/self/pagemap returns
 * zeroed PFNs on this kernel even to root, and the surfaces are ordinary shared
 * anonymous mappings. All three were checked on hardware.
 *
 * The caller writes its frame into gfxp_src(), calls gfxp_blit(), and copies
 * out of gfxp_dst(). That source copy is not extra work - both callers already
 * mirrored the frame into cached RAM to avoid sampling uncached memory four
 * times per output pixel; this just changes where the mirror lands.
 *
 * Header-only with file-scope state, matching sdl_legacy.h: each consumer is a
 * separate process with its own present path, so there is nothing to share at
 * runtime and nothing to link.
 *
 * Everything is a fast path. Any failure retires the engine for the life of the
 * process and the caller falls back to its CPU scaler.
 */

#include <mi_gfx.h>
#include <mi_sys.h>
#include <stddef.h>
#include <string.h>

#define GFXP_ALIGN4K(v) (((v) + 4095) & ~4095)

static int gfxp_state = -1; /* -1 unprobed, 0 unavailable, 1 usable */
static MI_PHY gfxp_src_pa, gfxp_dst_pa;
static void *gfxp_src_va, *gfxp_dst_va;
static size_t gfxp_src_sz, gfxp_dst_sz;
static int gfxp_sw, gfxp_sh, gfxp_dw, gfxp_dh;

static inline void *gfxp_src(void) { return gfxp_src_va; }
static inline void *gfxp_dst(void) { return gfxp_dst_va; }
static inline int gfxp_ready(void) { return gfxp_state == 1; }

static inline void gfxp_teardown(void)
{
    if (gfxp_src_va) { MI_SYS_Munmap(gfxp_src_va, gfxp_src_sz); gfxp_src_va = NULL; }
    if (gfxp_src_pa) { MI_SYS_MMA_Free(gfxp_src_pa); gfxp_src_pa = 0; }
    if (gfxp_dst_va) { MI_SYS_Munmap(gfxp_dst_va, gfxp_dst_sz); gfxp_dst_va = NULL; }
    if (gfxp_dst_pa) { MI_SYS_MMA_Free(gfxp_dst_pa); gfxp_dst_pa = 0; }
    gfxp_sw = gfxp_sh = gfxp_dw = gfxp_dh = 0;
    gfxp_state = 0;
}

/* Idempotent. Returns 1 when the engine is ready for this geometry. */
static inline int gfxp_init(int sw, int sh, int dw, int dh)
{
    if (gfxp_state == 0)
        return 0;
    if (gfxp_state == 1 && sw == gfxp_sw && sh == gfxp_sh &&
        dw == gfxp_dw && dh == gfxp_dh)
        return 1;
    if (gfxp_state == 1)
        gfxp_teardown(); /* geometry changed; sets state 0, cleared below */

    gfxp_state = 0;
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
        return 0;

    /* Both are almost certainly re-opens: the vendor SDL already drives MI_GFX
       in these processes. Both calls are refcounted. */
    if (MI_SYS_Init() != MI_SUCCESS)
        return 0;
    if (MI_GFX_Open() != MI_SUCCESS)
        return 0;

    gfxp_src_sz = GFXP_ALIGN4K((size_t)sw * sh * 4);
    gfxp_dst_sz = GFXP_ALIGN4K((size_t)dw * dh * 4);

    if (MI_SYS_MMA_Alloc(NULL, gfxp_src_sz, &gfxp_src_pa) != MI_SUCCESS ||
        MI_SYS_Mmap(gfxp_src_pa, gfxp_src_sz, &gfxp_src_va, TRUE) != MI_SUCCESS ||
        MI_SYS_MMA_Alloc(NULL, gfxp_dst_sz, &gfxp_dst_pa) != MI_SUCCESS ||
        MI_SYS_Mmap(gfxp_dst_pa, gfxp_dst_sz, &gfxp_dst_va, TRUE) != MI_SUCCESS) {
        gfxp_teardown();
        return 0;
    }

    gfxp_sw = sw; gfxp_sh = sh; gfxp_dw = dw; gfxp_dh = dh;
    gfxp_state = 1;
    return 1;
}

/* Blit gfxp_src() -> gfxp_dst(), scaling and optionally rotating 180 degrees.
   Returns 0 on failure, and on failure the destination is undefined. */
static inline int gfxp_blit(int rotate)
{
    MI_GFX_Surface_t src, dst;
    MI_GFX_Rect_t srect, drect;
    MI_GFX_Opt_t opt;
    MI_U16 fence;

    if (gfxp_state != 1)
        return 0;

    src.phyAddr = gfxp_src_pa;
    src.u32Width = gfxp_sw; src.u32Height = gfxp_sh;
    src.u32Stride = gfxp_sw * 4;
    src.eColorFmt = E_MI_GFX_FMT_ARGB8888;
    srect.s32Xpos = 0; srect.s32Ypos = 0;
    srect.u32Width = gfxp_sw; srect.u32Height = gfxp_sh;

    dst.phyAddr = gfxp_dst_pa;
    dst.u32Width = gfxp_dw; dst.u32Height = gfxp_dh;
    dst.u32Stride = gfxp_dw * 4;
    dst.eColorFmt = E_MI_GFX_FMT_ARGB8888;
    drect.s32Xpos = 0; drect.s32Ypos = 0;
    drect.u32Width = gfxp_dw; drect.u32Height = gfxp_dh;

    memset(&opt, 0, sizeof(opt));
    opt.eSrcDfbBldOp = E_MI_GFX_DFB_BLD_ONE;
    opt.eRotate = rotate ? E_MI_GFX_ROTATE_180 : E_MI_GFX_ROTATE_0;

    if (MI_SYS_FlushInvCache(gfxp_src_va, gfxp_src_sz) != MI_SUCCESS)
        return 0;
    if (MI_GFX_BitBlit(&src, &srect, &dst, &drect, &opt, &fence) != MI_SUCCESS)
        return 0;
    MI_GFX_WaitAllDone(FALSE, fence);
    return 1;
}

#endif // UTILS_GFX_PRESENT_H__
