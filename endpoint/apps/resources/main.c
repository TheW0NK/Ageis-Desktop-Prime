#include "aegis.h"
#include "ui.h"

// Resource Manager: live graphs of processor, memory, network and disk use.

#define HISTORY 120                 // samples, one per second
#define MAX_CPUS 64

static const char page[] =
    "<window title='Resource Manager' width='860' height='620' padding='14' spacing='10'>"
    "  <grid columns='2' spacing='12' stretch='0,1' expand='1'>"
    "    <group title='Processor' expand='1'>"
    "      <label id='cpu' bold='true'/>"
    "      <canvas id='cpugraph' height='120' expand='0'/>"
    "      <canvas id='cores' height='60' expand='0'/>"
    "    </group>"
    "    <group title='Memory' expand='1'>"
    "      <label id='mem' bold='true'/>"
    "      <canvas id='memgraph' height='120' expand='0'/>"
    "      <label id='memdetail' dim='true'/>"
    "    </group>"
    "    <group title='Network' expand='1'>"
    "      <label id='net' bold='true'/>"
    "      <canvas id='netgraph' height='120' expand='0'/>"
    "      <label id='netdetail' dim='true'/>"
    "    </group>"
    "    <group title='Disks' expand='1'>"
    "      <vbox id='disks' spacing='10'/>"
    "    </group>"
    "  </grid>"
    "  <hbox spacing='8'>"
    "    <label id='uptime' dim='true' align='center'/>"
    "    <spacer/>"
    "    <button text='Open Task Manager' onclick='tasks'/>"
    "  </hbox>"
    "</window>";

struct series {
    double v[HISTORY];
    int len;
};

static struct ui_window *win;
static struct series cpu_hist, mem_hist, rx_hist, tx_hist;
static double core_now[MAX_CPUS];
static int ncpus;
static uint64_t prev_busy[MAX_CPUS], prev_idle[MAX_CPUS], prev_rx, prev_tx, prev_ms;
static bool have_prev;

static void push(struct series *s, double v)
{
    if (s->len == HISTORY) {
        memmove(s->v, s->v + 1, (HISTORY - 1) * sizeof(double));
        s->len--;
    }
    s->v[s->len++] = v;
}

static void grid_lines(struct gfx *g, struct rect r)
{
    color_t c = ALPHA(ui_theme.text, 0x18);

    for (int i = 1; i < 4; i++)
        gfx_fill(g, (struct rect){ r.x, r.y + r.h * i / 4, r.w, 1 }, c);
    for (int i = 1; i < 6; i++)
        gfx_fill(g, (struct rect){ r.x + r.w * i / 6, r.y, 1, r.h }, c);
}

// Plots s (0..max) right-aligned so the newest sample is at the right edge.
static void plot(struct gfx *g, struct rect r, const struct series *s, double max, color_t c, bool fill)
{
    float step = (float)r.w / (HISTORY - 1);

    if (s->len < 2 || max <= 0)
        return;
    for (int i = 1; i < s->len; i++) {
        float x0 = r.x + r.w - (s->len - i) * step, x1 = x0 + step;
        float y0 = r.y + r.h - (float)(MIN(s->v[i - 1], max) / max) * (r.h - 2) - 1;
        float y1 = r.y + r.h - (float)(MIN(s->v[i], max) / max) * (r.h - 2) - 1;

        if (fill) {
            float pts[] = { x0, y0, x1, y1, x1, (float)(r.y + r.h), x0, (float)(r.y + r.h) };

            gfx_polygon(g, pts, 4, ALPHA(c, 0x40));
        }
        gfx_line(g, x0, y0, x1, y1, 2, c);
    }
}

static void frame(struct gfx *g, struct rect r)
{
    gfx_fill_rounded(g, r, 6, ui_theme.input);
    gfx_outline_rounded(g, r, 6, 1, ui_theme.border);
    grid_lines(g, r);
}

