/* display.overscan: the crop each GPU presenter samples (nv2a_present_crop)
 * and the destination nv2a_present_rect gives it; present.shader's rect and
 * source size (nv2a_present_chain_rect, nv2a_present_chain_source). The whole frame at 0 is
 * compared exactly, since the presenters run it at stock. */
#include <math.h>
#include <stdio.h>
#include "nv2a_backend_common.h"

#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static int near(float a, float b)
{
    return fabsf(a - b) <= 1e-5f * (fabsf(b) > 1.0f ? fabsf(b) : 1.0f);
}

static int crop_is(const struct nv2a_crop *c, float x, float y, float w, float h,
                   uint32_t tw, uint32_t th, uint32_t fit_w, uint32_t fit_h)
{
    return near(c->x, x) && near(c->y, y) && near(c->w, w) && near(c->h, h)
        && near(c->u_off, x / tw) && near(c->v_off, y / th)
        && near(c->u_scale, w / tw) && near(c->v_scale, h / th)
        && c->fit_w == fit_w && c->fit_h == fit_h;
}

static int rect_is(const struct nv2a_rect *r, int x, int y, int w, int h)
{
    return r->x == x && r->y == y && r->w == w && r->h == h;
}

/* The presenter's quad corner (u, v) in 0..1 lands on texel u * scale + off
 * in the texture: the crop's edges. */
static int edges_at(const struct nv2a_crop *c, uint32_t tw, uint32_t th,
                    float x0, float y0, float x1, float y1)
{
    return near(c->u_off * tw, x0) && near(c->v_off * th, y0)
        && near((c->u_off + c->u_scale) * tw, x1) && near((c->v_off + c->v_scale) * th, y1);
}

