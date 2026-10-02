#include "gfx.h"
#include <math.h>

// Icons drawn from shapes, so they are sharp at every size and need no
// image files. App icons are a coloured tile with a white symbol; file and
// interface icons are symbols in a given colour.

struct pen {
    struct gfx *g;
    float x, y, s;                  // origin and size of the unit box
    color_t c;
};

#define X(v) (p->x + (v) * p->s)
#define Y(v) (p->y + (v) * p->s)

static void line(struct pen *p, float x0, float y0, float x1, float y1, float w)
{
    gfx_line(p->g, X(x0), Y(y0), X(x1), Y(y1), fmaxf(1.2f, w * p->s), p->c);
}

static void dot(struct pen *p, float cx, float cy, float r)
{
    gfx_circle(p->g, X(cx), Y(cy), r * p->s, p->c);
}

static void ring(struct pen *p, float cx, float cy, float r, float w)
{
    gfx_ring(p->g, X(cx), Y(cy), r * p->s, fmaxf(1.2f, w * p->s), p->c);
}

static void box(struct pen *p, float x, float y, float w, float h, float r)
{
    gfx_fill_rounded(p->g, (struct rect){ (int)roundf(X(x)), (int)roundf(Y(y)), (int)roundf(w * p->s),
                                          (int)roundf(h * p->s) }, (int)roundf(r * p->s), p->c);
}

static void frame(struct pen *p, float x, float y, float w, float h, float r, float lw)
{
    gfx_outline_rounded(p->g, (struct rect){ (int)roundf(X(x)), (int)roundf(Y(y)), (int)roundf(w * p->s),
                                             (int)roundf(h * p->s) }, (int)roundf(r * p->s),
                        (int)fmaxf(1, roundf(lw * p->s)), p->c);
}

static void poly(struct pen *p, const float *pts, int n)
{
    float xy[64];

    for (int i = 0; i < n && i < 32; i++) {
        xy[2 * i] = X(pts[2 * i]);
        xy[2 * i + 1] = Y(pts[2 * i + 1]);
    }
    gfx_polygon(p->g, xy, n, p->c);
}

static void arc(struct pen *p, float cx, float cy, float r, float a0, float a1, float w)
{
    int steps = 16;
    float px = cx + r * cosf(a0), py = cy + r * sinf(a0);

    for (int i = 1; i <= steps; i++) {
        float a = a0 + (a1 - a0) * i / steps, nx = cx + r * cosf(a), ny = cy + r * sinf(a);

        line(p, px, py, nx, ny, w);
        px = nx;
        py = ny;
    }
}

static void person(struct pen *p, float cx, float top, float scale)
{
    float hw = 0.17f * scale;

    dot(p, cx, top + 0.11f * scale, 0.11f * scale);
    {
        float pts[] = { cx - hw, top + 0.42f * scale, cx - hw * 0.7f, top + 0.27f * scale,
                        cx + hw * 0.7f, top + 0.27f * scale, cx + hw, top + 0.42f * scale };

        poly(p, pts, 4);
    }
}

// ---- Symbols ----

static void sym_terminal(struct pen *p)
{
    line(p, 0.22f, 0.32f, 0.40f, 0.48f, 0.07f);
    line(p, 0.40f, 0.48f, 0.22f, 0.64f, 0.07f);
    line(p, 0.48f, 0.66f, 0.76f, 0.66f, 0.07f);
}

static void sym_folder(struct pen *p)
{
    float back[] = { 0.18f, 0.28f, 0.42f, 0.28f, 0.48f, 0.35f, 0.82f, 0.35f, 0.82f, 0.74f, 0.18f, 0.74f };

    poly(p, back, 6);
}

static void sym_gear(struct pen *p)
{
    for (int i = 0; i < 8; i++) {
        float a = i * 3.14159265f / 4;

        line(p, 0.5f + 0.16f * cosf(a), 0.5f + 0.16f * sinf(a), 0.5f + 0.30f * cosf(a), 0.5f + 0.30f * sinf(a),
             0.11f);
    }
    ring(p, 0.5f, 0.5f, 0.19f, 0.09f);
}

static void sym_chart(struct pen *p)
{
    box(p, 0.22f, 0.52f, 0.12f, 0.24f, 0.02f);
    box(p, 0.44f, 0.34f, 0.12f, 0.42f, 0.02f);
    box(p, 0.66f, 0.24f, 0.12f, 0.52f, 0.02f);
}

