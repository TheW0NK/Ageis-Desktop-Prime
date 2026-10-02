#include "mail.h"
#include "ui.h"

// Email: folders on the left, messages and the open message on the right.
// Talking to the servers happens on a worker thread; finished jobs come
// back to the window through a pipe.

static const char window_aui[] =
    "<window title='Email' width='1120' height='740' padding='0' spacing='0'>"
    "  <toolbar>"
    "    <button id='getmail' flat='true' symbol='restart' text='Get mail' onclick='getmail' shortcut='F5'/>"
    "    <button flat='true' text='Write' onclick='write' shortcut='Ctrl+N'/>"
    "    <separator/>"
    "    <button id='reply' flat='true' text='Reply' onclick='reply' shortcut='Ctrl+R'/>"
    "    <button id='replyall' flat='true' text='Reply all' onclick='replyall' shortcut='Ctrl+Shift+R'/>"
    "    <button id='forward' flat='true' text='Forward' onclick='forward' shortcut='Ctrl+L'/>"
    "    <button id='delete' flat='true' text='Delete' onclick='delete' shortcut='Delete'/>"
    "    <spacer/>"
    "    <button flat='true' text='Account...' onclick='account'/>"
    "  </toolbar>"
    "  <hbox expand='true' spacing='0'>"
    "    <list id='folders' width='200' onselect='folder'/>"
    "    <separator/>"
    "    <vbox expand='true' spacing='0'>"
    "      <table id='messages' height='250' columns='From:240|Subject|Date:150' onselect='show'/>"
    "      <separator/>"
    "      <vbox id='header' padding='12' spacing='4'>"
    "        <label id='subject' size='large' bold='true' text=' '/>"
    "        <label id='from' dim='true'/>"
    "        <label id='to' dim='true'/>"
    "        <hbox id='attbar' spacing='8' hidden='true'>"
    "          <label text='Attachments:'/><dropdown id='attachments' width='280'/>"
    "          <button text='Save...' onclick='saveatt'/>"
    "        </hbox>"
    "      </vbox>"
    "      <textarea id='body' readonly='true' wrap='true' expand='true'/>"
    "    </vbox>"
    "  </hbox>"
    "  <statusbar><label id='status' expand='true'/></statusbar>"
    "</window>";

static const char account_aui[] =
    "<window title='Email account' width='520' role='dialog' padding='16' spacing='10'>"
    "  <grid columns='2' spacing='8'>"
    "    <label text='Your name'/><input id='name'/>"
    "    <label text='Email address'/><input id='email' placeholder='you@example.com'/>"
    "    <label text='Sign-in name'/><input id='user' placeholder='Usually the email address'/>"
    "    <label text='Password'/><password id='password'/>"
    "    <label/><checkbox id='remember' text='Remember the password' checked='true'/>"
    "    <label text='Incoming (IMAP)'/><hbox spacing='6'><input id='imap_host' expand='true'"
    "      placeholder='imap.example.com'/><input id='imap_port' width='60'/>"
    "      <dropdown id='imap_sec' width='110'><option>None</option><option>SSL/TLS</option>"
    "      <option>STARTTLS</option></dropdown></hbox>"
    "    <label text='Outgoing (SMTP)'/><hbox spacing='6'><input id='smtp_host' expand='true'"
    "      placeholder='smtp.example.com'/><input id='smtp_port' width='60'/>"
    "      <dropdown id='smtp_sec' width='110'><option>None</option><option>SSL/TLS</option>"
    "      <option>STARTTLS</option></dropdown></hbox>"
    "    <label text='Trust certificates'/><input id='ca_file' placeholder='Only for private servers (a .pem file)'/>"
    "  </grid>"
    "  <label id='note' dim='true' wrap='true'/>"
    "  <hbox spacing='8' justify='end'>"
    "    <button text='Cancel' cancel='true' onclick='cancel'/>"
    "    <button text='Save' default='true' onclick='save'/>"
    "  </hbox>"
    "</window>";

