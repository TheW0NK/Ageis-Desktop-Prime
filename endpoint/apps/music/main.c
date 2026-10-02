#include "aegis.h"
#include "sound.h"
#include "ui.h"
#include <math.h>

// Audio Player: a playlist of WAV, Ogg Vorbis and MP3 files. A thread
// decodes the current track and streams it to the audio server; the window
// polls its progress.

static const char window_aui[] =
    "<window title='Audio Player' width='780' height='560' padding='0' spacing='0'>"
    "  <toolbar>"
    "    <button flat='true' text='Add files...' onclick='addfiles' shortcut='Ctrl+O'/>"
    "    <button flat='true' text='Add folder...' onclick='addfolder'/>"
    "    <button flat='true' text='Remove' onclick='remove' shortcut='Delete'/>"
    "    <button flat='true' text='Clear' onclick='clear'/>"
    "    <spacer/>"
    "    <checkbox id='shuffle' text='Shuffle'/>"
    "    <checkbox id='repeat' text='Repeat' checked='true'/>"
    "  </toolbar>"
    "  <hbox padding='16' spacing='16'>"
    "    <canvas id='art' width='132' height='132'/>"
    "    <vbox expand='true' spacing='4' justify='center'>"
    "      <label id='title' size='title' bold='true' text='Nothing playing'/>"
    "      <label id='artist' dim='true'/>"
    "      <label id='format' dim='true' size='small'/>"
    "    </vbox>"
    "  </hbox>"
    "  <hbox padding='0' spacing='10'>"
    "    <spacer size='16'/><label id='elapsed' text='0:00' width='48'/>"
    "    <slider id='seek' expand='true' min='0' max='1000' value='0' onchange='seeking' onrelease='seek'/>"
    "    <label id='total' text='0:00' width='48' textalign='right'/><spacer size='16'/>"
    "  </hbox>"
    "  <hbox padding='10' spacing='8' justify='center'>"
    "    <button symbol='back' onclick='prev' shortcut='Ctrl+Left'/>"
    "    <button id='play' symbol='play' default='true' onclick='play' shortcut='Space'/>"
    "    <button symbol='stop' onclick='stop'/>"
    "    <button symbol='forward' onclick='next' shortcut='Ctrl+Right'/>"
    "    <spacer size='30'/>"
    "    <label text='Volume'/><slider id='volume' width='160' min='0' max='100' value='80' onchange='volume'/>"
    "  </hbox>"
    "  <table id='playlist' expand='true' columns='Title|Artist:160|Format:90|Length:70:right' onactivate='pick'/>"
    "  <statusbar><label id='status' expand='true'/></statusbar>"
    "</window>";

struct track {
    char path[512], title[128], artist[128], format[16];
    double length;
};

static struct ui_window *win;
static struct track *tracks;
static int ntracks, current = -1;

// Shared with the player thread.
static mutex_t lock;
static struct {
    bool active, paused, stop, finished;
    double seek_to;                 // >= 0: seek there
    double position, length;
    float levels[48];               // recent loudness, for the picture
    int level_at;
    char error[200];
} st;
static thread_t player;
static bool player_running, seeking;

static void format_time(double s, char *out, size_t size)
{
    int t = (int)(s + 0.5);

    snprintf(out, size, "%d:%02d", t / 60, t % 60);
}

// ---- Playing ----