static void sym_tasks(struct pen *p)
{
    float ys[] = { 0.30f, 0.50f, 0.70f };

    for (int i = 0; i < 3; i++) {
        dot(p, 0.27f, ys[i], 0.045f);
        line(p, 0.38f, ys[i], 0.76f, ys[i], 0.065f);
    }
}

static void sym_features(struct pen *p)
{
    box(p, 0.22f, 0.22f, 0.24f, 0.24f, 0.05f);
    box(p, 0.54f, 0.22f, 0.24f, 0.24f, 0.05f);
    box(p, 0.22f, 0.54f, 0.24f, 0.24f, 0.05f);
    line(p, 0.66f, 0.56f, 0.66f, 0.76f, 0.07f);
    line(p, 0.56f, 0.66f, 0.76f, 0.66f, 0.07f);
}

static void sym_console(struct pen *p)
{
    frame(p, 0.18f, 0.22f, 0.64f, 0.56f, 0.06f, 0.06f);
    line(p, 0.18f, 0.36f, 0.82f, 0.36f, 0.05f);
    line(p, 0.40f, 0.36f, 0.40f, 0.78f, 0.05f);
    line(p, 0.48f, 0.48f, 0.72f, 0.48f, 0.045f);
    line(p, 0.48f, 0.60f, 0.66f, 0.60f, 0.045f);
}

static void sym_users(struct pen *p)
{
    color_t c = p->c;

    p->c = ALPHA(c, 0xB0);
    person(p, 0.64f, 0.22f, 0.95f);
    p->c = c;
    person(p, 0.40f, 0.30f, 1.05f);
}

static void sym_user(struct pen *p)
{
    person(p, 0.5f, 0.2f, 1.45f);
}

static void sym_clock(struct pen *p)
{
    ring(p, 0.5f, 0.5f, 0.28f, 0.07f);
    line(p, 0.5f, 0.5f, 0.5f, 0.33f, 0.065f);
    line(p, 0.5f, 0.5f, 0.63f, 0.58f, 0.065f);
}

static void sym_cron(struct pen *p)
{
    sym_clock(p);
    arc(p, 0.5f, 0.5f, 0.36f, -2.6f, -0.9f, 0.05f);
}

static void sym_note(struct pen *p)
{
    box(p, 0.25f, 0.18f, 0.50f, 0.64f, 0.05f);
}

static void sym_notepad(struct pen *p)
{
    color_t c = p->c;

    frame(p, 0.25f, 0.18f, 0.50f, 0.64f, 0.05f, 0.06f);
    p->c = ALPHA(c, 0xD0);
    for (int i = 0; i < 4; i++)
        line(p, 0.34f, 0.34f + i * 0.11f, 0.66f - (i == 3 ? 0.12f : 0), 0.34f + i * 0.11f, 0.045f);
    p->c = c;
}

static void sym_calculator(struct pen *p)
{
    frame(p, 0.26f, 0.16f, 0.48f, 0.68f, 0.06f, 0.06f);
    box(p, 0.34f, 0.25f, 0.32f, 0.12f, 0.02f);
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            dot(p, 0.37f + c * 0.13f, 0.48f + r * 0.11f, 0.035f);
}

static void sym_globe(struct pen *p)
{
    ring(p, 0.5f, 0.5f, 0.29f, 0.06f);
    line(p, 0.21f, 0.5f, 0.79f, 0.5f, 0.05f);
    line(p, 0.5f, 0.21f, 0.5f, 0.79f, 0.05f);
    arc(p, 0.5f, 0.5f, 0.29f, -1.3f, 1.3f, 0.0001f);
    {
        // Meridians as narrow ellipses: two arcs.
        int steps = 18;

        for (int i = 0; i < steps; i++) {
            float a0 = -1.5708f + 3.14159f * i / steps, a1 = -1.5708f + 3.14159f * (i + 1) / steps;

            line(p, 0.5f + 0.13f * cosf(a0), 0.5f + 0.29f * sinf(a0), 0.5f + 0.13f * cosf(a1),
                 0.5f + 0.29f * sinf(a1), 0.045f);
            line(p, 0.5f - 0.13f * cosf(a0), 0.5f + 0.29f * sinf(a0), 0.5f - 0.13f * cosf(a1),
                 0.5f + 0.29f * sinf(a1), 0.045f);
        }
    }
}

