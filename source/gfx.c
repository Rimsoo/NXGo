#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <switch.h>

#include "gfx.h"
#include "third_party/stb/stb_image.h"
#include "third_party/stb/stb_truetype.h"

static Framebuffer s_fb;
static uint32_t *s_px;
static int s_pitch;  // en pixels
static int s_cx0, s_cy0, s_cx1, s_cy1;  // zone de dessin autorisée

static stbtt_fontinfo s_font;
static bool s_font_ok;

// ------------------------------------------------------------------ init

bool gfx_init(void)
{
    if (R_FAILED(framebufferCreate(&s_fb, nwindowGetDefault(), SCREEN_W, SCREEN_H, PIXEL_FORMAT_RGBA_8888, 2)))
        return false;
    framebufferMakeLinear(&s_fb);

    // Police système de la Switch (latin, accents compris)
    PlFontData font;
    if (R_SUCCEEDED(plInitialize(PlServiceType_User)) &&
        R_SUCCEEDED(plGetSharedFontByType(&font, PlSharedFontType_Standard))) {
        const unsigned char *data = font.address;
        s_font_ok = stbtt_InitFont(&s_font, data, stbtt_GetFontOffsetForIndex(data, 0));
    }
    gfx_reset_clip();
    return true;
}

void gfx_exit(void)
{
    plExit();
    framebufferClose(&s_fb);
}

void gfx_begin(void)
{
    u32 stride;
    s_px = framebufferBegin(&s_fb, &stride);
    s_pitch = stride / 4;
}

void gfx_end(void)
{
    framebufferEnd(&s_fb);
    s_px = NULL;
}

void gfx_set_clip(int x, int y, int w, int h)
{
    s_cx0 = x < 0 ? 0 : x;
    s_cy0 = y < 0 ? 0 : y;
    s_cx1 = x + w > SCREEN_W ? SCREEN_W : x + w;
    s_cy1 = y + h > SCREEN_H ? SCREEN_H : y + h;
}

void gfx_reset_clip(void)
{
    gfx_set_clip(0, 0, SCREEN_W, SCREEN_H);
}

// ------------------------------------------------------------------ pixels

static inline void blend(int x, int y, GfxColor c, int coverage)
{
    if (x < s_cx0 || y < s_cy0 || x >= s_cx1 || y >= s_cy1)
        return;
    uint32_t a = ((c >> 24) * coverage) / 255;
    if (a == 0)
        return;
    uint32_t *d = &s_px[y * s_pitch + x];
    if (a == 255) {
        *d = c;
        return;
    }
    uint32_t dst = *d, inv = 255 - a;
    uint32_t r = ((c & 0xFF) * a + (dst & 0xFF) * inv) / 255;
    uint32_t g = (((c >> 8) & 0xFF) * a + ((dst >> 8) & 0xFF) * inv) / 255;
    uint32_t b = (((c >> 16) & 0xFF) * a + ((dst >> 16) & 0xFF) * inv) / 255;
    *d = r | g << 8 | b << 16 | 0xFFu << 24;
}

void gfx_clear(GfxColor c)
{
    for (int x = 0; x < SCREEN_W; x++)
        s_px[x] = c;
    for (int y = 1; y < SCREEN_H; y++)
        memcpy(&s_px[y * s_pitch], s_px, SCREEN_W * 4);
}

void gfx_rect(int x, int y, int w, int h, GfxColor c)
{
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++)
            blend(i, j, c, 255);
}

void gfx_round_rect(int x, int y, int w, int h, int r, GfxColor c)
{
    if (r * 2 > w)
        r = w / 2;
    if (r * 2 > h)
        r = h / 2;
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            // Distance au centre du coin le plus proche (seulement dans les coins)
            int cx = i < r ? r : (i >= w - r ? w - r - 1 : i);
            int cy = j < r ? r : (j >= h - r ? h - r - 1 : j);
            int coverage = 255;
            if (cx != i || cy != j) {
                float dx = i - cx, dy = j - cy;
                float d = sqrtf(dx * dx + dy * dy);
                float cov = r - d + 0.5f;  // bord adouci sur un pixel
                if (cov <= 0)
                    continue;
                coverage = cov >= 1 ? 255 : (int)(cov * 255);
            }
            blend(x + i, y + j, c, coverage);
        }
    }
}

void gfx_circle(int cx, int cy, int radius, GfxColor c)
{
    gfx_round_rect(cx - radius, cy - radius, radius * 2, radius * 2, radius, c);
}