static const char compose_aui[] =
    "<window title='New message' width='720' height='560' padding='12' spacing='8'>"
    "  <grid columns='2' spacing='6'>"
    "    <label text='To'/><input id='to' placeholder='name@example.com, ...'/>"
    "    <label text='Cc'/><input id='cc'/>"
    "    <label text='Subject'/><input id='subject'/>"
    "  </grid>"
    "  <textarea id='body' wrap='true' expand='true' tabfocus='true'/>"
    "  <hbox spacing='8'>"
    "    <button text='Attach...' onclick='attach'/><label id='files' dim='true' expand='true'/>"
    "    <button text='Discard' onclick='discard'/>"
    "    <button id='send' text='Send' default='true' onclick='send' shortcut='Ctrl+Enter'/>"
    "  </hbox>"
    "</window>";

enum { OP_SYNC, OP_SEND, OP_FLAG, OP_DELETE };

struct compose {
    struct ui_window *win;
    char **files;
    int nfiles;
    char *in_reply_to;
};

struct op {
    int kind;
    char folder[128], trash[128], sent[128], flag[32];
    uint32_t uid;
    bool on;
    char *msg;
    size_t len;
    struct compose *compose;
    struct account account;
    char *password;
    int result, count;
    char error[400];
    struct op *next;
};

static struct ui_window *win;
static struct account account;
static bool have_account;
static char *password;              // in memory for this run
static int pipe_fds[2];
static mutex_t queue_lock;
static cond_t queue_cond;
static struct op *queue;
static int busy;

static char folders[64][128];
static int nfolders, current_folder;
static struct summary *list;
static int nlist;
static struct message open_msg;
static uint32_t open_uid;
static bool have_open;

static bool contains(const char *hay, const char *needle)
{
    size_t n = strlen(needle);

    for (; *hay; hay++)
        if (!strncasecmp(hay, needle, n))
            return true;
    return false;
}

static void status(const char *fmt, ...)
{
    char buf[512];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ui_set_text(ui_get(win, "status"), buf);
}

// ---- The worker ----

static const char *find_folder(const char *a, const char *b)
{
    for (int i = 0; i < nfolders; i++)
        if (contains(folders[i], a) || (b && contains(folders[i], b)))
            return folders[i];
    return "";
}

static void run_op(struct op *op)
{
    struct imap im;

    switch (op->kind) {
    case OP_SEND:
        if (smtp_send(&op->account, op->password, op->msg, op->len, op->error, sizeof(op->error)) < 0) {
            op->result = -1;
            return;
        }
        // A copy in Sent, when there is one.
        if (op->sent[0] && imap_open(&im, &op->account, op->password) == 0) {
            imap_append(&im, op->sent, op->msg, op->len, "\\Seen");
            imap_close(&im);
        }
        return;
    default:
        break;
    }
    if (imap_open(&im, &op->account, op->password) < 0) {
        strlcpy(op->error, im.error, sizeof(op->error));
        op->result = -1;
        return;
    }
    switch (op->kind) {
    case OP_SYNC: {
        char names[64][128];
        int n = imap_list(&im, names, 64);

        if (n < 0) {
            strlcpy(op->error, im.error, sizeof(op->error));
            op->result = -1;
            break;
        }
        store_set_folders(names, n);
        for (int i = 0; i < n; i++) {
            int added = imap_sync(&im, names[i]);

            if (added < 0) {
                strlcpy(op->error, im.error, sizeof(op->error));
                op->result = -1;
                break;
            }
            if (!strcasecmp(names[i], "INBOX"))
                op->count = added;
        }
        break;
    }
    case OP_FLAG:
        op->result = imap_flag(&im, op->folder, op->uid, op->flag, op->on);
        break;
    case OP_DELETE:
        op->result = imap_delete(&im, op->folder, op->uid, op->trash);
        if (op->result < 0)
            strlcpy(op->error, im.error, sizeof(op->error));
        break;
    }
    imap_close(&im);
}