static void sym_image(struct pen *p)
{
    float hill[] = { 0.20f, 0.74f, 0.42f, 0.44f, 0.56f, 0.60f, 0.64f, 0.50f, 0.80f, 0.74f };

    frame(p, 0.18f, 0.22f, 0.64f, 0.56f, 0.06f, 0.06f);
    dot(p, 0.36f, 0.38f, 0.06f);
    poly(p, hill, 5);
}

static void sym_mail(struct pen *p)
{
    frame(p, 0.18f, 0.28f, 0.64f, 0.44f, 0.05f, 0.06f);
    line(p, 0.21f, 0.32f, 0.5f, 0.54f, 0.06f);
    line(p, 0.5f, 0.54f, 0.79f, 0.32f, 0.06f);
}

static void sym_music(struct pen *p)
{
    dot(p, 0.36f, 0.68f, 0.10f);
    dot(p, 0.66f, 0.60f, 0.10f);
    line(p, 0.44f, 0.68f, 0.44f, 0.26f, 0.06f);
    line(p, 0.74f, 0.60f, 0.74f, 0.20f, 0.06f);
    line(p, 0.44f, 0.27f, 0.74f, 0.21f, 0.10f);
}

static void sym_camera(struct pen *p)
{
    float top[] = { 0.38f, 0.30f, 0.44f, 0.22f, 0.58f, 0.22f, 0.64f, 0.30f };

    box(p, 0.18f, 0.30f, 0.64f, 0.46f, 0.07f);
    poly(p, top, 4);
    {
        color_t c = p->c;

        p->c = ALPHA(0x000000, 0x60);
        dot(p, 0.5f, 0.53f, 0.15f);
        p->c = c;
        ring(p, 0.5f, 0.53f, 0.11f, 0.05f);
    }
}

static void sym_builder(struct pen *p)
{
    frame(p, 0.18f, 0.22f, 0.64f, 0.56f, 0.05f, 0.06f);
    box(p, 0.26f, 0.32f, 0.20f, 0.38f, 0.03f);
    box(p, 0.52f, 0.32f, 0.22f, 0.14f, 0.03f);
    box(p, 0.52f, 0.52f, 0.22f, 0.18f, 0.03f);
}

static void sym_appmaker(struct pen *p)
{
    float top[] = { 0.5f, 0.18f, 0.80f, 0.34f, 0.5f, 0.50f, 0.20f, 0.34f };
    color_t c = p->c;

    poly(p, top, 4);
    p->c = ALPHA(c, 0xC0);
    {
        float left[] = { 0.20f, 0.38f, 0.48f, 0.54f, 0.48f, 0.84f, 0.20f, 0.68f };

        poly(p, left, 4);
    }
    p->c = ALPHA(c, 0x90);
    {
        float right[] = { 0.80f, 0.38f, 0.52f, 0.54f, 0.52f, 0.84f, 0.80f, 0.68f };

        poly(p, right, 4);
    }
    p->c = c;
}

static void sym_bug(struct pen *p)
{
    dot(p, 0.5f, 0.32f, 0.09f);
    box(p, 0.36f, 0.38f, 0.28f, 0.38f, 0.13f);
    for (int i = 0; i < 3; i++) {
        float y = 0.46f + i * 0.11f;

        line(p, 0.36f, y, 0.22f, y - 0.04f + i * 0.04f, 0.045f);
        line(p, 0.64f, y, 0.78f, y - 0.04f + i * 0.04f, 0.045f);
    }
    line(p, 0.44f, 0.24f, 0.38f, 0.16f, 0.04f);
    line(p, 0.56f, 0.24f, 0.62f, 0.16f, 0.04f);
}

static void sym_logs(struct pen *p)
{
    frame(p, 0.24f, 0.18f, 0.52f, 0.64f, 0.05f, 0.06f);
    for (int i = 0; i < 5; i++)
        line(p, 0.33f, 0.31f + i * 0.095f, i % 2 ? 0.58f : 0.67f, 0.31f + i * 0.095f, 0.04f);
}

static void sym_power(struct pen *p)
{
    arc(p, 0.5f, 0.53f, 0.25f, -1.05f, 4.19f, 0.07f);
    line(p, 0.5f, 0.20f, 0.5f, 0.50f, 0.07f);
}

