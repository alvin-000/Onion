/*
 * libfbpin - hold the framebuffer in the mode Onion chose for the session
 *
 * Miyoo Mini Flip and Miyoo Mini v4 have 752x560 panels. Onion pins the
 * framebuffer to the panel's native mode at boot (runtime.sh
 * pin_ui_resolution) and everything - the UI, apps, games - renders in that one
 * mode for the whole session.
 *
 * That is the entire point. Changing the framebuffer mode reconfigures the
 * SigmaStar GOP scaler, and the reconfiguration is visibly garbled: measured on
 * a Mini Flip, switching modes with an all-zero aperture and nothing drawing
 * still garbles, so it cannot be prevented by managing framebuffer contents.
 * The only reliable fix is to never change the mode.
 *
 * Processes change it anyway. SDL 1.2's fbcon backend builds its mode list by
 * setting each candidate mode in turn (1600x1200, 1408x1056, ... 320x200) on
 * every video init, and then sets the mode the app asked for. Onion's own apps
 * ask for the mode that is already set, so only the probing needs to be
 * absorbed - but absorbed it must be, or every app launch flickers.
 *
 * The rule: any FBIOPUT_VSCREENINFO whose geometry differs from
 * /tmp/fb_target_res is rewritten to the target. Onion owns the mode -
 * change_resolution() writes the target *before* calling fbset, so a deliberate
 * switch always matches and passes through. Everything else is a process
 * changing the mode behind Onion's back, and gets neutralised.
 *
 * Rewriting rather than failing the ioctl is deliberate. SDL's FB_CheckMode
 * compares the geometry it reads back against what it asked for and drops the
 * mode from its list when they differ, which is exactly the desired outcome for
 * a probe. A process that genuinely needs a different mode must not be run
 * under this shim - MainUI is the one such case, and it gets libuiscale
 * instead.
 *
 * 640x480 panels are left completely alone: they never scale, so there is
 * nothing to protect and no reason to interpose.
 *
 * Fails open: if anything cannot be determined, the call is passed through.
 */

#define _GNU_SOURCE

#include <dlfcn.h>
#include <linux/fb.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>

#define FB_PROC "/proc/mi_modules/fb/mi_fb0"
#define TARGET_FILE "/tmp/fb_target_res"

static int (*real_ioctl)(int, unsigned long, void *) = NULL;

/* Panel timing never changes for the life of the process, so resolve it once.
   state: 0 = not yet read, 1 = known, -1 = unavailable */
static int panel_state = 0;
static int panel_w = 0;
static int panel_h = 0;

/* Two firmware families describe the panel differently, and this must agree
   with read_panel_mode() in runtime.sh or the shell and the interposer will
   disagree about which devices scale.

     Flip     "Current TimingWidth=752,TimingWidth=560,..." - the panel timing,
              which is not the framebuffer's current mode (640x480 until Onion
              pins it). Always preferred where present.

     Mini v4  no timing line at all; "xres=752, yres=560" is both the current
              mode and the panel's native one.

   Taking xres/yres first would be wrong on a Flip - it would read 640x480 and
   conclude the panel does not scale. */
static int panel_native(int *w, int *h)
{
    if (panel_state == 0) {
        FILE *f = fopen(FB_PROC, "r");
        int fb_w = 0, fb_h = 0;
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
                /* "xres_virtual=" does not contain "xres=", so this cannot
                   pick up the virtual geometry by mistake. Remembered rather
                   than used immediately, in case a timing line follows. */
                if (fb_w == 0) {
                    p = strstr(line, "xres=");
                    if (p && sscanf(p, "xres=%d, yres=%d", &fb_w, &fb_h) != 2)
                        fb_w = 0;
                }
            }
            fclose(f);
        }

        if (panel_state != 1 && fb_w > 0 && fb_h > 0) {
            panel_w = fb_w;
            panel_h = fb_h;
            panel_state = 1;
        }
    }

    if (panel_state != 1)
        return 0;

    *w = panel_w;
    *h = panel_h;
    return 1;
}

/* Read fresh every time: change_resolution() rewrites this when Onion switches
   the mode deliberately, and a long-lived process must see the update.
   FBIOPUT_VSCREENINFO is rare, so the cost is irrelevant. */
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
        state = access("/mnt/SDCARD/.fbpin_debug", F_OK) == 0 ? 1 : 0;
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
        /* Zeroed because the debug log below prints them whether or not the
           probes succeeded - it marks an unknown value with "?" rather than
           omitting it, so it must still have something defined to read. */
        int nw = 0, nh = 0, tw = 0, th = 0;
        int have_panel = panel_native(&nw, &nh);
        int have_target = target_mode(&tw, &th);

        /* touch /mnt/SDCARD/.fbpin_debug to record every mode set that reaches
           this hook: who asked, for what, and whether it was rewritten. This is
           the log to check when verifying that no real mode change happens
           after boot. */
        if (debug_enabled()) {
            FILE *lf = fopen("/tmp/fbpin.log", "a");
            if (lf) {
                fprintf(lf, "pid=%d comm=%s req=%ux%u panel=%s%dx%d target=%s%dx%d%s\n",
                        (int)getpid(), self_comm(), var->xres, var->yres,
                        have_panel ? "" : "?", nw, nh,
                        have_target ? "" : "?", tw, th,
                        (have_panel && !(nw == 640 && nh == 480) && have_target &&
                         ((int)var->xres != tw || (int)var->yres != th))
                            ? " PINNED"
                            : "");
                fclose(lf);
            }
        }

        /* Only panels that scale have this problem; a 640x480 panel never
           does, so leave those devices completely alone. */
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