static void *worker(void *arg)
{
    (void)arg;
    for (;;) {
        struct op *op;

        mutex_lock(&queue_lock);
        while (!queue)
            cond_wait(&queue_cond, &queue_lock);
        op = queue;
        queue = op->next;
        mutex_unlock(&queue_lock);
        run_op(op);
        write(pipe_fds[1], &op, sizeof(op));
    }
    return NULL;
}

static bool ask_password(void);

static struct op *new_op(int kind)
{
    struct op *op;

    if (!have_account || (!password && !ask_password()))
        return NULL;
    if (!(op = calloc(1, sizeof(*op))))
        return NULL;
    op->kind = kind;
    op->account = account;
    op->password = strdup(password);
    strlcpy(op->trash, find_folder("Trash", "Deleted"), sizeof(op->trash));
    strlcpy(op->sent, find_folder("Sent", NULL), sizeof(op->sent));
    return op;
}

static void submit(struct op *op)
{
    struct op **tail;

    mutex_lock(&queue_lock);
    for (tail = &queue; *tail; tail = &(*tail)->next)
        ;
    *tail = op;
    cond_signal(&queue_cond);
    mutex_unlock(&queue_lock);
    busy++;
}

static void op_free(struct op *op)
{
    if (op->password) {
        memset(op->password, 0, strlen(op->password));
        free(op->password);
    }
    free(op->msg);
    free(op);
}

// ---- Showing mail ----

static const char *folder_title(const char *name)
{
    if (!strcasecmp(name, "INBOX"))
        return "Inbox";
    return strrchr(name, '/') ? strrchr(name, '/') + 1 : strrchr(name, '.') && strncmp(name, "INBOX.", 6) == 0
                                                       ? strrchr(name, '.') + 1 : name;
}

static void load_folders(void)
{
    struct widget *fl = ui_get(win, "folders");
    char keep[128] = "INBOX";

    if (current_folder < nfolders)
        strlcpy(keep, folders[current_folder], sizeof(keep));
    nfolders = store_folders(folders, 64);
    if (!nfolders) {
        strlcpy(folders[0], "INBOX", 128);
        nfolders = 1;
    }
    ui_list_clear(fl);
    current_folder = 0;
    for (int i = 0; i < nfolders; i++) {
        static const struct {
            const char *part, *icon;
        } icons[] = { { "INBOX", "mail-glyph" }, { "Sent", "forward" }, { "Trash", "trash" },
                      { "Draft", "documents" }, { "Junk", "warning" }, { "Spam", "warning" } };
        struct surface *icon = NULL;

        ui_list_add(fl, folder_title(folders[i]));
        for (size_t k = 0; k < sizeof(icons) / sizeof(icons[0]); k++)
            if (contains(folders[i], icons[k].part)) {
                icon = icon_get(icons[k].icon, 16);
                break;
            }
        if (!icon)
            icon = icon_get("folder", 16);
        if (icon)
            ui_list_set_icon_shared(fl, i, icon);
        if (!strcmp(folders[i], keep))
            current_folder = i;
    }
    ui_list_select(fl, current_folder);
}

static void clear_open(void)
{
    if (have_open)
        mime_free(&open_msg);
    have_open = false;
    ui_set_text(ui_get(win, "subject"), " ");
    ui_set_text(ui_get(win, "from"), "");
    ui_set_text(ui_get(win, "to"), "");
    ui_set_text(ui_get(win, "body"), "");
    ui_set_visible(ui_get(win, "attbar"), false);
    ui_set_enabled(ui_get(win, "reply"), false);
    ui_set_enabled(ui_get(win, "replyall"), false);
    ui_set_enabled(ui_get(win, "forward"), false);
    ui_set_enabled(ui_get(win, "delete"), false);
}

