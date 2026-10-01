#include "pipe.h"
#include "mem.h"
#include "process.h"
#include "string.h"
#include "abi/poll.h"

#define PIPE_SIZE   (64 * 1024)

struct pipe {
    struct wait_queue wq;           // wq.lock guards everything here
    uint8_t *buf;
    size_t head, count;
    uint32_t readers, writers;
};

static void pipe_free(struct pipe *p)
{
    kfree(p->buf);
    kfree(p);
}

static int64_t pipe_read(struct file *f, void *out, size_t size)
{
    struct pipe *p = f->priv;
    uint64_t flags = spin_lock_irqsave(&p->wq.lock);
    size_t n = 0;

    for (;;) {
        wait_prepare();
        if (p->count || !p->writers)
            break;
        if (f->flags & O_NONBLOCK) {
            spin_unlock_irqrestore(&p->wq.lock, flags);
            return -EAGAIN;
        }
        if (signal_pending()) {
            spin_unlock_irqrestore(&p->wq.lock, flags);
            return -EINTR;
        }
        wait_queue_sleep_locked(&p->wq);
        spin_lock(&p->wq.lock);
    }
    while (n < size && p->count) {
        size_t chunk = MIN(size - n, MIN(p->count, PIPE_SIZE - p->head));

        memcpy((uint8_t *)out + n, p->buf + p->head, chunk);
        p->head = (p->head + chunk) % PIPE_SIZE;
        p->count -= chunk;
        n += chunk;
    }
    wake_up_locked(&p->wq);
    spin_unlock_irqrestore(&p->wq.lock, flags);
    return n;
}

static int64_t pipe_write(struct file *f, const void *in, size_t size)
{
    struct pipe *p = f->priv;
    uint64_t flags = spin_lock_irqsave(&p->wq.lock);
    size_t n = 0;

    while (n < size) {
        wait_prepare();
        if (!p->readers) {
            spin_unlock_irqrestore(&p->wq.lock, flags);
            if (!n)
                signal_thread(sched_current(), SIGPIPE);
            return n ? (int64_t)n : -EPIPE;
        }
        if (p->count == PIPE_SIZE) {
            if ((f->flags & O_NONBLOCK) || signal_pending()) {
                spin_unlock_irqrestore(&p->wq.lock, flags);
                return n ? (int64_t)n : (f->flags & O_NONBLOCK) ? -EAGAIN : -EINTR;
            }
            wait_queue_sleep_locked(&p->wq);
            spin_lock(&p->wq.lock);
            continue;
        }
        size_t tail = (p->head + p->count) % PIPE_SIZE;
        size_t chunk = MIN(size - n, MIN(PIPE_SIZE - p->count, PIPE_SIZE - tail));

        memcpy(p->buf + tail, (const uint8_t *)in + n, chunk);
        p->count += chunk;
        n += chunk;
        wake_up_locked(&p->wq);
    }
    spin_unlock_irqrestore(&p->wq.lock, flags);
    return n;
}

static uint32_t pipe_poll(struct file *f, struct poll_table *pt)
{
    struct pipe *p = f->priv;
    uint64_t flags = spin_lock_irqsave(&p->wq.lock);
    uint32_t ev = 0;

    if ((f->flags & O_ACCMODE) == O_RDONLY) {
        if (p->count)
            ev |= POLLIN;
        if (!p->writers)
            ev |= POLLHUP;
    } else {
        if (p->count < PIPE_SIZE)
            ev |= POLLOUT;
        if (!p->readers)
            ev |= POLLERR;
    }
    spin_unlock_irqrestore(&p->wq.lock, flags);
    poll_wait(pt, &p->wq);
    return ev;
}

static void pipe_close(struct file *f)
{
    struct pipe *p = f->priv;
    uint64_t flags = spin_lock_irqsave(&p->wq.lock);
    bool last;

    if ((f->flags & O_ACCMODE) == O_RDONLY)
        p->readers--;
    else
        p->writers--;
    last = !p->readers && !p->writers;
    wake_up_locked(&p->wq);
    spin_unlock_irqrestore(&p->wq.lock, flags);
    if (last)
        pipe_free(p);
}

static const struct file_ops pipe_ops = {
    .read = pipe_read, .write = pipe_write, .close = pipe_close, .poll = pipe_poll,
};

int pipe_create(uint32_t flags, struct file **read_end, struct file **write_end)
{
    struct pipe *p = kzalloc(sizeof(*p));
    struct file *r, *w;

    if (!p || !(p->buf = kmalloc(PIPE_SIZE))) {
        kfree(p);
        return -ENOMEM;
    }
    p->wq = (struct wait_queue)WAIT_QUEUE_INIT;
    r = file_alloc(&pipe_ops, O_RDONLY | (flags & O_NONBLOCK));
    w = file_alloc(&pipe_ops, O_WRONLY | (flags & O_NONBLOCK));
    if (!r || !w) {
        kfree(r);
        kfree(w);
        pipe_free(p);
        return -ENOMEM;
    }
    r->priv = w->priv = p;
    p->readers = p->writers = 1;
    *read_end = r;
    *write_end = w;
    return 0;
}
