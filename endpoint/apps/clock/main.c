#include "aegis.h"
#include "ui.h"
#include <math.h>

// Clock: the time here and elsewhere, a countdown timer and a stopwatch.

static const char page[] =
    "<window title='Clock' width='520' height='560' padding='0'>"
    "  <tabs id='tabs' expand='1'>"
    "    <tab title='Clock'>"
    "      <canvas id='face' expand='0' height='260'/>"
    "      <label id='time' textalign='center' size='huge' bold='true'/>"
    "      <label id='date' textalign='center' dim='true'/>"
    "      <h2 text='World'/>"
    "      <table id='world' expand='1' columns='City|Time:90:right|Difference:110:right'/>"
    "    </tab>"
    "    <tab title='Timer'>"
    "      <canvas id='ring' expand='0' height='240'/>"
    "      <hbox justify='center' spacing='8'>"
    "        <spin id='hours' width='80' min='0' max='99' value='0'/><label text='h' align='center'/>"
    "        <spin id='minutes' width='80' min='0' max='59' value='5'/><label text='min' align='center'/>"
    "        <spin id='seconds' width='80' min='0' max='59' value='0'/><label text='s' align='center'/>"
    "      </hbox>"
    "      <hbox justify='center' spacing='8'>"
    "        <button id='tstart' text='Start' primary='true' onclick='tstart'/>"
    "        <button text='Reset' onclick='treset'/>"
    "      </hbox>"
    "      <hbox justify='center' spacing='6'>"
    "        <button flat='true' text='1 min' onclick='preset'/><button flat='true' text='3 min' onclick='preset'/>"
    "        <button flat='true' text='5 min' onclick='preset'/><button flat='true' text='10 min' onclick='preset'/>"
    "        <button flat='true' text='25 min' onclick='preset'/>"
    "      </hbox>"
    "    </tab>"
    "    <tab title='Stopwatch'>"
    "      <label id='elapsed' textalign='center' size='huge' bold='true' mono='true' text='00:00.00'/>"
    "      <hbox justify='center' spacing='8'>"
    "        <button id='sstart' text='Start' primary='true' onclick='sstart'/>"
    "        <button id='slap' text='Lap' onclick='slap'/>"
    "        <button text='Reset' onclick='sreset'/>"
    "      </hbox>"
    "      <table id='laps' expand='1' columns='Lap:60|Lap time:140:right|Total:140:right'/>"
    "    </tab>"
    "  </tabs>"
    "</window>";

static const struct {
    const char *city;
    int minutes;
} cities[] = {
    { "London", 0 }, { "Paris", 60 }, { "Berlin", 60 }, { "Madrid", 60 }, { "Moscow", 180 }, { "Dubai", 240 },
    { "Delhi", 330 }, { "Beijing", 480 }, { "Tokyo", 540 }, { "Sydney", 600 }, { "New York", -300 },
    { "Chicago", -360 }, { "Los Angeles", -480 }, { "S\xC3\xA3o Paulo", -180 },
};

static struct ui_window *win;
// Timer
static bool timer_running;
static uint64_t timer_end, timer_total, timer_left;
// Stopwatch
static bool sw_running;
static uint64_t sw_start, sw_accum, last_lap;
static int laps;

static uint64_t sw_elapsed(void)
{
    return sw_accum + (sw_running ? uptime_ms() - sw_start : 0);
}

