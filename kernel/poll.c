#include "poll.h"
#include "apic.h"
#include "mem.h"
#include "process.h"
#include "abi/poll.h"

void poll_wait(struct poll_table *pt, struct wait_queue *q)
{
    if (!pt || pt->count == POLL_MAX_WAITS)
        return;
    pt->waits[pt->count].q = q;
    pt->waits[pt->count].e.thread = sched_current();
    wait_queue_add(q, &pt->waits[pt->count].e);
    pt->count++;
}

uint32_t file_poll(struct file *f, struct poll_table *pt)
{
    if (f->ops && f->ops->poll)
        return f->ops->poll(f, pt);
    return POLLIN | POLLOUT;
}

int do_poll(struct process *p, struct pollfd *fds, int n, int64_t timeout_ms)
{
    struct poll_table *pt = kzalloc(sizeof(*pt));
    struct file **files = kzalloc(n * sizeof(*files) + 1);
    uint64_t deadline = timeout_ms < 0 ? UINT64_MAX : timer_uptime_ms() + timeout_ms;
    bool first = true;
    int ready = 0;

    if (!pt || !files) {
        kfree(pt);
        kfree(files);
        return -ENOMEM;
    }
    // Hold the files so their wait queues outlive the wait.
    for (int i = 0; i < n; i++) {
        if (fds[i].fd >= 0 && (files[i] = fd_get(p, fds[i].fd)))
            file_ref(files[i]);
    }
    for (;;) {
        uint64_t now;

        wait_prepare();
        ready = 0;
        for (int i = 0; i < n; i++) {
            uint32_t ev;

            fds[i].revents = 0;
            if (fds[i].fd < 0)
                continue;
            if (!files[i]) {
                fds[i].revents = POLLNVAL;
                ready++;
                continue;
            }
            ev = file_poll(files[i], first ? pt : NULL);
            fds[i].revents = ev & (fds[i].events | POLLERR | POLLHUP | POLLNVAL);
            if (fds[i].revents)
                ready++;
        }
        first = false;
        if (ready || timeout_ms == 0)
            break;
        if (signal_pending()) {
            ready = -EINTR;
            break;
        }
        now = timer_uptime_ms();
        if (now >= deadline)
            break;
        if (!sched_block_timeout(deadline == UINT64_MAX ? UINT64_MAX : deadline - now)
            && timer_uptime_ms() >= deadline)
            break;
    }
    for (int i = 0; i < pt->count; i++)
        wait_queue_remove(pt->waits[i].q, &pt->waits[i].e);
    for (int i = 0; i < n; i++)
        file_put(files[i]);
    kfree(files);
    kfree(pt);
    return ready;
}