static void sym_logout(struct pen *p)
{
    line(p, 0.46f, 0.22f, 0.24f, 0.22f, 0.06f);
    line(p, 0.24f, 0.22f, 0.24f, 0.78f, 0.06f);
    line(p, 0.24f, 0.78f, 0.46f, 0.78f, 0.06f);
    line(p, 0.40f, 0.5f, 0.78f, 0.5f, 0.06f);
    line(p, 0.64f, 0.36f, 0.78f, 0.5f, 0.06f);
    line(p, 0.64f, 0.64f, 0.78f, 0.5f, 0.06f);
}

static void sym_restart(struct pen *p)
{
    arc(p, 0.5f, 0.52f, 0.25f, -0.9f, 4.4f, 0.07f);
    {
        float head[] = { 0.62f, 0.12f, 0.76f, 0.34f, 0.52f, 0.34f };

        poly(p, head, 3);
    }
}

static void sym_lock(struct pen *p)
{
    box(p, 0.26f, 0.44f, 0.48f, 0.36f, 0.06f);
    arc(p, 0.5f, 0.44f, 0.15f, 3.14159f, 6.28318f, 0.07f);
}

static void sym_search(struct pen *p)
{
    ring(p, 0.44f, 0.44f, 0.19f, 0.07f);
    line(p, 0.58f, 0.58f, 0.78f, 0.78f, 0.09f);
}

static void sym_apps(struct pen *p)
{
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            box(p, 0.22f + c * 0.20f, 0.22f + r * 0.20f, 0.15f, 0.15f, 0.04f);
}

static void sym_home(struct pen *p)
{
    float roof[] = { 0.5f, 0.18f, 0.84f, 0.48f, 0.74f, 0.48f, 0.74f, 0.80f, 0.26f, 0.80f, 0.26f, 0.48f,
                     0.16f, 0.48f };

    poly(p, roof, 7);
}

static void sym_desktop(struct pen *p)
{
    frame(p, 0.16f, 0.22f, 0.68f, 0.46f, 0.05f, 0.06f);
    line(p, 0.5f, 0.68f, 0.5f, 0.78f, 0.06f);
    line(p, 0.34f, 0.80f, 0.66f, 0.80f, 0.06f);
}

static void sym_download(struct pen *p)
{
    line(p, 0.5f, 0.18f, 0.5f, 0.60f, 0.07f);
    line(p, 0.32f, 0.44f, 0.5f, 0.62f, 0.07f);
    line(p, 0.68f, 0.44f, 0.5f, 0.62f, 0.07f);
    line(p, 0.22f, 0.78f, 0.78f, 0.78f, 0.07f);
}

static void sym_trash(struct pen *p)
{
    line(p, 0.22f, 0.28f, 0.78f, 0.28f, 0.06f);
    line(p, 0.40f, 0.20f, 0.60f, 0.20f, 0.06f);
    frame(p, 0.30f, 0.32f, 0.40f, 0.48f, 0.05f, 0.06f);
    line(p, 0.44f, 0.42f, 0.44f, 0.70f, 0.04f);
    line(p, 0.56f, 0.42f, 0.56f, 0.70f, 0.04f);
}

static void sym_network(struct pen *p)
{
    dot(p, 0.5f, 0.72f, 0.06f);
    arc(p, 0.5f, 0.74f, 0.20f, -2.36f, -0.79f, 0.06f);
    arc(p, 0.5f, 0.74f, 0.36f, -2.36f, -0.79f, 0.06f);
}

static void sym_volume(struct pen *p)
{
    float sp[] = { 0.20f, 0.40f, 0.34f, 0.40f, 0.52f, 0.24f, 0.52f, 0.76f, 0.34f, 0.60f, 0.20f, 0.60f };

    poly(p, sp, 6);
    arc(p, 0.52f, 0.5f, 0.14f, -0.9f, 0.9f, 0.055f);
    arc(p, 0.52f, 0.5f, 0.27f, -0.9f, 0.9f, 0.055f);
}

static void sym_text(struct pen *p)
{
    line(p, 0.28f, 0.26f, 0.72f, 0.26f, 0.08f);
    line(p, 0.5f, 0.26f, 0.5f, 0.76f, 0.08f);
}

static void sym_code(struct pen *p)
{
    line(p, 0.38f, 0.32f, 0.22f, 0.5f, 0.07f);
    line(p, 0.22f, 0.5f, 0.38f, 0.68f, 0.07f);
    line(p, 0.62f, 0.32f, 0.78f, 0.5f, 0.07f);
    line(p, 0.78f, 0.5f, 0.62f, 0.68f, 0.07f);
}

