/*
 * libfbclamp - stop unintended switches to the panel's native mode
 *
 * Miyoo Mini Flip and Miyoo Mini v4 have 752x560 panels. Onion's UI renders at
 * 640x480 and the SigmaStar GOP hardware-upscales it to fill the panel:
 *
 *     StretchWindow Src[640,480]  ->  Dst[752,560]
 *
 * On startup some processes set the framebuffer to the panel's native 752x560.
 * The GOP stretch source follows xres/yres, so the scaler then reads
 * 640-pixel-stride content as 752-pixel-wide rows: every row shifts 112px, the
 * pattern repeats every ~6.7 rows, and the screen shows fine horizontal banding
 * in the previous screen's colours until the app finally sets 640x480.
 *
 * Measured on a Mini Flip: the bad mode persisted ~2.1s per transition.
 *
 * script/fbwatch.sh mitigates this by polling and setting the mode back, but it
 * can only react after the fact - two real mode transitions still occur and
 * both are visible. Polling faster does not help (0.5ms and 2ms were
 * indistinguishable), because the cost is the mode changes themselves, not the
 * width of the window being raced.
 *
 * This intercepts the FBIOPUT_VSCREENINFO that requests the native mode and
 * rewrites it to the intended one, so the bad mode is never set and there is
 * nothing to correct.
 *
 * The rule: while the panel is not 640x480, any FBIOPUT_VSCREENINFO whose
 * geometry differs from /tmp/fb_target_res is rewritten to the target. Onion
 * owns the mode - change_resolution() writes the target *before* calling fbset,
 * so the deliberate switch always matches and passes through. Everything else
 * is a process changing the mode behind Onion's back, and gets neutralised.
 *
 * This is deliberately broader than "clamp only exact-native requests", which
 * was the first attempt. Measured on a Mini Flip: entering a game was clean
 * (Onion blanks the backlight across its own switch), but *exiting* garbled,
 * because RetroArch restores 640x480 during its shutdown ~120ms before Onion
 * regains control - unblanked, and invisible to a native-only rule since 640x480
 * is not the native mode. Holding the mode until Onion switches it deliberately
 * keeps every transition inside a blanked window. It also neutralises SDL's
 * mode enumeration, which sets each candidate mode in turn (1600x1200,
 * 1408x1056, ... 320x200) on every video init.
 *
 * 640x480 panels are left completely alone - they never scale, so there is
 * nothing to protect and no reason to interpose.
 *
 * Fails open: if anything cannot be determined, the call is passed through.
 */

#define _GNU_SOURCE

#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <linux/fb.h>

#define FB_PROC "/proc/mi_modules/fb/mi_fb0"
#define TARGET_FILE "/tmp/fb_target_res"

static int (*real_ioctl)(int, unsigned long, void *) = NULL;

/* Panel timing never changes for the life of the process, so resolve it once.
   state: 0 = not yet read, 1 = known, -1 = unavailable */
static int panel_state = 0;
static int panel_w = 0;
static int panel_h = 0;

static int panel_native(int *w, int *h)
{
    if (panel_state == 0) {
        FILE *f = fopen(FB_PROC, "r");
        panel_state = -1;
        if (f) {
            char line[256];
            while (fgets(line, sizeof(line), f)) {
                const char *p = strstr(line, "Current TimingWidth=");
                if (p && sscanf(p, "Current TimingWidth=%d,TimingWidth=%d",
                                &panel_w, &panel_h) == 2 &&
                    panel_w > 0 && panel_h > 0) {
                    panel_state = 1;
                    break;
                }
            }
            fclose(f);
        }
    }

    if (panel_state != 1)
        return 0;

    *w = panel_w;
    *h = panel_h;
    return 1;
}

/* Read fresh every time: change_resolution() rewrites this when Onion switches
   to 560p, and a long-lived process must see the update. FBIOPUT_VSCREENINFO is
   rare, so the cost is irrelevant. */
static int target_mode(int *w, int *h)
{
    FILE *f = fopen(TARGET_FILE, "r");
    int tw = 0, th = 0;
    int ok = 0;

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

/* Debug plumbing - inert unless the flag file exists. */
static int debug_enabled(void)
{
    static int state = -1;
    if (state < 0)
        state = access("/mnt/SDCARD/.fbclamp_debug", F_OK) == 0 ? 1 : 0;
    return state;
}

static const char *self_comm(void)
{
    static char comm[64];
    static int loaded = 0;
    FILE *f;

    if (loaded)
        return comm;
    loaded = 1;
    comm[0] = '?';
    comm[1] = '\0';

    f = fopen("/proc/self/comm", "r");
    if (f) {
        if (fgets(comm, sizeof(comm), f)) {
            char *nl = strchr(comm, '\n');
            if (nl)
                *nl = '\0';
        }
        fclose(f);
    }
    return comm;
}

int ioctl(int fd, unsigned long request, ...)
{
    va_list ap;
    void *arg;

    va_start(ap, request);
    arg = va_arg(ap, void *);
    va_end(ap);

    if (!real_ioctl) {
        real_ioctl = (int (*)(int, unsigned long, void *))dlsym(RTLD_NEXT, "ioctl");
        if (!real_ioctl)
            return -1;
    }

    if (request == FBIOPUT_VSCREENINFO && arg) {
        struct fb_var_screeninfo *var = (struct fb_var_screeninfo *)arg;
        int nw, nh, tw, th;
        int have_panel = panel_native(&nw, &nh);
        int have_target = target_mode(&tw, &th);

        /* touch /mnt/SDCARD/.fbclamp_debug to record every mode set that
           reaches this hook: who asked, for what, and whether it was clamped.
           Answers "is FBIOPUT_VSCREENINFO even the mechanism, and is this
           process loading the shim at all". */
        if (debug_enabled()) {
            FILE *lf = fopen("/tmp/fbclamp.log", "a");
            if (lf) {
                fprintf(lf, "pid=%d comm=%s req=%ux%u panel=%s%dx%d target=%s%dx%d\n",
                        (int)getpid(), self_comm(), var->xres, var->yres,
                        have_panel ? "" : "?", nw, nh,
                        have_target ? "" : "?", tw, th);
                fclose(lf);
            }
        }

        /* Only 752x560-class panels have this problem; a 640x480 panel never
           scales, so leave those devices completely alone. */
        if (have_panel && !(nw == 640 && nh == 480) && have_target &&
            ((int)var->xres != tw || (int)var->yres != th)) {
            /* Rewritten in place rather than on a copy so the caller's struct
               reflects what was actually set, and so the driver's write-back
               still reaches it. */
            var->xres = tw;
            var->yres = th;
            var->xres_virtual = tw;
            if ((int)var->yres_virtual < th)
                var->yres_virtual = th * 2;
            if ((int)var->yoffset > 0)
                var->yoffset = 0;
        }
    }

    return real_ioctl(fd, request, arg);
}