static void load_messages(void)
{
    struct widget *t = ui_get(win, "messages");
    int unread = 0;
    uint32_t keep = have_open ? open_uid : 0;

    free(list);
    nlist = store_list(folders[current_folder], &list);
    ui_list_clear(t);
    for (int i = 0; i < nlist; i++) {
        char row[512], when[64];
        struct tm tm;
        int64_t now = time(NULL);

        if (list[i].when) {
            localtime_r(&list[i].when, &tm);
            strftime(when, sizeof(when), now - list[i].when < 20 * 3600 ? "%H:%M" : "%d %b %Y", &tm);
        } else {
            when[0] = 0;
        }
        snprintf(row, sizeof(row), "%s%s\t%s\t%s", list[i].seen ? "" : "\xE2\x97\x8F ", list[i].from,
                 list[i].subject, when);
        ui_list_add(t, row);
        unread += !list[i].seen;
        if (list[i].uid == keep)
            ui_list_select(t, i);
    }
    if (busy)
        return;
    if (!have_account)
        status("Set up your account with Account...");
    else
        status("%s: %d message%s, %d unread", folder_title(folders[current_folder]), nlist, nlist == 1 ? "" : "s",
               unread);
}

static void show_message(int index)
{
    char *raw, line[600];
    size_t len;

    clear_open();
    if (index < 0 || index >= nlist)
        return;
    if (!(raw = store_read(folders[current_folder], list[index].uid, &len)))
        return;
    mime_parse(raw, len, &open_msg);
    free(raw);
    have_open = true;
    open_uid = list[index].uid;
    ui_set_text(ui_get(win, "subject"), open_msg.subject && *open_msg.subject ? open_msg.subject : "(no subject)");
    snprintf(line, sizeof(line), "From %s%s%s", open_msg.from ? open_msg.from : "?",
             open_msg.date ? "  \xC2\xB7  " : "", open_msg.date ? open_msg.date : "");
    ui_set_text(ui_get(win, "from"), line);
    snprintf(line, sizeof(line), "To %s%s%s", open_msg.to ? open_msg.to : "", open_msg.cc ? ", cc " : "",
             open_msg.cc ? open_msg.cc : "");
    ui_set_text(ui_get(win, "to"), line);
    ui_set_text(ui_get(win, "body"), open_msg.text);
    if (open_msg.natts) {
        struct widget *d = ui_get(win, "attachments");

        ui_list_clear(d);
        for (int i = 0; i < open_msg.natts; i++) {
            char size[32], row[300];

            ui_format_size(open_msg.atts[i].len, size, sizeof(size));
            snprintf(row, sizeof(row), "%s (%s)", open_msg.atts[i].name, size);
            ui_list_add(d, row);
        }
        ui_list_select(d, 0);
        ui_set_visible(ui_get(win, "attbar"), true);
    }
    ui_set_enabled(ui_get(win, "reply"), true);
    ui_set_enabled(ui_get(win, "replyall"), true);
    ui_set_enabled(ui_get(win, "forward"), true);
    ui_set_enabled(ui_get(win, "delete"), true);
    // Opening it marks it read.
    if (!list[index].seen) {
        struct op *op;

        list[index].seen = true;
        store_set_flags(folders[current_folder], list[index].uid, "\\Seen");
        if ((op = new_op(OP_FLAG))) {
            strlcpy(op->folder, folders[current_folder], sizeof(op->folder));
            strlcpy(op->flag, "\\Seen", sizeof(op->flag));
            op->uid = list[index].uid;
            op->on = true;
            submit(op);
        }
        {
            char row[512];
            const char *old = ui_list_item(ui_get(win, "messages"), index);

            if (old && !strncmp(old, "\xE2\x97\x8F ", 4)) {
                strlcpy(row, old + 4, sizeof(row));
                ui_list_set_item(ui_get(win, "messages"), index, row);
            }
        }
    }
}

// ---- Jobs finishing ----

