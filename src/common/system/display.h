#ifndef DISPLAY_H__
#define DISPLAY_H__

#include <linux/fb.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "system.h"
#include "utils/file.h"
#include "utils/log.h"

#ifdef PLATFORM_MIYOOMINI
#define DEFAULT_WIDTH 640
#define DEFAULT_HEIGHT 480
#else
#define DEFAULT_WIDTH 752
#define DEFAULT_HEIGHT 560
#endif

#define display_on() display_setScreen(true)
#define display_off() display_setScreen(false)

// Must start negative. Three call sites below open the framebuffer lazily with
// `if (fb_fd < 0)`, and a zero-initialised fd makes every one of those guards
// fail: the open never happens and the ioctls run against fd 0 - stdin - and
// fail, leaving g_display at its compile-time DEFAULT_WIDTH/HEIGHT. That was
// harmless while the default matched the panel, and silently wrong the moment
// it did not: on a 752x560 panel every caller of display_getRenderResolution()
// was told the screen was 640x480.
static int fb_fd = -1;
static int DISPLAY_WIDTH = DEFAULT_WIDTH; // physical screen resolution
static int DISPLAY_HEIGHT = DEFAULT_HEIGHT;
struct timeval start_time, end_time;

typedef struct Display {
    long fb_size;
    int width;
    int height;
    int bpp;
    int stride;
    uint32_t *fb_addr;
    uint8_t *fb_ofs;
    struct fb_fix_screeninfo finfo;
    struct fb_var_screeninfo vinfo;
    uint8_t *savebuf;
    bool enabled;
    bool init_done;
} display_t;

static display_t g_display = {
    .width = DEFAULT_WIDTH,
    .height = DEFAULT_HEIGHT,
    .bpp = 32,
    .stride = 0,
    .fb_size = 0,
    .fb_addr = NULL,
    .fb_ofs = NULL,
    .enabled = true,
    .init_done = false,
};

typedef struct Rect {
    int x;
    int y;
    int w;
    int h;
} rect_t;

void display_clear(void)
{
    if (g_display.fb_addr) {
        memset(g_display.fb_addr, 0, g_display.fb_size);
    }
}

void display_reset(void)
{
    if (fb_fd < 0)
        fb_fd = open("/dev/fb0", O_RDWR);
    ioctl(fb_fd, FBIOGET_VSCREENINFO, &g_display.vinfo);
    g_display.vinfo.yoffset = 0;
    ioctl(fb_fd, FBIOPUT_VSCREENINFO, &g_display.vinfo);
}

//
//    Get render resolution
//
void display_getRenderResolution()
{
    if (fb_fd < 0)
        fb_fd = open("/dev/fb0", O_RDWR);
    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &g_display.vinfo) == 0) {
        g_display.width = g_display.vinfo.xres;
        g_display.height = g_display.vinfo.yres;
    }
    printf_debug("Render resolution: %dx%d\n", g_display.width, g_display.height);
}

//
//    Get physical screen resolution
//
void display_getResolution(void)
{
    FILE *file = fopen("/tmp/screen_resolution", "r");
    if (file == NULL) {
        printf("Failed to open screen resolution file\n");
        return;
    }
    if (fscanf(file, "%dx%d", &DISPLAY_WIDTH, &DISPLAY_HEIGHT) != 2)
        printf("Failed to get screen resolution\n");
    fclose(file);
}

//
//    Cache the framebuffer's authoritative geometry.
//
//    finfo.line_length is the real byte stride of a row: the driver may pad
//    rows, so it is not necessarily xres * bpp. Anything that walks the
//    framebuffer must use it rather than deriving a stride from xres.
//
void display_updateGeometry(void)
{
    g_display.bpp = g_display.vinfo.bits_per_pixel / 8; // bytes per pixel
    g_display.stride = (int)g_display.finfo.line_length;

    if (g_display.stride <= 0)
        g_display.stride = g_display.vinfo.xres_virtual * g_display.bpp;
}

//
//    Re-read the screeninfo from the driver.
//
//    yoffset, yres_virtual and line_length all change when the mode changes,
//    so anything that depends on them has to refresh rather than trust what
//    was cached at init.
//
void display_refreshVinfo(void)
{
    if (fb_fd < 0)
        fb_fd = open("/dev/fb0", O_RDWR);
    ioctl(fb_fd, FBIOGET_VSCREENINFO, &g_display.vinfo);
    ioctl(fb_fd, FBIOGET_FSCREENINFO, &g_display.finfo);
    display_updateGeometry();
}