static void paint_cpu(struct widget *w, struct gfx *g, struct rect r, void *u)
{
    (void)w;
    (void)u;
    frame(g, r);
    plot(g, r, &cpu_hist, 100, ui_theme.accent, true);
}

static void paint_mem(struct widget *w, struct gfx *g, struct rect r, void *u)
{
    (void)w;
    (void)u;
    frame(g, r);
    plot(g, r, &mem_hist, 100, RGB(0x9C6BFF), true);
}

static double series_max(const struct series *s)
{
    double m = 0;

    for (int i = 0; i < s->len; i++)
        m = MAX(m, s->v[i]);
    return m;
}

static void paint_net(struct widget *w, struct gfx *g, struct rect r, void *u)
{
    double max = MAX(MAX(series_max(&rx_hist), series_max(&tx_hist)) * 1.2, 1024);

    (void)w;
    (void)u;
    frame(g, r);
    plot(g, r, &rx_hist, max, RGB(0x2CC4B0), true);
    plot(g, r, &tx_hist, max, RGB(0xFFA24C), false);
}

static void paint_cores(struct widget *w, struct gfx *g, struct rect r, void *u)
{
    int n = MAX(ncpus, 1), gap = 6, bw = MIN(48, (r.w - gap * (n - 1)) / n);
    struct font *f = font_get(FONT_SANS, 11);

    (void)w;
    (void)u;
    for (int i = 0; i < ncpus; i++) {
        struct rect bar = { r.x + i * (bw + gap), r.y, bw, r.h - 16 };
        int fill = (int)(bar.h * MIN(core_now[i], 100) / 100);
        char label[16];

        gfx_fill_rounded(g, bar, 4, ui_theme.surface_alt);
        if (fill > 0)
            gfx_fill_rounded(g, (struct rect){ bar.x, bar.y + bar.h - fill, bar.w, fill }, 4, ui_theme.accent);
        snprintf(label, sizeof(label), "%d", i);
        text_draw(g, f, bar.x + (bw - text_width(f, label, -1)) / 2, r.y + r.h - 14, label, -1, ui_theme.text_dim);
    }
}

static void rate(double bytes_per_s, char *buf, size_t size)
{
    char s[32];

    ui_format_size((uint64_t)bytes_per_s, s, sizeof(s));
    snprintf(buf, size, "%s/s", s);
}

static void show_disks(void)
{
    static const char *const mounts[] = { "/", "/osystem/boot" };
    struct widget *box = ui_get(win, "disks");
    uint64_t seen_blocks = 0;

    if (ui_attr(box, "filled"))
        return;
    ui_set_attr(box, "filled", "true");
    for (size_t i = 0; i < sizeof(mounts) / sizeof(mounts[0]); i++) {
        struct aegis_statfs fs;
        char line[160], total[32], used[32];
        struct widget *l, *p;

        if (statfs(mounts[i], &fs) < 0 || fs.blocks == seen_blocks)
            continue;
        seen_blocks = fs.blocks;
        ui_format_size(fs.blocks * fs.block_size, total, sizeof(total));
        ui_format_size((fs.blocks - fs.blocks_free) * fs.block_size, used, sizeof(used));
        snprintf(line, sizeof(line), "%s (%s): %s used of %s", mounts[i], fs.fstype, used, total);
        l = ui_create(win, "label");
        ui_set_text(l, line);
        ui_add(box, l);
        p = ui_create(win, "progress");
        ui_set_value(p, fs.blocks ? 100.0 * (fs.blocks - fs.blocks_free) / fs.blocks : 0);
        ui_add(box, p);
    }
}