void gfx_disc(float cx, float cy, float radius, GfxColor c)
{
    int x0 = (int)floorf(cx - radius - 1), x1 = (int)ceilf(cx + radius + 1);
    int y0 = (int)floorf(cy - radius - 1), y1 = (int)ceilf(cy + radius + 1);
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            float cov = radius - sqrtf(dx * dx + dy * dy) + 0.5f;
            if (cov <= 0)
                continue;
            blend(x, y, c, cov >= 1 ? 255 : (int)(cov * 255));
        }
}

void gfx_ring(float cx, float cy, float radius, float thickness, GfxColor c)
{
    float inner = radius - thickness;
    int x0 = (int)floorf(cx - radius - 1), x1 = (int)ceilf(cx + radius + 1);
    int y0 = (int)floorf(cy - radius - 1), y1 = (int)ceilf(cy + radius + 1);
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            float d = sqrtf(dx * dx + dy * dy);
            float out = radius - d + 0.5f, in = d - inner + 0.5f;
            float cov = out < in ? out : in;
            if (cov <= 0)
                continue;
            blend(x, y, c, cov >= 1 ? 255 : (int)(cov * 255));
        }
}

static float edge(int ax, int ay, int bx, int by, int px, int py)
{
    return (float)(bx - ax) * (py - ay) - (float)(by - ay) * (px - ax);
}

void gfx_triangle(int x0, int y0, int x1, int y1, int x2, int y2, GfxColor c)
{
    int minx = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2);
    int maxx = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
    int miny = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2);
    int maxy = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);
    float area = edge(x0, y0, x1, y1, x2, y2);
    if (area == 0)
        return;
    for (int y = miny; y <= maxy; y++)
        for (int x = minx; x <= maxx; x++) {
            float w0 = edge(x1, y1, x2, y2, x, y), w1 = edge(x2, y2, x0, y0, x, y), w2 = edge(x0, y0, x1, y1, x, y);
            if (area > 0 ? (w0 >= 0 && w1 >= 0 && w2 >= 0) : (w0 <= 0 && w1 <= 0 && w2 <= 0))
                blend(x, y, c, 255);
        }
}

// ------------------------------------------------------------------ texte

typedef struct {
    uint32_t cp;
    int size;
    int w, h, xoff, yoff, advance;
    uint8_t *bitmap;
} Glyph;

#define GLYPH_CACHE 2048
static Glyph s_glyphs[GLYPH_CACHE];

static const Glyph *get_glyph(uint32_t cp, int size)
{
    unsigned slot = (cp * 31u + size * 7919u) % GLYPH_CACHE;
    for (int probe = 0; probe < GLYPH_CACHE; probe++) {
        Glyph *g = &s_glyphs[(slot + probe) % GLYPH_CACHE];
        if (g->size == size && g->cp == cp)
            return g;
        if (g->size == 0) {
            float scale = stbtt_ScaleForPixelHeight(&s_font, size);
            int advance, lsb;
            stbtt_GetCodepointHMetrics(&s_font, cp, &advance, &lsb);
            g->cp = cp;
            g->size = size;
            g->advance = (int)lroundf(advance * scale);
            g->bitmap = stbtt_GetCodepointBitmap(&s_font, scale, scale, cp, &g->w, &g->h, &g->xoff, &g->yoff);
            return g;
        }
    }
    return NULL;  // cache plein : le caractère n'est pas dessiné
}

static uint32_t next_codepoint(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    uint32_t cp;
    int extra;
    if (p[0] < 0x80) {
        cp = p[0];
        extra = 0;
    } else if ((p[0] & 0xE0) == 0xC0) {
        cp = p[0] & 0x1F;
        extra = 1;
    } else if ((p[0] & 0xF0) == 0xE0) {
        cp = p[0] & 0x0F;
        extra = 2;
    } else {
        cp = p[0] & 0x07;
        extra = 3;
    }
    p++;
    for (int i = 0; i < extra && (*p & 0xC0) == 0x80; i++, p++)
        cp = cp << 6 | (*p & 0x3F);
    *s = (const char *)p;
    return cp;
}