static void *play_thread(void *arg)
{
    char *path = arg, error[200];
    struct sound *s = sound_open(path, error, sizeof(error));
    int16_t buf[2048 * 8];
    int fd = -1;

    if (!s) {
        mutex_lock(&lock);
        strlcpy(st.error, error, sizeof(st.error));
        st.finished = true;
        mutex_unlock(&lock);
        free(path);
        return NULL;
    }
    mutex_lock(&lock);
    st.length = sound_length(s);
    mutex_unlock(&lock);
    for (;;) {
        bool paused, stop;
        double seek;
        size_t n;

        mutex_lock(&lock);
        paused = st.paused;
        stop = st.stop;
        seek = st.seek_to;
        st.seek_to = -1;
        mutex_unlock(&lock);
        if (stop)
            break;
        if (seek >= 0) {
            // Drop what the server still holds, then go there.
            if (fd >= 0)
                close(fd);
            fd = -1;
            sound_seek(s, seek);
        }
        if (paused) {
            if (fd >= 0) {
                close(fd);
                fd = -1;
            }
            msleep(50);
            continue;
        }
        if (fd < 0 && (fd = audio_open(sound_title(s)[0] ? sound_title(s) : "Audio Player", sound_rate(s),
                                       sound_channels(s))) < 0) {
            mutex_lock(&lock);
            strlcpy(st.error, "The audio server is not running.", sizeof(st.error));
            st.finished = true;
            mutex_unlock(&lock);
            break;
        }
        if (!(n = sound_read(s, buf, 2048 / (sound_channels(s) > 2 ? 4 : 1)))) {
            mutex_lock(&lock);
            st.finished = true;
            mutex_unlock(&lock);
            break;
        }
        {
            // Loudness of this block.
            double sum = 0;
            size_t count = n * sound_channels(s);

            for (size_t i = 0; i < count; i++)
                sum += (double)buf[i] * buf[i];
            mutex_lock(&lock);
            st.levels[st.level_at] = (float)sqrt(sum / count) / 12000.0f;
            st.level_at = (st.level_at + 1) % 48;
            // What is audible lags the decoder by the buffers on the way.
            st.position = MAX(sound_position(s) - 0.35, 0);
            mutex_unlock(&lock);
        }
        if (audio_write(fd, buf, n * sound_channels(s) * 2) < 0) {
            close(fd);
            fd = -1;
        }
    }
    if (fd >= 0)
        close(fd);
    sound_close(s);
    free(path);
    return NULL;
}

static void stop_player(void)
{
    if (!player_running)
        return;
    mutex_lock(&lock);
    st.stop = true;
    mutex_unlock(&lock);
    thread_join(player, NULL);
    player_running = false;
}

static void update_now_playing(void);

static void play_index(int i)
{
    char *path;

    stop_player();
    if (i < 0 || i >= ntracks)
        return;
    current = i;
    memset(&st, 0, sizeof(st));
    st.seek_to = -1;
    st.active = true;
    if (!(path = strdup(tracks[i].path)) || thread_create(&player, play_thread, path) < 0) {
        free(path);
        st.active = false;
        return;
    }
    player_running = true;
    ui_list_select(ui_get(win, "playlist"), i);
    update_now_playing();
}

static int pick_next(int dir)
{
    if (!ntracks)
        return -1;
    if (ui_value(ui_get(win, "shuffle")) && ntracks > 1) {
        int n;

        do
            n = (int)(time(NULL) * 7919 + uptime_ms()) % ntracks;
        while (n == current);
        return n;
    }
    if (current + dir >= ntracks)
        return ui_value(ui_get(win, "repeat")) ? 0 : -1;
    if (current + dir < 0)
        return ntracks - 1;
    return current + dir;
}

// ---- The window ----

static void update_now_playing(void)
{
    char total[16], line[200];

    if (current < 0 || current >= ntracks) {
        ui_set_text(ui_get(win, "title"), "Nothing playing");
        ui_set_text(ui_get(win, "artist"), "");
        ui_set_text(ui_get(win, "format"), "");
        return;
    }
    ui_set_text(ui_get(win, "title"), tracks[current].title);
    ui_set_text(ui_get(win, "artist"), tracks[current].artist[0] ? tracks[current].artist : "Unknown artist");
    snprintf(line, sizeof(line), "%s  \xC2\xB7  %s", tracks[current].format, tracks[current].path);
    ui_set_text(ui_get(win, "format"), line);
    format_time(tracks[current].length, total, sizeof(total));
    ui_set_text(ui_get(win, "total"), total);
    ui_set_attr(ui_get(win, "play"), "symbol", st.paused ? "play" : "pause");
    ui_window_set_title(win, tracks[current].title);
}

static bool tick(void *u)
{
    bool finished, active;
    double pos, len;
    char err[200], elapsed[16];

    (void)u;
    mutex_lock(&lock);
    finished = st.finished;
    active = st.active;
    pos = st.position;
    len = st.length;
    strlcpy(err, st.error, sizeof(err));
    st.error[0] = 0;
    mutex_unlock(&lock);
    if (*err)
        ui_set_text(ui_get(win, "status"), err);
    if (finished && player_running) {
        int next;

        stop_player();
        st.active = false;
        next = *err ? -1 : pick_next(1);
        if (next >= 0)
            play_index(next);
        else {
            ui_set_attr(ui_get(win, "play"), "symbol", "play");
            if (!*err)
                ui_set_text(ui_get(win, "status"), "Finished.");
        }
        return true;
    }
    if (active && !seeking) {
        format_time(pos, elapsed, sizeof(elapsed));
        ui_set_text(ui_get(win, "elapsed"), elapsed);
        ui_set_value(ui_get(win, "seek"), len > 0 ? pos / len * 1000 : 0);
    }
    ui_redraw(ui_get(win, "art"));
    return true;
}