static void sym_package(struct pen *p)
{
    frame(p, 0.20f, 0.30f, 0.60f, 0.48f, 0.04f, 0.06f);
    line(p, 0.20f, 0.30f, 0.30f, 0.18f, 0.06f);
    line(p, 0.80f, 0.30f, 0.70f, 0.18f, 0.06f);
    line(p, 0.30f, 0.18f, 0.70f, 0.18f, 0.06f);
    line(p, 0.5f, 0.18f, 0.5f, 0.46f, 0.06f);
}

static void sym_info(struct pen *p)
{
    ring(p, 0.5f, 0.5f, 0.29f, 0.07f);
    dot(p, 0.5f, 0.34f, 0.05f);
    line(p, 0.5f, 0.46f, 0.5f, 0.68f, 0.08f);
}

static void sym_warning(struct pen *p)
{
    float tri[] = { 0.5f, 0.16f, 0.86f, 0.80f, 0.14f, 0.80f };
    color_t c = p->c;

    poly(p, tri, 3);
    p->c = RGB(0x1D2633);
    line(p, 0.5f, 0.38f, 0.5f, 0.60f, 0.07f);
    dot(p, 0.5f, 0.70f, 0.045f);
    p->c = c;
}

static void sym_error(struct pen *p)
{
    ring(p, 0.5f, 0.5f, 0.29f, 0.07f);
    line(p, 0.38f, 0.38f, 0.62f, 0.62f, 0.07f);
    line(p, 0.62f, 0.38f, 0.38f, 0.62f, 0.07f);
}

static void sym_question(struct pen *p)
{
    ring(p, 0.5f, 0.5f, 0.29f, 0.07f);
    arc(p, 0.5f, 0.42f, 0.09f, -3.3f, 0.9f, 0.065f);
    line(p, 0.56f, 0.49f, 0.5f, 0.56f, 0.065f);
    dot(p, 0.5f, 0.68f, 0.045f);
}

static void sym_close(struct pen *p)
{
    line(p, 0.30f, 0.30f, 0.70f, 0.70f, 0.07f);
    line(p, 0.70f, 0.30f, 0.30f, 0.70f, 0.07f);
}

static void sym_plus(struct pen *p)
{
    line(p, 0.5f, 0.24f, 0.5f, 0.76f, 0.08f);
    line(p, 0.24f, 0.5f, 0.76f, 0.5f, 0.08f);
}

static void sym_back(struct pen *p)
{
    line(p, 0.62f, 0.24f, 0.36f, 0.5f, 0.08f);
    line(p, 0.36f, 0.5f, 0.62f, 0.76f, 0.08f);
}

static void sym_forward(struct pen *p)
{
    line(p, 0.38f, 0.24f, 0.64f, 0.5f, 0.08f);
    line(p, 0.64f, 0.5f, 0.38f, 0.76f, 0.08f);
}

static void sym_up(struct pen *p)
{
    line(p, 0.24f, 0.62f, 0.5f, 0.36f, 0.08f);
    line(p, 0.5f, 0.36f, 0.76f, 0.62f, 0.08f);
}

static void sym_refresh(struct pen *p)
{
    sym_restart(p);
}

static void sym_play(struct pen *p)
{
    float tri[] = { 0.34f, 0.22f, 0.78f, 0.5f, 0.34f, 0.78f };

    poly(p, tri, 3);
}

static void sym_pause(struct pen *p)
{
    box(p, 0.30f, 0.24f, 0.14f, 0.52f, 0.03f);
    box(p, 0.56f, 0.24f, 0.14f, 0.52f, 0.03f);
}

static void sym_stop(struct pen *p)
{
    box(p, 0.28f, 0.28f, 0.44f, 0.44f, 0.05f);
}

static void sym_bell(struct pen *p)
{
    float body[] = { 0.30f, 0.66f, 0.34f, 0.40f, 0.42f, 0.28f, 0.58f, 0.28f, 0.66f, 0.40f, 0.70f, 0.66f };

    poly(p, body, 6);
    line(p, 0.24f, 0.68f, 0.76f, 0.68f, 0.07f);
    dot(p, 0.5f, 0.78f, 0.06f);
    dot(p, 0.5f, 0.24f, 0.05f);
}