static void job_done(int fd, void *u)
{
    struct op *op;

    (void)u;
    if (read(fd, &op, sizeof(op)) != sizeof(op))
        return;
    busy--;
    switch (op->kind) {
    case OP_SYNC:
        ui_set_enabled(ui_get(win, "getmail"), true);
        if (op->result < 0) {
            status("Getting mail failed: %s", op->error);
            if (strstr(op->error, "password")) {
                free(password);
                password = NULL;
            }
        } else {
            load_folders();
            load_messages();
            if (op->count)
                status("%d new message%s", op->count, op->count == 1 ? "" : "s");
            if (op->count)
                syslog("mail", "%d new message%s", op->count, op->count == 1 ? "" : "s");
        }
        break;
    case OP_SEND:
        if (op->result < 0) {
            ui_set_enabled(ui_get(op->compose->win, "send"), true);
            ui_message(op->compose->win, "Sending failed", op->error, "OK");
            status("Sending failed.");
        } else {
            struct compose *c = op->compose;

            ui_window_close(c->win);
            for (int i = 0; i < c->nfiles; i++)
                free(c->files[i]);
            free(c->files);
            free(c->in_reply_to);
            free(c);
            status("Message sent.");
            {
                struct op *sync = new_op(OP_SYNC);

                if (sync)
                    submit(sync);
            }
        }
        break;
    case OP_DELETE:
        if (op->result < 0) {
            status("Deleting failed: %s", op->error);
        } else {
            // Bring Trash up to date.
            struct op *sync = new_op(OP_SYNC);

            if (sync)
                submit(sync);
        }
        break;
    }
    op_free(op);
}

// ---- Account ----

static struct ui_window *dlg;

static void on_dlg_save(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    if (!*ui_text(ui_get(dlg, "email")) || !*ui_text(ui_get(dlg, "imap_host"))) {
        ui_message(dlg, "Email account", "Type at least the email address and the incoming server.", "OK");
        return;
    }
    ui_dialog_end(dlg, 1);
}

static void on_dlg_cancel(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    ui_dialog_end(dlg, 0);
}

static bool edit_account(void)
{
    static const struct ui_handler_entry handlers[] = {
        { "save", on_dlg_save }, { "cancel", on_dlg_cancel }, { NULL, NULL },
    };
    char num[16], secret[200];
    struct account a = account;

    if (!(dlg = ui_load_string_named(account_aui, handlers, NULL, "mail-account")))
        return false;
    ui_set_text(ui_get(dlg, "name"), a.name);
    ui_set_text(ui_get(dlg, "email"), a.email);
    ui_set_text(ui_get(dlg, "user"), a.user);
    ui_set_text(ui_get(dlg, "imap_host"), a.imap_host);
    snprintf(num, sizeof(num), "%d", a.imap_port);
    ui_set_text(ui_get(dlg, "imap_port"), num);
    ui_list_select(ui_get(dlg, "imap_sec"), a.imap_security);
    ui_set_text(ui_get(dlg, "smtp_host"), a.smtp_host);
    snprintf(num, sizeof(num), "%d", a.smtp_port);
    ui_set_text(ui_get(dlg, "smtp_port"), num);
    ui_list_select(ui_get(dlg, "smtp_sec"), a.smtp_security);
    ui_set_text(ui_get(dlg, "ca_file"), a.ca_file);
    if (password)
        ui_set_text(ui_get(dlg, "password"), password);
    ui_set_text(ui_get(dlg, "note"), cred_unlocked()
                ? "A remembered password is kept encrypted in your credentials folder."
                : "Your credential store is locked (you signed in without a password), so the password "
                  "is asked for each time.");
    if (!a.name[0]) {
        struct user_info me;

        if (user_current(&me) == 0)
            ui_set_text(ui_get(dlg, "name"), me.display);
    }
    if (ui_dialog_run(dlg, win) != 1)
        return false;
    strlcpy(a.name, ui_text(ui_get(dlg, "name")), sizeof(a.name));
    strlcpy(a.email, ui_text(ui_get(dlg, "email")), sizeof(a.email));
    strlcpy(a.user, ui_text(ui_get(dlg, "user")), sizeof(a.user));
    strlcpy(a.imap_host, ui_text(ui_get(dlg, "imap_host")), sizeof(a.imap_host));
    a.imap_port = atoi(ui_text(ui_get(dlg, "imap_port")));
    a.imap_security = ui_list_selected(ui_get(dlg, "imap_sec"));
    strlcpy(a.smtp_host, ui_text(ui_get(dlg, "smtp_host")), sizeof(a.smtp_host));
    a.smtp_port = atoi(ui_text(ui_get(dlg, "smtp_port")));
    a.smtp_security = ui_list_selected(ui_get(dlg, "smtp_sec"));
    strlcpy(a.ca_file, ui_text(ui_get(dlg, "ca_file")), sizeof(a.ca_file));
    if (!a.imap_port)
        a.imap_port = a.imap_security == SEC_TLS ? 993 : 143;
    if (!a.smtp_port)
        a.smtp_port = a.smtp_security == SEC_TLS ? 465 : 587;
    if (!a.smtp_host[0])
        strlcpy(a.smtp_host, a.imap_host, sizeof(a.smtp_host));
    account = a;
    have_account = account_save(&account) == 0;
    free(password);
    password = *ui_text(ui_get(dlg, "password")) ? strdup(ui_text(ui_get(dlg, "password"))) : NULL;
    account_secret_name(&account, secret, sizeof(secret));
    if (password && ui_value(ui_get(dlg, "remember")) && cred_unlocked())
        cred_set(secret, password);
    else if (!ui_value(ui_get(dlg, "remember")))
        cred_delete(secret);
    return true;
}