static bool tick(void *u)
{
    struct aegis_sysinfo si;
    struct aegis_netif nif;
    uint64_t rx = 0, tx = 0, now = uptime_ms();
    char buf[160], a[32], b[32];

    (void)u;
    if (sysinfo(&si) < 0)
        return true;
    ncpus = MIN((int)si.cpus, MAX_CPUS);
    for (int i = 0; netconfig(NETCONFIG_GET, i, &nif) == 0; i++) {
        rx += nif.rx_bytes;
        tx += nif.tx_bytes;
    }
    if (have_prev) {
        double total = 0, dt = MAX(now - prev_ms, 1) / 1000.0;

        for (int i = 0; i < ncpus; i++) {
            uint64_t busy = si.cpu_busy_ms[i] - prev_busy[i], idle = si.cpu_idle_ms[i] - prev_idle[i];

            core_now[i] = busy + idle ? 100.0 * busy / (busy + idle) : 0;
            total += core_now[i];
        }
        push(&cpu_hist, total / MAX(ncpus, 1));
        push(&rx_hist, (rx - prev_rx) / dt);
        push(&tx_hist, (tx - prev_tx) / dt);
        snprintf(buf, sizeof(buf), "%.0f%% in use on %d processors", total / MAX(ncpus, 1), ncpus);
        ui_set_text(ui_get(win, "cpu"), buf);
        rate((rx - prev_rx) / dt, a, sizeof(a));
        rate((tx - prev_tx) / dt, b, sizeof(b));
        snprintf(buf, sizeof(buf), "Receiving %s, sending %s", a, b);
        ui_set_text(ui_get(win, "net"), buf);
    }
    for (int i = 0; i < ncpus; i++) {
        prev_busy[i] = si.cpu_busy_ms[i];
        prev_idle[i] = si.cpu_idle_ms[i];
    }
    prev_rx = rx;
    prev_tx = tx;
    prev_ms = now;
    have_prev = true;

    push(&mem_hist, 100.0 * (si.memory_total - si.memory_free) / MAX(si.memory_total, 1));
    ui_format_size(si.memory_total - si.memory_free, a, sizeof(a));
    ui_format_size(si.memory_total, b, sizeof(b));
    snprintf(buf, sizeof(buf), "%s of %s in use (%.0f%%)", a, b, mem_hist.v[mem_hist.len - 1]);
    ui_set_text(ui_get(win, "mem"), buf);
    ui_format_size(si.memory_free, a, sizeof(a));
    snprintf(buf, sizeof(buf), "%s free, %u threads, %u strands", a, si.processes, si.threads);
    ui_set_text(ui_get(win, "memdetail"), buf);
    ui_format_size(rx, a, sizeof(a));
    ui_format_size(tx, b, sizeof(b));
    snprintf(buf, sizeof(buf), "%s received, %s sent since start", a, b);
    ui_set_text(ui_get(win, "netdetail"), buf);
    {
        uint64_t s = si.uptime_ms / 1000;

        snprintf(buf, sizeof(buf), "Running for %lu days, %lu h %02lu min", (unsigned long)(s / 86400),
                 (unsigned long)(s / 3600 % 24), (unsigned long)(s / 60 % 60));
        ui_set_text(ui_get(win, "uptime"), buf);
    }
    show_disks();
    ui_redraw(ui_get(win, "cpugraph"));
    ui_redraw(ui_get(win, "memgraph"));
    ui_redraw(ui_get(win, "netgraph"));
    ui_redraw(ui_get(win, "cores"));
    return true;
}

static void open_tasks(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    launch("/sysapps/taskmanager", NULL);
}

int main(void)
{
    static const struct ui_handler_entry handlers[] = { { "tasks", open_tasks }, { NULL, NULL } };

    ui_load_user_theme();
    if (!(win = ui_load_string_named(page, handlers, NULL, "resources")))
        return 1;
    ui_canvas_set(ui_get(win, "cpugraph"), paint_cpu, NULL, NULL);
    ui_canvas_set(ui_get(win, "memgraph"), paint_mem, NULL, NULL);
    ui_canvas_set(ui_get(win, "netgraph"), paint_net, NULL, NULL);
    ui_canvas_set(ui_get(win, "cores"), paint_cores, NULL, NULL);
    tick(NULL);
    ui_timer(1000, tick, NULL);
    ui_window_show(win);
    return ui_run();
}