static void sym_shield(struct pen *p)
{
    float sh[] = { 0.5f, 0.16f, 0.78f, 0.26f, 0.76f, 0.52f, 0.5f, 0.84f, 0.24f, 0.52f, 0.22f, 0.26f };

    poly(p, sh, 6);
}

static void sym_key(struct pen *p)
{
    ring(p, 0.34f, 0.5f, 0.13f, 0.07f);
    line(p, 0.47f, 0.5f, 0.80f, 0.5f, 0.07f);
    line(p, 0.70f, 0.5f, 0.70f, 0.64f, 0.07f);
    line(p, 0.80f, 0.5f, 0.80f, 0.60f, 0.07f);
}

static void sym_cpu(struct pen *p)
{
    frame(p, 0.30f, 0.30f, 0.40f, 0.40f, 0.05f, 0.06f);
    box(p, 0.42f, 0.42f, 0.16f, 0.16f, 0.02f);
    for (int i = 0; i < 3; i++) {
        float v = 0.38f + i * 0.12f;

        line(p, v, 0.18f, v, 0.30f, 0.04f);
        line(p, v, 0.70f, v, 0.82f, 0.04f);
        line(p, 0.18f, v, 0.30f, v, 0.04f);
        line(p, 0.70f, v, 0.82f, v, 0.04f);
    }
}

static void sym_disk(struct pen *p)
{
    frame(p, 0.18f, 0.30f, 0.64f, 0.40f, 0.08f, 0.06f);
    dot(p, 0.68f, 0.5f, 0.05f);
    line(p, 0.28f, 0.5f, 0.52f, 0.5f, 0.05f);
}

struct symbol {
    const char *name;
    void (*draw)(struct pen *);
    color_t top, bottom;            // tile colours for app icons
};

static const struct symbol symbols[] = {
    { "terminal", sym_terminal, RGB(0x3A4452), RGB(0x1C2128) },
    { "files", sym_folder, RGB(0x4C9BFF), RGB(0x1F66D1) },
    { "settings", sym_gear, RGB(0x8C96A6), RGB(0x535D6C) },
    { "resources", sym_chart, RGB(0x2CC4B0), RGB(0x0F8A7B) },
    { "tasks", sym_tasks, RGB(0x9C6BFF), RGB(0x6438D0) },
    { "features", sym_features, RGB(0xFFA24C), RGB(0xE06A12) },
    { "console", sym_console, RGB(0x6C7CFF), RGB(0x3A47C9) },
    { "users", sym_users, RGB(0x35C6E8), RGB(0x1488B8) },
    { "cron", sym_cron, RGB(0xFFC53D), RGB(0xD68A00) },
    { "notepad", sym_notepad, RGB(0xFFD45C), RGB(0xE8A417) },
    { "calculator", sym_calculator, RGB(0x5A6577), RGB(0x2E3542) },
    { "browser", sym_globe, RGB(0x45A6FF), RGB(0x1565D8) },
    { "clock", sym_clock, RGB(0x3C4F8F), RGB(0x1E2A57) },
    { "images", sym_image, RGB(0x37D18B), RGB(0x10915A) },
    { "mail", sym_mail, RGB(0xFF6A6A), RGB(0xD13434) },
    { "music", sym_music, RGB(0xFF6FB2), RGB(0xD12C7D) },
    { "camera", sym_camera, RGB(0x6A7280), RGB(0x30353E) },
    { "builder", sym_builder, RGB(0xA77BFF), RGB(0x6E3FD6) },
    { "appmaker", sym_appmaker, RGB(0xF070D8), RGB(0xA92A96) },
    { "debugger", sym_bug, RGB(0xFF7A59), RGB(0xC93A1C) },
    { "logs", sym_logs, RGB(0x6E8296), RGB(0x3B4A5A) },
    { "user", sym_user, RGB(0x7A8CFF), RGB(0x4655C9) },
    { "folder", sym_folder, 0, 0 },
    { "file", sym_note, 0, 0 },
    { "text", sym_text, 0, 0 },
    { "code", sym_code, 0, 0 },
    { "image", sym_image, 0, 0 },
    { "audio", sym_music, 0, 0 },
    { "package", sym_package, RGB(0xC79A62), RGB(0x8A6233) },
    { "power", sym_power, 0, 0 },
    { "logout", sym_logout, 0, 0 },
    { "restart", sym_restart, 0, 0 },
    { "lock", sym_lock, 0, 0 },
    { "search", sym_search, 0, 0 },
    { "apps", sym_apps, 0, 0 },
    { "home", sym_home, 0, 0 },
    { "desktop", sym_desktop, 0, 0 },
    { "documents", sym_notepad, 0, 0 },
    { "downloads", sym_download, 0, 0 },
    { "trash", sym_trash, 0, 0 },
    { "network", sym_network, 0, 0 },
    { "volume", sym_volume, 0, 0 },
    { "info", sym_info, 0, 0 },
    { "warning", sym_warning, 0, 0 },
    { "error", sym_error, 0, 0 },
    { "question", sym_question, 0, 0 },
    { "close", sym_close, 0, 0 },
    { "add", sym_plus, 0, 0 },
    { "back", sym_back, 0, 0 },
    { "forward", sym_forward, 0, 0 },
    { "up", sym_up, 0, 0 },
    { "refresh", sym_refresh, 0, 0 },
    { "play", sym_play, 0, 0 },
    { "pause", sym_pause, 0, 0 },
    { "stop", sym_stop, 0, 0 },
    { "bell", sym_bell, 0, 0 },
    { "shield", sym_shield, 0, 0 },
    { "key", sym_key, 0, 0 },
    { "cpu", sym_cpu, 0, 0 },
    { "disk", sym_disk, 0, 0 },
    { "clock-glyph", sym_clock, 0, 0 },
    { "mail-glyph", sym_mail, 0, 0 },
    { "camera-glyph", sym_camera, 0, 0 },
    { "globe", sym_globe, 0, 0 },
    { "gear", sym_gear, 0, 0 },
    { "chart", sym_chart, 0, 0 },
};

