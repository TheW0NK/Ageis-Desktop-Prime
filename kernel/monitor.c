#include "kernel.h"
#include "acpi.h"
#include "apic.h"
#include "display.h"
#include "process.h"
#include "tty.h"
#include "mem.h"
#include "vfs.h"
#include "pci.h"
#include "sched.h"
#include "string.h"

#define LINE_MAX 128

static size_t read_line(char *buf, size_t size)
{
    int64_t n = tty_read(buf, size - 1, false);

    if (n <= 0)
        n = 0;
    if (n && buf[n - 1] == '\n')
        n--;
    buf[n] = '\0';
    return n;
}

static void cmd_help(void)
{
    kprintf("  help      this list\n"
            "  mem       memory usage\n"
            "  threads   list threads\n"
            "  ps        list processes\n"
            "  pci       list PCI devices\n"
            "  acpi      ACPI summary\n"
            "  displays  list displays\n"
            "  uptime    time since boot\n"
            "  clear     clear the screen\n"
            "  reboot    restart the machine\n"
            "  shutdown  power off\n");
}

static void cmd_mem(void)
{
    kprintf("  %lu MiB free of %lu MiB\n", pmm_free_count() * PAGE_SIZE >> 20,
            pmm_total_count() * PAGE_SIZE >> 20);
}

static void cmd_pci(void)
{
    for (size_t i = 0; i < pci_device_count(); i++) {
        const struct pci_device *d = pci_device_at(i);

        kprintf("  %02x:%02x.%x  %04x:%04x  %s\n", d->bus, d->slot, d->function,
                d->vendor, d->device, pci_class_name(d->class_code, d->subclass));
    }
}

static void cmd_acpi(void)
{
    if (!acpi.present) {
        kprintf("  ACPI not available\n");
        return;
    }
    kprintf("  OEM %s, revision %u\n", acpi.oem, acpi.revision);
    kprintf("  %u CPU(s), %u IO APIC(s), local APIC at 0x%lx\n",
            acpi.cpu_count, acpi.ioapic_count, acpi.lapic_address);
    kprintf("  PM timer port 0x%x, reset %s, S5 %s\n", acpi.pm_timer_port,
            acpi.reset_supported ? "yes" : "no", acpi.s5_valid ? "yes" : "no");
}

static void cmd_displays(void)
{
    for (uint32_t i = 0; i < display_count(); i++) {
        const struct display *d = display_get(i);

        kprintf("  %u: %ux%u, %u bpp, framebuffer 0x%lx%s\n", i, d->width, d->height,
                d->bytes_per_pixel * 8, (uint64_t)d->vram, i == 0 ? " (console)" : "");
    }
}

static void run(const char *cmd)
{
    if (!strcmp(cmd, "help"))
        cmd_help();
    else if (!strcmp(cmd, "mem"))
        cmd_mem();
    else if (!strcmp(cmd, "threads"))
        sched_list();
    else if (!strcmp(cmd, "ps"))
        process_list();
    else if (!strcmp(cmd, "pci"))
        cmd_pci();
    else if (!strcmp(cmd, "acpi"))
        cmd_acpi();
    else if (!strcmp(cmd, "displays"))
        cmd_displays();
    else if (!strcmp(cmd, "uptime"))
        kprintf("  %lu ms\n", timer_uptime_ms());
    else if (!strcmp(cmd, "clear"))
        console_clear();
    else if (!strcmp(cmd, "reboot")) {
        vfs_unmount_all();
        acpi_reboot();
    } else if (!strcmp(cmd, "shutdown")) {
        vfs_unmount_all();
        acpi_shutdown();
    }
    else if (cmd[0])
        kprintf("  unknown command '%s'; try 'help'\n", cmd);
}

void monitor_thread(void *arg)
{
    char line[LINE_MAX];

    (void)arg;
    kprintf("Kernel monitor (rescue mode). Type 'help'.\n");
    for (;;) {
        kprintf("aegis> ");
        read_line(line, sizeof(line));
        run(line);
    }
}