static bool ask_password(void)
{
    char text[300], *p;

    snprintf(text, sizeof(text), "The password for %s:", account.user[0] ? account.user : account.email);
    if (!(p = ui_prompt_password(win, "Email", text)))
        return false;
    free(password);
    password = p;
    return true;
}

// ---- Toolbar ----

static void on_getmail(struct widget *w, void *u)
{
    struct op *op;

    (void)w;
    (void)u;
    if (!have_account && !edit_account())
        return;
    if (!(op = new_op(OP_SYNC)))
        return;
    ui_set_enabled(ui_get(win, "getmail"), false);
    status("Getting mail from %s...", account.imap_host);
    submit(op);
}

static bool auto_check(void *u)
{
    (void)u;
    if (have_account && password && !busy)
        on_getmail(NULL, NULL);
    return true;
}

static void on_account(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    if (edit_account())
        on_getmail(NULL, NULL);
}

static void on_folder(struct widget *w, void *u)
{
    int i = ui_list_selected(w);

    (void)u;
    if (i < 0 || i >= nfolders || i == current_folder)
        return;
    current_folder = i;
    clear_open();
    load_messages();
}

static void on_show(struct widget *w, void *u)
{
    (void)u;
    show_message(ui_list_selected(w));
}

static void on_saveatt(struct widget *w, void *u)
{
    int i = ui_list_selected(ui_get(win, "attachments"));
    char *path, dir[300];
    struct user_info me;
    int fd;

    (void)w;
    (void)u;
    if (!have_open || i < 0 || i >= open_msg.natts)
        return;
    dir[0] = 0;
    if (user_current(&me) == 0)
        user_path(&me, "home/Downloads", dir, sizeof(dir));
    if (!(path = ui_file_dialog(win, "Save attachment", dir[0] ? dir : NULL, true, open_msg.atts[i].name)))
        return;
    if ((fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644)) >= 0) {
        write(fd, open_msg.atts[i].data, open_msg.atts[i].len);
        close(fd);
        status("Saved %s", path);
    } else {
        ui_message(win, "Email", "The attachment could not be saved.", "OK");
    }
    free(path);
}

static void on_delete(struct widget *w, void *u)
{
    int i = ui_list_selected(ui_get(win, "messages"));
    struct op *op;

    (void)w;
    (void)u;
    if (i < 0 || i >= nlist || !(op = new_op(OP_DELETE)))
        return;
    strlcpy(op->folder, folders[current_folder], sizeof(op->folder));
    op->uid = list[i].uid;
    store_remove(folders[current_folder], list[i].uid);
    submit(op);
    clear_open();
    load_messages();
    if (nlist) {
        ui_list_select(ui_get(win, "messages"), MIN(i, nlist - 1));
        show_message(MIN(i, nlist - 1));
    }
    status("Deleted.");
}