static const struct symbol *find(const char *name)
{
    for (size_t i = 0; i < sizeof(symbols) / sizeof(symbols[0]); i++)
        if (!strcmp(symbols[i].name, name))
            return &symbols[i];
    return NULL;
}

bool icon_exists(const char *name)
{
    return find(name) != NULL;
}

void icon_draw_glyph(struct gfx *g, const char *name, struct rect r, color_t c)
{
    const struct symbol *s = find(name);
    struct pen p = { g, r.x, r.y, (float)MIN(r.w, r.h), c };

    if (!s)
        s = find("file");
    p.x += (r.w - p.s) / 2;
    p.y += (r.h - p.s) / 2;
    s->draw(&p);
}

static void draw_file_icon(struct gfx *g, const char *name, struct rect r)
{
    float s = MIN(r.w, r.h);
    struct pen p = { g, r.x + (r.w - s) / 2, r.y + (r.h - s) / 2, s, RGB(0xFFFFFF) };

    if (!strcmp(name, "folder")) {
        float back[] = { 0.08f, 0.20f, 0.40f, 0.20f, 0.48f, 0.28f, 0.92f, 0.28f, 0.92f, 0.84f, 0.08f, 0.84f };
        float front[] = { 0.08f, 0.38f, 0.92f, 0.38f, 0.92f, 0.84f, 0.08f, 0.84f };

        p.c = RGB(0xE8A417);
        poly(&p, back, 6);
        p.c = RGB(0xFFC94D);
        poly(&p, front, 4);
        return;
    }
    // A page with a folded corner and the type's symbol.
    {
        float page[] = { 0.20f, 0.08f, 0.62f, 0.08f, 0.82f, 0.28f, 0.82f, 0.92f, 0.20f, 0.92f };
        float fold[] = { 0.62f, 0.08f, 0.82f, 0.28f, 0.62f, 0.28f };
        color_t accent = !strcmp(name, "image") ? RGB(0x10915A) : !strcmp(name, "audio") ? RGB(0xD12C7D)
                         : !strcmp(name, "code") ? RGB(0x6438D0) : !strcmp(name, "package") ? RGB(0x8A6233)
                         : !strcmp(name, "text") ? RGB(0x3B4A5A) : RGB(0x7A8594);

        p.c = RGB(0xC5CDD8);
        {
            float edge[] = { 0.19f, 0.07f, 0.63f, 0.07f, 0.83f, 0.27f, 0.83f, 0.93f, 0.19f, 0.93f };

            poly(&p, edge, 5);
        }
        p.c = RGB(0xFFFFFF);
        poly(&p, page, 5);
        p.c = RGB(0xDDE3EA);
        poly(&p, fold, 3);
        if (strcmp(name, "file")) {
            struct pen q = { g, p.x + 0.26f * s, p.y + 0.36f * s, 0.50f * s, accent };
            const struct symbol *sym = find(name);

            if (sym)
                sym->draw(&q);
        }
    }
}