static void paint_art(struct widget *w, struct gfx *g, struct rect r, void *u)
{
    float levels[48];
    int at;

    (void)w;
    (void)u;
    mutex_lock(&lock);
    memcpy(levels, st.levels, sizeof(levels));
    at = st.level_at;
    mutex_unlock(&lock);
    gfx_fill_rounded(g, r, 12, RGB(0x2A2350));
    gfx_gradient(g, (struct rect){ r.x, r.y + r.h / 2, r.w, r.h / 2 }, ALPHA(0x7B5CFF, 0), ALPHA(0xFF5C8A, 90));
    if (!player_running) {
        icon_draw_glyph(g, "audio", (struct rect){ r.x + r.w / 2 - 28, r.y + r.h / 2 - 28, 56, 56 }, RGB(0xFFFFFF));
        return;
    }
    // The last 24 loudness readings as bars.
    for (int i = 0; i < 24; i++) {
        float v = levels[(at + 48 - 24 + i) % 48];
        int h = (int)(MIN(v, 1.0f) * (r.h - 24)) + 3, x = r.x + 10 + i * (r.w - 20) / 24;

        gfx_fill_rounded(g, (struct rect){ x, r.y + r.h - 12 - h, (r.w - 20) / 24 - 2, h }, 2,
                         ALPHA(0xFFFFFF, 140 + i * 4));
    }
}

static void playlist_row(int i, char *out, size_t size)
{
    char len[16] = "";

    if (tracks[i].length > 0)
        format_time(tracks[i].length, len, sizeof(len));
    snprintf(out, size, "%s\t%s\t%s\t%s", tracks[i].title, tracks[i].artist, tracks[i].format, len);
}

static bool add_track(const char *path)
{
    char error[200];
    struct sound *s = sound_open(path, error, sizeof(error));
    struct track *t, *m;
    char row[512];

    if (!s)
        return false;
    if (!(m = realloc(tracks, (ntracks + 1) * sizeof(*m)))) {
        sound_close(s);
        return false;
    }
    tracks = m;
    t = &tracks[ntracks];
    memset(t, 0, sizeof(*t));
    strlcpy(t->path, path, sizeof(t->path));
    if (*sound_title(s)) {
        strlcpy(t->title, sound_title(s), sizeof(t->title));
    } else {
        const char *base = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
        char *dot;

        strlcpy(t->title, base, sizeof(t->title));
        if ((dot = strrchr(t->title, '.')))
            *dot = 0;
    }
    strlcpy(t->artist, sound_artist(s), sizeof(t->artist));
    strlcpy(t->format, sound_format(s), sizeof(t->format));
    t->length = sound_length(s);
    sound_close(s);
    ntracks++;
    playlist_row(ntracks - 1, row, sizeof(row));
    ui_list_add(ui_get(win, "playlist"), row);
    return true;
}

static bool is_audio(const char *name)
{
    const char *dot = strrchr(name, '.');

    return dot && (!strcasecmp(dot, ".wav") || !strcasecmp(dot, ".ogg") || !strcasecmp(dot, ".oga")
                   || !strcasecmp(dot, ".mp3"));
}

static int by_name(const void *a, const void *b)
{
    return strcasecmp(*(char *const *)a, *(char *const *)b);
}

static int add_folder(const char *dir)
{
    struct dir_stream *d = opendir(dir);
    struct aegis_dirent *e;
    char *names[512];
    int n = 0, added = 0;

    if (!d)
        return 0;
    while ((e = readdir(d)) && n < 512)
        if (e->name[0] != '.' && is_audio(e->name))
            names[n++] = strdup(e->name);
    closedir(d);
    qsort(names, n, sizeof(char *), by_name);
    for (int i = 0; i < n; i++) {
        char path[600];

        snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
        added += add_track(path);
        free(names[i]);
    }
    return added;
}

static void status_count(void)
{
    char line[64];

    snprintf(line, sizeof(line), "%d track%s", ntracks, ntracks == 1 ? "" : "s");
    ui_set_text(ui_get(win, "status"), line);
}

// ---- Handlers ----

static void on_play(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    if (!player_running) {
        int i = ui_list_selected(ui_get(win, "playlist"));

        play_index(i >= 0 ? i : current >= 0 ? current : 0);
        return;
    }
    mutex_lock(&lock);
    st.paused = !st.paused;
    mutex_unlock(&lock);
    ui_set_attr(ui_get(win, "play"), "symbol", st.paused ? "play" : "pause");
    ui_set_text(ui_get(win, "status"), st.paused ? "Paused." : "Playing.");
}