// ---- Writing ----

static void compose_attach(struct widget *w, void *u)
{
    struct compose *c = u;
    char *path = ui_file_dialog(c->win, "Attach a file", NULL, false, NULL), **m, names[512] = "";

    (void)w;
    if (!path)
        return;
    if (!(m = realloc(c->files, (c->nfiles + 1) * sizeof(char *)))) {
        free(path);
        return;
    }
    c->files = m;
    c->files[c->nfiles++] = path;
    for (int i = 0; i < c->nfiles; i++) {
        const char *base = strrchr(c->files[i], '/') ? strrchr(c->files[i], '/') + 1 : c->files[i];

        if (i)
            strlcat(names, ", ", sizeof(names));
        strlcat(names, base, sizeof(names));
    }
    ui_set_text(ui_get(c->win, "files"), names);
}

static void compose_discard(struct widget *w, void *u)
{
    struct compose *c = u;

    (void)w;
    if (*ui_text(ui_get(c->win, "body"))
        && ui_message(c->win, "Discard", "Throw this message away?", "Discard|Keep writing") != 0)
        return;
    ui_window_close(c->win);
    for (int i = 0; i < c->nfiles; i++)
        free(c->files[i]);
    free(c->files);
    free(c->in_reply_to);
    free(c);
}

static void compose_send(struct widget *w, void *u)
{
    struct compose *c = u;
    struct op *op;
    const char *to = ui_text(ui_get(c->win, "to"));

    (void)w;
    if (!strchr(to, '@')) {
        ui_message(c->win, "Send", "Type who the message is for.", "OK");
        return;
    }
    if (!have_account && !edit_account())
        return;
    if (!(op = new_op(OP_SEND)))
        return;
    op->msg = mime_compose(&account, to, ui_text(ui_get(c->win, "cc")), ui_text(ui_get(c->win, "subject")),
                           ui_text(ui_get(c->win, "body")), c->in_reply_to, c->files, c->nfiles, &op->len);
    op->compose = c;
    ui_set_enabled(ui_get(c->win, "send"), false);
    status("Sending...");
    submit(op);
}

static struct compose *compose(const char *to, const char *cc, const char *subject, const char *body,
                               const char *in_reply_to)
{
    struct compose *c = calloc(1, sizeof(*c));
    struct ui_handler_entry handlers[] = {
        { "attach", compose_attach }, { "discard", compose_discard }, { "send", compose_send }, { NULL, NULL },
    };

    if (!c || !(c->win = ui_load_string_named(compose_aui, handlers, c, "mail-compose"))) {
        free(c);
        return NULL;
    }
    ui_set_text(ui_get(c->win, "to"), to ? to : "");
    ui_set_text(ui_get(c->win, "cc"), cc ? cc : "");
    ui_set_text(ui_get(c->win, "subject"), subject ? subject : "");
    ui_set_text(ui_get(c->win, "body"), body ? body : "");
    if (subject && *subject)
        ui_window_set_title(c->win, subject);
    c->in_reply_to = in_reply_to ? strdup(in_reply_to) : NULL;
    ui_window_show(c->win);
    ui_focus(ui_get(c->win, to && *to ? "body" : "to"));
    return c;
}

static void on_write(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    compose(NULL, NULL, NULL, NULL, NULL);
}