void icon_draw(struct gfx *g, const char *name, struct rect r)
{
    const struct symbol *s;

    // "glyph:name" is a symbol in the accent colour (sidebars, lists).
    if (!strncmp(name, "glyph:", 6)) {
        icon_draw_glyph(g, name + 6, r, RGB(0x3D7BFF));
        return;
    }
    s = find(name);
    int size = MIN(r.w, r.h);
    struct rect tile = { r.x + (r.w - size) / 2, r.y + (r.h - size) / 2, size, size };

    if (!s) {
        draw_file_icon(g, "file", r);
        return;
    }
    if (!s->top) {
        draw_file_icon(g, name, r);
        return;
    }
    // App tile: rounded square with a soft vertical gradient.
    {
        struct surface *tmp = surface_create(size, size);
        struct gfx tg;
        int rad = size * 22 / 100;

        if (!tmp)
            return;
        gfx_init(&tg, tmp);
        gfx_gradient(&tg, (struct rect){ 0, 0, size, size }, s->top, s->bottom);
        // Cut the corners: clear outside a rounded rectangle.
        for (int y = 0; y < size; y++) {
            for (int x = 0; x < size; x++) {
                float dx = fmaxf(fmaxf(rad - x - 0.5f, x + 0.5f - (size - rad)), 0);
                float dy = fmaxf(fmaxf(rad - y - 0.5f, y + 0.5f - (size - rad)), 0);
                float d = sqrtf(dx * dx + dy * dy) - rad;
                float a = d <= -0.5f ? 1 : d >= 0.5f ? 0 : 0.5f - d;

                if (a < 1) {
                    uint32_t px = tmp->pixels[y * tmp->stride + x];
                    uint32_t out = 0;

                    for (int sh = 0; sh < 32; sh += 8)
                        out |= (uint32_t)(((px >> sh) & 255) * a + 0.5f) << sh;
                    tmp->pixels[y * tmp->stride + x] = out;
                }
            }
        }
        {
            struct pen p = { &tg, 0, 0, size, RGB(0xFFFFFF) };

            s->draw(&p);
        }
        gfx_blit(g, tmp, (struct rect){ 0, 0, size, size }, tile.x, tile.y);
        surface_destroy(tmp);
    }
}

// Rendered icons are cached by name and size.
struct cached {
    char name[32];
    int size;
    struct surface *s;
};

static struct cached cache[96];
static int ncache;

struct surface *icon_get(const char *name, int size)
{
    struct surface *s;
    struct gfx g;

    for (int i = 0; i < ncache; i++)
        if (cache[i].size == size && !strcmp(cache[i].name, name))
            return cache[i].s;
    if (!(s = surface_create(size, size)))
        return NULL;
    gfx_init(&g, s);
    icon_draw(&g, name, (struct rect){ 0, 0, size, size });
    if (ncache < (int)(sizeof(cache) / sizeof(cache[0]))) {
        strlcpy(cache[ncache].name, name, sizeof(cache[ncache].name));
        cache[ncache].size = size;
        cache[ncache].s = s;
        ncache++;
    }
    return s;
}

// The icon for a file name, by extension.
const char *icon_for_file(const char *name, bool dir)
{
    static const struct {
        const char *ext, *icon;
    } map[] = {
        { ".txt", "text" }, { ".md", "text" }, { ".log", "text" }, { ".conf", "text" }, { ".aui", "code" },
        { ".c", "code" }, { ".h", "code" }, { ".py", "code" }, { ".sh", "code" }, { ".html", "code" },
        { ".css", "code" }, { ".js", "code" }, { ".json", "code" }, { ".png", "image" }, { ".jpg", "image" },
        { ".jpeg", "image" }, { ".gif", "image" }, { ".bmp", "image" }, { ".tga", "image" },
        { ".wav", "audio" }, { ".mp3", "audio" }, { ".ogg", "audio" }, { ".flac", "audio" },
        { ".aip", "package" },
    };
    const char *dot = strrchr(name, '.');

    if (dir)
        return "folder";
    if (dot) {
        for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
            if (!strcasecmp(dot, map[i].ext))
                return map[i].icon;
    }
    return "file";
}