static void on_stop(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    stop_player();
    st.active = false;
    ui_set_attr(ui_get(win, "play"), "symbol", "play");
    ui_set_text(ui_get(win, "elapsed"), "0:00");
    ui_set_value(ui_get(win, "seek"), 0);
    ui_redraw(ui_get(win, "art"));
}

static void on_next(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    play_index(pick_next(1));
}

static void on_prev(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    // Back to the start first, like most players.
    if (player_running && st.position > 3) {
        mutex_lock(&lock);
        st.seek_to = 0;
        mutex_unlock(&lock);
        return;
    }
    play_index(pick_next(-1));
}

static void on_pick(struct widget *w, void *u)
{
    (void)u;
    play_index(ui_list_selected(w));
}

static void on_seeking(struct widget *w, void *u)
{
    char elapsed[16];

    (void)u;
    seeking = true;
    format_time(ui_value(w) / 1000 * st.length, elapsed, sizeof(elapsed));
    ui_set_text(ui_get(win, "elapsed"), elapsed);
}

static void on_seek(struct widget *w, void *u)
{
    (void)u;
    seeking = false;
    if (!player_running)
        return;
    mutex_lock(&lock);
    st.seek_to = ui_value(w) / 1000 * st.length;
    st.position = st.seek_to;
    mutex_unlock(&lock);
}

static void on_volume(struct widget *w, void *u)
{
    (void)u;
    audio_set_volume((int)ui_value(w));
}

static void on_addfiles(struct widget *w, void *u)
{
    char *path = ui_file_dialog_filtered(win, "Add music", NULL, false, NULL, "*.wav;*.ogg;*.mp3");

    (void)w;
    (void)u;
    if (path) {
        if (!add_track(path))
            ui_message(win, "Audio Player", "That file could not be read as music.", "OK");
        free(path);
        status_count();
    }
}

static void on_addfolder(struct widget *w, void *u)
{
    char *path = ui_file_dialog(win, "Add a folder of music (pick any file in it)", NULL, false, NULL);
    char *slash;

    (void)w;
    (void)u;
    if (!path)
        return;
    if ((slash = strrchr(path, '/')))
        *slash = 0;
    add_folder(*path ? path : "/");
    free(path);
    status_count();
}

static void on_remove(struct widget *w, void *u)
{
    int i = ui_list_selected(ui_get(win, "playlist"));

    (void)w;
    (void)u;
    if (i < 0 || i >= ntracks)
        return;
    if (i == current)
        on_stop(NULL, NULL);
    memmove(&tracks[i], &tracks[i + 1], (ntracks - i - 1) * sizeof(*tracks));
    ntracks--;
    if (current > i)
        current--;
    else if (current == i)
        current = -1;
    ui_list_remove(ui_get(win, "playlist"), i);
    status_count();
}

static void on_clear(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    on_stop(NULL, NULL);
    free(tracks);
    tracks = NULL;
    ntracks = 0;
    current = -1;
    ui_list_clear(ui_get(win, "playlist"));
    update_now_playing();
    status_count();
}

static bool on_close(struct ui_window *w, void *u)
{
    (void)w;
    (void)u;
    stop_player();
    return true;
}

int main(int argc, char **argv)
{
    static const struct ui_handler_entry handlers[] = {
        { "play", on_play }, { "stop", on_stop }, { "next", on_next }, { "prev", on_prev }, { "pick", on_pick },
        { "seeking", on_seeking }, { "seek", on_seek }, { "volume", on_volume }, { "addfiles", on_addfiles },
        { "addfolder", on_addfolder }, { "remove", on_remove }, { "clear", on_clear }, { NULL, NULL },
    };
    int v;

    ui_load_user_theme();
    if (!(win = ui_load_string_named(window_aui, handlers, NULL, "music")))
        return 1;
    ui_canvas_set(ui_get(win, "art"), paint_art, NULL, NULL);
    ui_on_close(win, on_close, NULL);
    if ((v = audio_get_volume(NULL)) >= 0)
        ui_set_value(ui_get(win, "volume"), v);
    if (argc > 1) {
        for (int i = 1; i < argc; i++)
            add_track(argv[i]);
    } else {
        struct user_info me;
        char dir[300];

        if (user_current(&me) == 0) {
            user_path(&me, "home/Music", dir, sizeof(dir));
            add_folder(dir);
        }
    }
    status_count();
    ui_window_show(win);
    ui_timer(200, tick, NULL);
    if (argc > 1 && ntracks)
        play_index(0);
    return ui_run();
}