static void format_ms(uint64_t ms, char *buf, size_t size, bool centis)
{
    uint64_t s = ms / 1000;

    if (s >= 3600)
        snprintf(buf, size, "%lu:%02lu:%02lu", (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60),
                 (unsigned long)(s % 60));
    else if (centis)
        snprintf(buf, size, "%02lu:%02lu.%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60),
                 (unsigned long)(ms / 10 % 100));
    else
        snprintf(buf, size, "%02lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
}

static void paint_face(struct widget *w, struct gfx *g, struct rect r, void *u)
{
    int64_t now = time(NULL);
    struct tm tm;
    float cx = r.x + r.w / 2.0f, cy = r.y + r.h / 2.0f, rad = MIN(r.w, r.h) / 2.0f - 12;
    float ms = (uptime_ms() % 1000) / 1000.0f;

    (void)w;
    (void)u;
    localtime_r(&now, &tm);
    gfx_circle(g, cx, cy, rad, ui_theme.surface);
    gfx_ring(g, cx, cy, rad, 2, ui_theme.border);
    for (int i = 0; i < 60; i++) {
        float a = i * 6.2831853f / 60, inner = rad - (i % 5 ? 6 : 14);

        gfx_line(g, cx + sinf(a) * inner, cy - cosf(a) * inner, cx + sinf(a) * (rad - 3), cy - cosf(a) * (rad - 3),
                 i % 5 ? 1 : 3, i % 5 ? ui_theme.text_dim : ui_theme.text);
    }
    {
        float h = (tm.tm_hour % 12 + tm.tm_min / 60.0f) * 6.2831853f / 12;
        float m = (tm.tm_min + tm.tm_sec / 60.0f) * 6.2831853f / 60;
        float s = (tm.tm_sec + ms * 0) * 6.2831853f / 60;

        (void)ms;
        gfx_line(g, cx, cy, cx + sinf(h) * rad * 0.5f, cy - cosf(h) * rad * 0.5f, 6, ui_theme.text);
        gfx_line(g, cx, cy, cx + sinf(m) * rad * 0.78f, cy - cosf(m) * rad * 0.78f, 4, ui_theme.text);
        gfx_line(g, cx - sinf(s) * 14, cy + cosf(s) * 14, cx + sinf(s) * rad * 0.85f, cy - cosf(s) * rad * 0.85f, 2,
                 ui_theme.danger);
        gfx_circle(g, cx, cy, 5, ui_theme.danger);
    }
}

static void paint_ring(struct widget *w, struct gfx *g, struct rect r, void *u)
{
    float cx = r.x + r.w / 2.0f, cy = r.y + r.h / 2.0f, rad = MIN(r.w, r.h) / 2.0f - 16;
    uint64_t left = timer_running ? (timer_end > uptime_ms() ? timer_end - uptime_ms() : 0) : timer_left;
    float frac = timer_total ? (float)left / timer_total : 0;
    char buf[32];
    struct font *f = font_get(FONT_SANS_BOLD, 40);

    (void)w;
    (void)u;
    gfx_ring(g, cx, cy, rad, 10, ui_theme.surface_alt);
    // The remaining time as an arc, drawn in short segments.
    for (int i = 0; i < 120 && frac > 0; i++) {
        float a0 = i / 120.0f, a1 = (i + 1) / 120.0f;

        if (a0 >= frac)
            break;
        a1 = fminf(a1, frac);
        gfx_line(g, cx + sinf(a0 * 6.2831853f) * rad, cy - cosf(a0 * 6.2831853f) * rad,
                 cx + sinf(a1 * 6.2831853f) * rad, cy - cosf(a1 * 6.2831853f) * rad, 10, ui_theme.accent);
    }
    format_ms(left + 999, buf, sizeof(buf), false);
    text_draw(g, f, (int)(cx - text_width(f, buf, -1) / 2.0f), (int)(cy - font_line_height(f) / 2.0f), buf, -1,
              ui_theme.text);
}

static void show_world(void)
{
    struct widget *t = ui_get(win, "world");
    int64_t now = time(NULL);
    int here = timezone_offset() / 60;

    for (size_t i = 0; i < sizeof(cities) / sizeof(cities[0]); i++) {
        int64_t local = now + cities[i].minutes * 60;
        struct tm tm;
        char row[128], when[16], diff[32];
        int d = cities[i].minutes - here;

        gmtime_r(&local, &tm);
        strftime(when, sizeof(when), "%H:%M", &tm);
        if (!d)
            strlcpy(diff, "Same time", sizeof(diff));
        else
            snprintf(diff, sizeof(diff), "%+d h%s", d / 60, d % 60 ? " 30" : "");
        snprintf(row, sizeof(row), "%s\t%s\t%s", cities[i].city, when, diff);
        if ((int)i < ui_list_count(t))
            ui_list_set_item(t, i, row);
        else
            ui_list_add(t, row);
    }
}

static void timer_done(void)
{
    timer_running = false;
    timer_left = 0;
    ui_set_text(ui_get(win, "tstart"), "Start");
    ui_redraw(ui_get(win, "ring"));
    ui_message(win, "Timer", "Time is up.", "OK");
}

static bool tick(void *u)
{
    int64_t now = time(NULL);
    struct tm tm;
    char buf[64];

    (void)u;
    localtime_r(&now, &tm);
    strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
    ui_set_text(ui_get(win, "time"), buf);
    strftime(buf, sizeof(buf), "%A, %B %e %Y", &tm);
    ui_set_text(ui_get(win, "date"), buf);
    ui_redraw(ui_get(win, "face"));
    if (tm.tm_sec == 0)
        show_world();
    if (timer_running) {
        ui_redraw(ui_get(win, "ring"));
        if (uptime_ms() >= timer_end)
            timer_done();
    }
    return true;
}

static bool fast_tick(void *u)
{
    char buf[32];

    (void)u;
    if (!sw_running)
        return true;
    format_ms(sw_elapsed(), buf, sizeof(buf), true);
    ui_set_text(ui_get(win, "elapsed"), buf);
    return true;
}

static void on_tstart(struct widget *w, void *u)
{
    (void)u;
    if (timer_running) {
        timer_left = timer_end > uptime_ms() ? timer_end - uptime_ms() : 0;
        timer_running = false;
        ui_set_text(w, "Resume");
        return;
    }
    if (!timer_left) {
        timer_total = ((uint64_t)ui_value(ui_get(win, "hours")) * 3600 + (uint64_t)ui_value(ui_get(win, "minutes")) * 60
                       + (uint64_t)ui_value(ui_get(win, "seconds"))) * 1000;
        timer_left = timer_total;
    }
    if (!timer_left)
        return;
    timer_end = uptime_ms() + timer_left;
    timer_running = true;
    ui_set_text(w, "Pause");
    ui_redraw(ui_get(win, "ring"));
}

static void on_treset(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    timer_running = false;
    timer_left = timer_total = 0;
    ui_set_text(ui_get(win, "tstart"), "Start");
    ui_redraw(ui_get(win, "ring"));
}

static void on_preset(struct widget *w, void *u)
{
    int m = atoi(ui_text(w));

    (void)u;
    on_treset(NULL, NULL);
    ui_set_value(ui_get(win, "hours"), 0);
    ui_set_value(ui_get(win, "minutes"), m);
    ui_set_value(ui_get(win, "seconds"), 0);
    on_tstart(ui_get(win, "tstart"), NULL);
}

static void on_sstart(struct widget *w, void *u)
{
    (void)u;
    if (sw_running) {
        sw_accum = sw_elapsed();
        sw_running = false;
        ui_set_text(w, "Resume");
    } else {
        sw_start = uptime_ms();
        sw_running = true;
        ui_set_text(w, "Stop");
    }
}

static void on_slap(struct widget *w, void *u)
{
    uint64_t total = sw_elapsed();
    char a[32], b[32], row[96];

    (void)w;
    (void)u;
    if (!sw_running)
        return;
    format_ms(total - last_lap, a, sizeof(a), true);
    format_ms(total, b, sizeof(b), true);
    snprintf(row, sizeof(row), "%d\t%s\t%s", ++laps, a, b);
    ui_list_add(ui_get(win, "laps"), row);
    ui_list_select(ui_get(win, "laps"), laps - 1);
    last_lap = total;
}

static void on_sreset(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    sw_running = false;
    sw_accum = last_lap = 0;
    laps = 0;
    ui_list_clear(ui_get(win, "laps"));
    ui_set_text(ui_get(win, "elapsed"), "00:00.00");
    ui_set_text(ui_get(win, "sstart"), "Start");
}

int main(int argc, char **argv)
{
    static const struct ui_handler_entry handlers[] = {
        { "tstart", on_tstart }, { "treset", on_treset }, { "preset", on_preset }, { "sstart", on_sstart },
        { "slap", on_slap }, { "sreset", on_sreset }, { NULL, NULL },
    };

    ui_load_user_theme();
    if (!(win = ui_load_string_named(page, handlers, NULL, "clock")))
        return 1;
    ui_canvas_set(ui_get(win, "face"), paint_face, NULL, NULL);
    ui_canvas_set(ui_get(win, "ring"), paint_ring, NULL, NULL);
    if (argc > 1)
        ui_set_value(ui_get(win, "tabs"), !strcmp(argv[1], "timer") ? 1 : !strcmp(argv[1], "stopwatch") ? 2 : 0);
    show_world();
    tick(NULL);
    ui_timer(1000, tick, NULL);
    ui_timer(50, fast_tick, NULL);
    ui_window_show(win);
    return ui_run();
}