// The open message quoted, for replies and forwards.
static char *quoted(bool forward)
{
    size_t cap = strlen(open_msg.text) * 2 + 1024, o = 0;
    char *out = malloc(cap), name[128];
    const char *p = open_msg.text;

    if (!out)
        return NULL;
    mime_display_name(open_msg.from, name, sizeof(name));
    if (forward)
        o += snprintf(out, cap, "\n\n-------- Forwarded message --------\nFrom: %s\nDate: %s\nSubject: %s\nTo: %s\n\n",
                      open_msg.from ? open_msg.from : "", open_msg.date ? open_msg.date : "",
                      open_msg.subject ? open_msg.subject : "", open_msg.to ? open_msg.to : "");
    else
        o += snprintf(out, cap, "\n\nOn %s, %s wrote:\n", open_msg.date ? open_msg.date : "an earlier day", name);
    while (*p && o + 4 < cap) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);

        if (o + len + 4 >= cap)
            break;
        if (!forward) {
            out[o++] = '>';
            if (len && *p != '>')
                out[o++] = ' ';
        }
        memcpy(out + o, p, len);
        o += len;
        out[o++] = '\n';
        p = nl ? nl + 1 : p + len;
    }
    out[o] = 0;
    return out;
}

static void reply(bool all)
{
    char subject[300], *body, cc[1024] = "";
    const char *s = open_msg.subject ? open_msg.subject : "";
    char *reply_to = NULL;

    if (!have_open)
        return;
    snprintf(subject, sizeof(subject), "%s%s", strncasecmp(s, "Re:", 3) ? "Re: " : "", s);
    if (all) {
        // Everyone else on To and Cc.
        const char *lists[2] = { open_msg.to, open_msg.cc };

        for (int k = 0; k < 2; k++) {
            char *copy = lists[k] ? strdup(lists[k]) : NULL, *part = copy, *next;

            for (; part && *part; part = next) {
                char addr[256];

                next = strchr(part, ',');
                if (next)
                    *next++ = 0;
                else
                    next = part + strlen(part);

                mime_address(part, addr, sizeof(addr));
                if (!*addr || !strcasecmp(addr, account.email))
                    continue;
                if (*cc)
                    strlcat(cc, ", ", sizeof(cc));
                strlcat(cc, part[0] == ' ' ? part + 1 : part, sizeof(cc));
            }
            free(copy);
        }
    }
    body = quoted(false);
    reply_to = open_msg.message_id;
    compose(open_msg.from, cc, subject, body, reply_to);
    free(body);
}

static void on_reply(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    reply(false);
}

static void on_replyall(struct widget *w, void *u)
{
    (void)w;
    (void)u;
    reply(true);
}

static void on_forward(struct widget *w, void *u)
{
    char subject[300], *body;

    (void)w;
    (void)u;
    if (!have_open)
        return;
    snprintf(subject, sizeof(subject), "Fwd: %s", open_msg.subject ? open_msg.subject : "");
    body = quoted(true);
    compose(NULL, NULL, subject, body, NULL);
    free(body);
}

int main(int argc, char **argv)
{
    static const struct ui_handler_entry handlers[] = {
        { "getmail", on_getmail }, { "write", on_write }, { "reply", on_reply }, { "replyall", on_replyall },
        { "forward", on_forward }, { "delete", on_delete }, { "account", on_account }, { "folder", on_folder },
        { "show", on_show }, { "saveatt", on_saveatt }, { NULL, NULL },
    };
    thread_t t;
    char secret[200];

    (void)argc;
    (void)argv;
    ui_load_user_theme();
    if (pipe(pipe_fds) < 0 || thread_create(&t, worker, NULL) < 0)
        return 1;
    if (!(win = ui_load_string_named(window_aui, handlers, NULL, "mail")))
        return 1;
    ui_watch_fd(pipe_fds[0], job_done, NULL);
    have_account = account_load(&account) == 0;
    if (have_account) {
        account_secret_name(&account, secret, sizeof(secret));
        password = cred_get(secret);
    }
    load_folders();
    clear_open();
    load_messages();
    ui_window_show(win);
    if (have_account && password)
        on_getmail(NULL, NULL);
    else if (!have_account)
        status("Welcome. Set up your email account with Account... to start.");
    else
        status("Press Get mail to sign in.");
    ui_timer(5 * 60 * 1000, auto_check, NULL);
    return ui_run();
}