void display_init(bool map_fb)
{
    if (g_display.init_done)
        return;

    // Open and mmap FB
    fb_fd = open("/dev/fb0", O_RDWR);
    ioctl(fb_fd, FBIOGET_FSCREENINFO, &g_display.finfo);

    display_reset();

    if (map_fb) {
        g_display.fb_size = g_display.finfo.smem_len;
        g_display.fb_addr = (uint32_t *)mmap(0, g_display.fb_size, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    }
    else {
        g_display.fb_addr = NULL;
    }

    display_getResolution();
    display_getRenderResolution();
    display_updateGeometry();

    g_display.init_done = true;
}

//
//    Save/Clear Display area
//
void display_save(void)
{
    display_refreshVinfo(); // refreshes bpp and stride from the driver too
    g_display.fb_ofs = (uint8_t *)g_display.fb_addr + (g_display.vinfo.yoffset * g_display.stride);

    // Save display area and clear
    if ((g_display.savebuf = (uint8_t *)malloc(g_display.width * g_display.bpp * g_display.height))) {
        uint32_t i, ofss, ofsd;
        ofss = ofsd = 0;
        for (i = g_display.height; i > 0;
             i--, ofss += g_display.stride, ofsd += g_display.width * g_display.bpp) {
            memcpy(g_display.savebuf + ofsd, g_display.fb_ofs + ofss, g_display.width * g_display.bpp);
            memset(g_display.fb_ofs + ofss, 0, g_display.width * g_display.bpp);
        }
    }
}

//
//    Restore Display area
//
void display_restore(void)
{
    // Restore display area
    if (g_display.savebuf) {
        uint32_t i, ofss, ofsd;
        ofss = ofsd = 0;
        for (i = g_display.height; i > 0;
             i--, ofsd += g_display.stride, ofss += g_display.width * g_display.bpp) {
            memcpy(g_display.fb_ofs + ofsd, g_display.savebuf + ofss, g_display.width * g_display.bpp);
        }
        free(g_display.savebuf);
        g_display.savebuf = NULL;
    }
}

void display_free(display_t *display)
{
    if (display->savebuf) {
        free(display->savebuf);
        display->savebuf = NULL;
    }
    if (display->fb_addr) {
        munmap(display->fb_addr, display->fb_size);
        display->fb_addr = NULL;
    }
}

//
//    Screen On/Off
//
void display_setScreen(bool enabled)
{
    // export gpio4, direction: out
    file_write(GPIO_DIR1 "export", "4", 1);
    file_write(GPIO_DIR2 "gpio4/direction", "out", 3);

    // screen on/off
    file_write(GPIO_DIR2 "gpio4/value", enabled ? "1" : "0", 1);

    // unexport gpio4
    file_write(GPIO_DIR1 "unexport", "4", 1);

    if (enabled) {
        // re-enable brightness control
        file_write(PWM_DIR "export", "0", 1);
        file_write(PWM_DIR "pwm0/enable", "0", 1);
        file_write(PWM_DIR "pwm0/enable", "1", 1);
        display_restore();
    }
    else {
        display_save();
    }
    g_display.enabled = enabled;
}

void display_toggle(void) { display_setScreen(!g_display.enabled); }

uint32_t display_getBrightnessRaw()
{
    uint32_t duty_cycle = 0;
    FILE *fp;

    if (exists(PWM_DIR "pwm0/duty_cycle")) {
        file_get(fp, PWM_DIR "pwm0/duty_cycle", "%u", &duty_cycle);
    }
    return duty_cycle;
}

// Get display brightness from raw (0 - 10)
int display_getBrightnessFromRaw()
{
    int value_raw = display_getBrightnessRaw();
    int value = round((log(value_raw / 3.0) / 0.350656));
    return value;
}
//
//    Set Brightness (Raw)
//
void display_setBrightnessRaw(uint32_t value)
{
    FILE *fp;
    file_put_sync(fp, PWM_DIR "pwm0/duty_cycle", "%u", value);
    printf_debug("Raw brightness: %d\n", value);
}

// Set display brightness (0 - 10)
void display_setBrightness(uint32_t value)
{
    // Linear curve
    // int value_raw = (value == 0) ? 3 : (value * 10);

    // Exponential curve
    int value_raw = round(3.0 * exp(0.350656 * value));

    display_setBrightnessRaw(value_raw);
}

/**
 * @brief Read from or write to a framebuffer buffer.
 *
 * This function reads from or writes to a specific buffer in the framebuffer.
 * It supports optional rotation of the buffer content.
 *
 * @param index The index of the buffer to read/write.
 * @param display The display structure.
 * @param pixels Pointer to the pixel data.
 * @param rect The rectangle area to read/write.
 * @param rotate Whether to rotate the buffer content.
 * @param write Whether to write to the buffer (true) or read from it (false).
 * @param mask Whether to use a mask when writing to the buffer.
 */
void display_readOrWriteBuffer(int index, display_t *display, uint32_t *pixels, rect_t rect, bool rotate, bool mask, bool write)
{
    int bufferPos = index * display->vinfo.yres;

    if (display->fb_addr == NULL)
        return;

    // Address rows by the driver's real byte stride. finfo.line_length is not
    // necessarily xres * bpp - the driver is free to pad rows - and deriving it
    // from xres shears the image wherever that padding exists. Callers may hand
    // us a display_t they populated themselves (see osd.h), so fall back
    // through the screeninfo rather than trusting the cached field.
    long stride = display->stride > 0
                      ? (long)display->stride
                      : (display->finfo.line_length > 0
                             ? (long)display->finfo.line_length
                             : (long)display->vinfo.xres *
                                   (display->vinfo.bits_per_pixel / 8));

    for (int oy = 0; oy < rect.h; oy++) {
        int y = rect.y + oy;

        if (y < 0 || y >= (int)display->vinfo.yres)
            continue;

        int virtualY = bufferPos + (rotate ? (display->vinfo.yres - 1) - y : y);
        uint32_t *row = (uint32_t *)((uint8_t *)display->fb_addr + (long)virtualY * stride);
        int baseIndex = oy * rect.w;

        for (int ox = 0; ox < rect.w; ox++) {
            int x = rect.x + ox;

            if (rotate) {
                x = (display->vinfo.xres - 1) - x;
            }

            if (x < 0 || x >= (int)display->vinfo.xres)
                continue;

            int index = baseIndex + ox;
            if (write) {
                if (mask) {
                    if (pixels[index] != 0) {
                        row[x] = 0;
                    }
                }
                else {
                    row[x] = pixels[index];
                }
            }
            else {
                if (mask) {
                    pixels[index] = row[x] == 0 ? 1 : 0;
                }
                else {
                    pixels[index] = row[x];
                }
            }
        }
    }
}

/**
 * @brief Read from the current framebuffer buffer.
 * 
 * This function reads from the current buffer in the framebuffer.
 * It supports optional rotation of the buffer content.
 * 
 * @param display The display structure.
 * @param pixels Pointer to the pixel data.
 * @param rect The rectangle area to read/write.
 * @param rotate Whether to rotate the buffer content.
 * @param mask Whether to use a mask when writing to the buffer.
 */
void display_readCurrentBuffer(display_t *display, uint32_t *pixels, rect_t rect, bool rotate, bool mask)
{
    int index = display->vinfo.yoffset / display->vinfo.yres;
    display_readOrWriteBuffer(index, display, pixels, rect, rotate, mask, false);
}

/** 
 * @brief Read from a framebuffer buffer.
 *
 * This function reads from a specific buffer in the framebuffer.
 * It supports optional rotation of the buffer content.
 *
 * @param index The index of the buffer to read.
 * @param display The display structure.
 * @param pixels Pointer to the pixel data.
 * @param rect The rectangle area to read/write.
 * @param rotate Whether to rotate the buffer content.
 * @param mask Whether to use a mask when writing to the buffer.
 */
void display_readBuffer(int index, display_t *display, uint32_t *pixels, rect_t rect, bool rotate, bool mask)
{
    display_readOrWriteBuffer(index, display, pixels, rect, rotate, mask, false);
}

/** 
 * @brief Write to a framebuffer buffer.
 *
 * This function writes to a specific buffer in the framebuffer.
 * It supports optional rotation of the buffer content.
 *
 * @param index The index of the buffer to write.
 * @param display The display structure.
 * @param pixels Pointer to the pixel data.
 * @param rect The rectangle area to read/write.
 * @param rotate Whether to rotate the buffer content.
 * @param mask Whether to use a mask when writing to the buffer.
 */
void display_writeBuffer(int index, display_t *display, uint32_t *pixels, rect_t rect, bool rotate, bool mask)
{
    display_readOrWriteBuffer(index, display, pixels, rect, rotate, mask, true);
}

/**
 * @brief Read from or write to multiple framebuffer buffers.
 *
 * This function reads from or writes to multiple buffers in the framebuffer.
 * It supports optional rotation of the buffer content.
 *
 * @param display The display structure.
 * @param pixels Array of pointers to the pixel data for each buffer.
 * @param rect The rectangle area to read/write.
 * @param rotate Whether to rotate the buffer content.
 * @param write Whether to write to the buffers (true) or read from them (false).
 * @param mask Whether to use a mask when writing to the buffers.
 */
void display_readOrWriteBuffers(display_t *display, uint32_t **pixels, rect_t rect, bool rotate, bool mask, bool write)
{
    int numBuffers = display->vinfo.yres_virtual / display->vinfo.yres;

    for (int b = 0; b < numBuffers; b++) {
        display_readOrWriteBuffer(b, display, pixels[b], rect, rotate, mask, write);
    }
}

/** 
 * @brief Read from multiple framebuffer buffers.
 *
 * This function reads from multiple buffers in the framebuffer.
 * It supports optional rotation of the buffer content.
 *
 * @param display The display structure.
 * @param pixels Array of pointers to the pixel data for each buffer.
 * @param rect The rectangle area to read/write.
 * @param rotate Whether to rotate the buffer content.
 * @param mask Whether to use a mask when writing to the buffers.
 */
void display_readBuffers(display_t *display, uint32_t **pixels, rect_t rect, bool rotate, bool mask)
{
    display_readOrWriteBuffers(display, pixels, rect, rotate, mask, false);
}

/** 
 * @brief Write to multiple framebuffer buffers.
 *
 * This function writes to multiple buffers in the framebuffer.
 * It supports optional rotation of the buffer content.
 *
 * @param display The display structure.
 * @param pixels Array of pointers to the pixel data for each buffer.
 * @param rect The rectangle area to read/write.
 * @param rotate Whether to rotate the buffer content.
 * @param mask Whether to use a mask when writing to the buffers.
 */
void display_writeBuffers(display_t *display, uint32_t **pixels, rect_t rect, bool rotate, bool mask)
{
    display_readOrWriteBuffers(display, pixels, rect, rotate, mask, true);
}

//
//    Draw a one-pixel border around every framebuffer buffer.
//
//    Was hardcoded to 640x480x32bpp with a fixed three buffers, which writes
//    past the mapping on any other geometry. Derived from the driver now.
//
void display_drawFrame(uint32_t color)
{
    uint32_t *fb = g_display.fb_addr;
    int stride_px, buffers, w, h, b, x, y;

    if (!fb || g_display.vinfo.yres == 0)
        return;

    w = (int)g_display.vinfo.xres;
    h = (int)g_display.vinfo.yres;
    stride_px = g_display.stride > 0 ? g_display.stride / (int)sizeof(uint32_t) : w;
    buffers = g_display.vinfo.yres_virtual / g_display.vinfo.yres;
    if (buffers < 1)
        buffers = 1;

    for (b = 0; b < buffers; b++) {
        uint32_t *buf = fb + (long)b * h * stride_px;

        for (x = 0; x < w; x++) {
            buf[x] = color;                              // top
            buf[(long)(h - 1) * stride_px + x] = color;  // bottom
        }
        for (y = 0; y < h; y++) {
            buf[(long)y * stride_px] = color;            // left
            buf[(long)y * stride_px + w - 1] = color;    // right
        }
    }
}

//
//    Draw a battery icon
//
void display_drawBatteryIcon(uint32_t color, int x, int y, int level,
                             uint32_t fillColor)
{
    uint32_t *ofs = g_display.fb_addr;
    int i, j;

    // Draw battery body wireframe
    for (i = x; i < x + 30; i++) {
        ofs[i + y * g_display.width] = color;        // Top border
        ofs[i + (y + 14) * g_display.width] = color; // Bottom border
    }
    for (j = y; j < y + 15; j++) {
        ofs[x + j * g_display.width] = color;      // Left border
        ofs[x + 29 + j * g_display.width] = color; // Right border
    }

    // Draw battery charge level
    int levelWidth = (level * 26) / 100;
    for (i = x + 3 + 26 - levelWidth; i < x + 1 + 26; i++) {
        for (j = y + 3; j < y + 12; j++) {
            ofs[i + j * g_display.width] = fillColor;
        }
    }

    // Draw battery head wireframe
    for (i = x - 4; i < x; i++) {
        for (j = y + 2; j < y + 13; j++) {
            ofs[i + j * g_display.width] = color;
        }
    }
}

void display_close(void)
{
    display_reset();
    display_free(&g_display);

    if (fb_fd > 0)
        close(fb_fd);
}

#endif // DISPLAY_H__