int main(void)
{
    struct nv2a_crop c;
    struct nv2a_rect r;
    int f, whole;
    static const unsigned scales[] = {1, 2, 4};
    unsigned i;

    /* 0: the whole frame, the UV scale present_blit used before the key
     * (frame over texture), no offset, the whole size to fit. Exact. */
    nv2a_present_crop(640, 480, 640, 480, 1, 0, 0, NV2A_PRESENT_NEAREST, &c);
    CHECK(c.x == 0.0f && c.y == 0.0f && c.w == 640.0f && c.h == 480.0f);
    CHECK(c.u_scale == 1.0f && c.v_scale == 1.0f && c.u_off == 0.0f && c.v_off == 0.0f);
    CHECK(c.fit_w == 640 && c.fit_h == 480);
    nv2a_present_crop(1280, 960, 2048, 1024, 2, 0, 0, NV2A_PRESENT_LINEAR, &c);
    CHECK(c.u_scale == 1280.0f / 2048.0f && c.v_scale == 960.0f / 1024.0f);
    CHECK(c.u_off == 0.0f && c.v_off == 0.0f && c.fit_w == 1280 && c.fit_h == 960);

    /* SSX 3's title border: 9 guest px a side of 640x480, the rows in
     * proportion (6.75), the whole frame to fit for nearest and linear. */
    for (f = NV2A_PRESENT_NEAREST; f <= NV2A_PRESENT_LINEAR; f++) {
        for (i = 0; i < sizeof scales / sizeof scales[0]; i++) {
            unsigned n = scales[i];
            uint32_t fw = 640 * n, fh = 480 * n;
            nv2a_present_crop(fw, fh, fw, fh, n, 0, 9, f, &c);
            CHECK(crop_is(&c, 9.0f * n, 6.75f * n, 622.0f * n, 466.5f * n, fw, fh, fw, fh));
            /* UV is the same at every scale */
            CHECK(near(c.u_off, 9.0f / 640) && near(c.v_off, 6.75f / 480));
            CHECK(edges_at(&c, fw, fh, 9.0f * n, 6.75f * n, 631.0f * n, 473.25f * n));
            /* the crop keeps 4:3 */
            CHECK(near(c.w * 3.0f, c.h * 4.0f));
        }
    }

    /* Why fit_w x fit_h is the uncropped size: fitting the rounded crop
     * (622x467) into a 640x480 back buffer gives 639 columns, a one-column
     * bar that only nv2a_present_rect's near-shape fill hides, so the crop
     * must hand over the whole frame's size, which gives the whole buffer
     * on its own. */
    nv2a_present_crop(640, 480, 640, 480, 1, 0, 9, NV2A_PRESENT_NEAREST, &c);
    CHECK(c.fit_w == 640 && c.fit_h == 480);
    nv2a_present_rect(c.fit_w, c.fit_h, 640, 480, NV2A_PRESENT_NEAREST, &r);
    CHECK(rect_is(&r, 0, 0, 640, 480));
    /* and the same 4:3 letterbox as the uncropped frame in a 16:9 window */
    nv2a_present_rect(c.fit_w, c.fit_h, 2560, 1440, NV2A_PRESENT_LINEAR, &r);
    CHECK(rect_is(&r, 320, 0, 1920, 1440));

    /* D3D11: the render target can be larger than the frame; the UV terms
     * divide by the texture, the rectangle stays in frame px. */
    nv2a_present_crop(1280, 960, 2048, 1024, 2, 0, 9, NV2A_PRESENT_LINEAR, &c);
    CHECK(crop_is(&c, 18.0f, 13.5f, 1244.0f, 933.0f, 2048, 1024, 1280, 960));
    CHECK(edges_at(&c, 2048, 1024, 18.0f, 13.5f, 1262.0f, 946.5f));

    /* integer: the vertical cut rounded to whole host rows (6.75 -> 7,
     * 13.5 -> 14), the cropped size to fit, and a real whole multiple. */
    nv2a_present_crop(640, 480, 640, 480, 1, 0, 9, NV2A_PRESENT_INTEGER, &c);
    CHECK(crop_is(&c, 9.0f, 7.0f, 622.0f, 466.0f, 640, 480, 622, 466));
    whole = nv2a_present_rect(c.fit_w, c.fit_h, 1920, 1080, NV2A_PRESENT_INTEGER, &r);
    CHECK(whole == 1 && rect_is(&r, 338, 74, 1244, 932));
    nv2a_present_crop(1280, 960, 1280, 960, 2, 0, 9, NV2A_PRESENT_INTEGER, &c);
    CHECK(crop_is(&c, 18.0f, 14.0f, 1244.0f, 932.0f, 1280, 960, 1244, 932));
    /* the accepted drift: under 0.2% from 4:3 */
    CHECK(fabsf(c.w / c.h - 4.0f / 3.0f) < 0.002f * 4.0f / 3.0f);

    /* hor+ (a pad) refuses the crop: the whole padded frame */
    nv2a_present_crop(854, 480, 854, 480, 1, 107, 9, NV2A_PRESENT_LINEAR, &c);
    CHECK(crop_is(&c, 0.0f, 0.0f, 854.0f, 480.0f, 854, 480, 854, 480));

    /* a frame no wider than 2 * ov guest px is shown whole, never empty */
    nv2a_present_crop(128, 96, 128, 96, 1, 0, 64, NV2A_PRESENT_LINEAR, &c);
    CHECK(crop_is(&c, 0.0f, 0.0f, 128.0f, 96.0f, 128, 96, 128, 96));
    nv2a_present_crop(256, 192, 256, 192, 2, 0, 64, NV2A_PRESENT_LINEAR, &c);
    CHECK(c.w == 256.0f);
    nv2a_present_crop(130, 96, 130, 96, 1, 0, 64, NV2A_PRESENT_LINEAR, &c);
    CHECK(c.w > 0.0f && c.h > 0.0f);

    /* a zero size: the whole (empty) frame, no division by zero */
    nv2a_present_crop(0, 0, 0, 0, 1, 0, 9, NV2A_PRESENT_LINEAR, &c);
    CHECK(c.w == 0.0f && c.fit_w == 0);

    /* present.shader's rect: with the chain on, the picture
     * fills what the stock present shows. Under present.surface=internal
     * the compositor fits the frame, so integer turns into the fit. */
    whole = nv2a_present_chain_rect(1280, 960, 1280, 800, NV2A_PRESENT_INTEGER, 1, &r);
    CHECK(whole == 0 && rect_is(&r, 106, 0, 1067, 800));
    nv2a_present_rect(1280, 960, 1280, 800, NV2A_PRESENT_LINEAR, &r);
    CHECK(rect_is(&r, 106, 0, 1067, 800));
    /* elsewhere it is the stock rect, integer included */
    nv2a_present_chain_rect(1280, 960, 2560, 1600, NV2A_PRESENT_INTEGER, 0, &r);
    CHECK(rect_is(&r, 640, 320, 1280, 960));
    nv2a_present_chain_rect(1280, 960, 1280, 800, NV2A_PRESENT_INTEGER, 0, &r);
    {
        struct nv2a_rect s;
        nv2a_present_rect(1280, 960, 1280, 800, NV2A_PRESENT_INTEGER, &s);
        CHECK(rect_is(&r, s.x, s.y, s.w, s.h));
    }
    nv2a_present_chain_rect(1536, 960, 1280, 800, NV2A_PRESENT_NEAREST, 1, &r);
    CHECK(rect_is(&r, 0, 0, 1280, 800));
    /* hor+ 16:9 (854 x 480 padded, 1708 x 960 at 2x) on the Deck under
     * internal: integer is the fit, the 1/256 snap leaving it 720 high
     * would need a 799-row output, so it keeps its 719 rows. */
    nv2a_present_chain_rect(1708, 960, 1280, 800, NV2A_PRESENT_INTEGER, 1, &r);
    {
        struct nv2a_rect s;
        nv2a_present_rect(1708, 960, 1280, 800, NV2A_PRESENT_LINEAR, &s);
        CHECK(rect_is(&r, s.x, s.y, s.w, s.h));
        CHECK(rect_is(&r, 0, 40, 1280, 719));
    }
    /* 1366 x 768 under internal, 4:3 at 2x: pillarboxed to 1024 wide */
    nv2a_present_chain_rect(1280, 960, 1366, 768, NV2A_PRESENT_INTEGER, 1, &r);
    CHECK(rect_is(&r, 171, 0, 1024, 768));
    /* render.scale 3 under window keeps integer: 1x of 1920 x 1440 in
     * 4K, centred; in 1280 x 800 no whole multiple fits, so the fit. */
    nv2a_present_chain_rect(1920, 1440, 3840, 2160, NV2A_PRESENT_INTEGER, 0, &r);
    CHECK(rect_is(&r, 960, 360, 1920, 1440));
    nv2a_present_chain_rect(1920, 1440, 1280, 800, NV2A_PRESENT_INTEGER, 0, &r);
    CHECK(rect_is(&r, 106, 0, 1067, 800));

    /* present.shader's source: a crt* chain gets the guest's
     * line count at full host width; anything else the scaled frame. */
    {
        uint32_t sw, sh;
        nv2a_present_chain_source(1280, 960, 2, 1, &sw, &sh);
        CHECK(sw == 1280 && sh == 480);
        nv2a_present_chain_source(1920, 1440, 3, 1, &sw, &sh);
        CHECK(sw == 1920 && sh == 480);
        nv2a_present_chain_source(640, 480, 1, 1, &sw, &sh);
        CHECK(sw == 640 && sh == 480);
        nv2a_present_chain_source(1920, 1440, 3, 0, &sw, &sh);
        CHECK(sw == 1920 && sh == 1440);
        nv2a_present_chain_source(2, 1, 2, 1, &sw, &sh);
        CHECK(sh == 1);
        /* PAL at 3x: 576 lines */
        nv2a_present_chain_source(2160, 1728, 3, 1, &sw, &sh);
        CHECK(sw == 2160 && sh == 576);
        nv2a_present_chain_source(1280, 962, 2, 1, &sw, &sh);
        CHECK(sh == 481);
        /* no whole guest line per 2 rows: the host rows, which a backend
         * reads as no resolve (sh == hh) */
        nv2a_present_chain_source(1280, 961, 2, 1, &sw, &sh);
        CHECK(sw == 1280 && sh == 961);
        nv2a_present_chain_source(1920, 1441, 3, 1, &sw, &sh);
        CHECK(sh == 1441);
    }

    printf("present_crop: ok\n");
    return 0;
}