static int draw_text(int x, int y, int size, GfxColor c, const char *text, bool draw)
{
    if (!s_font_ok)
        return 0;
    float scale = stbtt_ScaleForPixelHeight(&s_font, size);
    int ascent;
    stbtt_GetFontVMetrics(&s_font, &ascent, NULL, NULL);
    int baseline = y + (int)lroundf(ascent * scale);

    int pen = x;
    while (*text) {
        uint32_t cp = next_codepoint(&text);
        const Glyph *g = get_glyph(cp, size);
        if (!g)
            continue;
        if (draw && g->bitmap)
            for (int j = 0; j < g->h; j++)
                for (int i = 0; i < g->w; i++)
                    blend(pen + g->xoff + i, baseline + g->yoff + j, c, g->bitmap[j * g->w + i]);
        pen += g->advance;
    }
    return pen - x;
}

int gfx_text(int x, int y, int size, GfxColor c, const char *text)
{
    return draw_text(x, y, size, c, text, true);
}

int gfx_text_width(int size, const char *text)
{
    return draw_text(0, 0, size, 0, text, false);
}

int gfx_text_fit(int x, int y, int size, GfxColor c, const char *text, int max_w)
{
    if (gfx_text_width(size, text) <= max_w)
        return gfx_text(x, y, size, c, text);
    char buf[512];
    size_t n = strlen(text);
    if (n >= sizeof(buf) - 4)
        n = sizeof(buf) - 4;
    memcpy(buf, text, n);
    while (n > 0) {
        n--;
        while (n > 0 && ((unsigned char)buf[n] & 0xC0) == 0x80)  // pas au milieu d'un caractère
            n--;
        memcpy(buf + n, "…", sizeof("…"));
        if (gfx_text_width(size, buf) <= max_w)
            break;
    }
    return gfx_text(x, y, size, c, buf);
}

// ------------------------------------------------------------------ images

bool gfx_image_load(const void *data, size_t len, int w, int h, GfxImage *out)
{
    int sw, sh, channels;
    uint8_t *src = stbi_load_from_memory(data, len, &sw, &sh, &channels, 4);
    if (!src)
        return false;
    out->w = w;
    out->h = h;
    out->px = malloc((size_t)w * h * 4);
    if (!out->px) {
        stbi_image_free(src);
        return false;
    }
    // Réduction par moyenne de zone : net même à petite taille
    for (int y = 0; y < h; y++) {
        int y0 = y * sh / h, y1 = (y + 1) * sh / h;
        if (y1 <= y0)
            y1 = y0 + 1;
        for (int x = 0; x < w; x++) {
            int x0 = x * sw / w, x1 = (x + 1) * sw / w;
            if (x1 <= x0)
                x1 = x0 + 1;
            uint32_t acc[4] = {0}, n = 0;
            for (int yy = y0; yy < y1; yy++)
                for (int xx = x0; xx < x1; xx++, n++)
                    for (int k = 0; k < 4; k++)
                        acc[k] += src[(yy * sw + xx) * 4 + k];
            out->px[y * w + x] = RGBA(acc[0] / n, acc[1] / n, acc[2] / n, acc[3] / n);
        }
    }
    stbi_image_free(src);
    return true;
}

bool gfx_image_alloc(GfxImage *out, int w, int h)
{
    out->w = w;
    out->h = h;
    out->px = calloc((size_t)w * h, 4);
    return out->px != NULL;
}

void gfx_image_free(GfxImage *img)
{
    free(img->px);
    img->px = NULL;
}

void gfx_image(const GfxImage *img, int x, int y)
{
    if (!img->px)
        return;
    for (int j = 0; j < img->h; j++)
        for (int i = 0; i < img->w; i++) {
            GfxColor c = img->px[j * img->w + i];
            blend(x + i, y + j, c, 255);
        }
}

void gfx_image_alpha(const GfxImage *img, int x, int y, int alpha)
{
    if (!img->px)
        return;
    for (int j = 0; j < img->h; j++)
        for (int i = 0; i < img->w; i++)
            blend(x + i, y + j, img->px[j * img->w + i], alpha);
}

void gfx_image_opaque(const GfxImage *img, int x, int y)
{
    if (!img->px)
        return;
    int x0 = x < s_cx0 ? s_cx0 : x, x1 = x + img->w > s_cx1 ? s_cx1 : x + img->w;
    if (x1 <= x0)
        return;
    for (int j = 0; j < img->h; j++) {
        int yy = y + j;
        if (yy < s_cy0 || yy >= s_cy1)
            continue;
        memcpy(&s_px[yy * s_pitch + x0], &img->px[j * img->w + (x0 - x)], (size_t)(x1 - x0) * 4);
    }
}
