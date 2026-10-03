#include "stopcodes.h"
#include "kernel.h"
#include "acpi.h"
#include "apic.h"
#include "block.h"
#include "bootinfo.h"
#include "cpu.h"
#include "devfs.h"
#include "display.h"
#include "input.h"
#include "keyboard.h"
#include "mem.h"
#include "net.h"
#include "pci.h"
#include "process.h"
#include "random.h"
#include "rtc.h"
#include "sched.h"
#include "serial.h"
#include "smp.h"
#include "string.h"
#include "syscall.h"
#include "vfs.h"

void monitor_thread(void *arg);

static char cmdline[512];
static uint64_t acpi_rsdp, ramdisk_base, ramdisk_size;

const char *kernel_cmdline(void)
{
    return cmdline;
}

static const char *cmdline_value(const char *key)
{
    size_t klen = strlen(key);

    for (const char *p = cmdline; *p;) {
        while (*p == ' ')
            p++;
        if (!strncmp(p, key, klen) && p[klen] == '=')
            return p + klen + 1;
        while (*p && *p != ' ')
            p++;
    }
    return NULL;
}

static struct block_device *find_root(void)
{
    const char *spec = cmdline_value("root");
    uint8_t guid[16];

    // The live system runs from the ramdisk the bootloader loaded.
    if (ramdisk_device() && (!spec || (!strncmp(spec, "ram0", 4) && (spec[4] == ' ' || !spec[4]))))
        return ramdisk_device();
    if (spec && !strncmp(spec, "PARTUUID=", 9) && guid_parse(spec + 9, guid)) {
        for (size_t i = 0; i < block_count(); i++) {
            struct block_device *d = block_at(i);
            if (d->parent && !memcmp(d->part_guid, guid, 16))
                return d;
        }
    }
    for (size_t i = 0; i < block_count(); i++) {
        struct block_device *d = block_at(i);
        if (d->parent && vfs_mount("ext4", d, "/", false) == 0)
            return d;
    }
    return NULL;
}

static void mount_root(void)
{
    struct block_device *dev = find_root();
    int ret;

    if (!dev) {
        kprintf("No root filesystem found\n");
        return;
    }
    if (!vfs_root() && (ret = vfs_mount("ext4", dev, "/", false)) < 0)
        kprintf("Cannot mount %s as root: error %d\n", dev->name, ret);
}

static void mount_boot(void)
{
    static const char esp_type[] = "c12a7328-f81f-11d2-ba4b-00a0c93ec93b";
    uint8_t guid[16];

    // A system running from the ramdisk (install media, recovery) has no
    // EFI partition of its own: any found belongs to a disk it works on.
    if (vfs_root() && vfs_root()->mount->dev == ramdisk_device() && ramdisk_device())
        return;
    guid_parse(esp_type, guid);
    for (size_t i = 0; i < block_count(); i++) {
        struct block_device *d = block_at(i);

        if (d->parent && !memcmp(d->type_guid, guid, 16) && vfs_mount("fat", d, "/osystem/boot", false) == 0)
            return;
    }
}

static void mount_dev(void)
{
    int ret = vfs_mkdir("/osystem/devices", NULL, &root_cred, 0755);

    devfs_set_time(rtc_now());
    if ((ret == 0 || ret == -EEXIST) && (ret = vfs_mount("devfs", NULL, "/osystem/devices", false)) == 0)
        return;
    kprintf("Cannot mount /osystem/devices: error %d\n", ret);
}

static void kinit(void *arg)
{
    (void)arg;
    ramdisk_init(ramdisk_base, ramdisk_size);
    xhci_init();
    ahci_init();
    nvme_init();
    virtio_blk_init();
    ext4_register();
    fat_register();
    devfs_register_fs();
    mount_root();
    if (vfs_root()) {
        mount_boot();
        mount_dev();
    }
    display_devfs_init();
    cmdline_devfs_init();
    hda_init();
    vcam_init();
    smp_init();
    net_init();
    virtio_net_init();
    e1000_init();

    static char *argv[] = { "init", NULL };
    static char *envp[] = { "PATH=/sysapps:/osystem/core:/userApps/commands", "HOME=/", NULL };
    int pid, ret = vfs_root() ? process_spawn("/osystem/core/init", argv, envp, NULL, &pid) : -ENOENT;

    if (ret < 0) {
        kprintf("Cannot start /osystem/core/init (error %d); starting the kernel monitor\n", ret);
        if (!thread_create("monitor", monitor_thread, NULL))
            panic_code(STOP_THREAD_START, "Cannot start the kernel monitor");
    }
}

void kmain(struct aegis_boot_info *info)
{
    serial_init();

    if (!info || info->magic != AEGIS_BOOT_MAGIC || info->version != AEGIS_BOOT_VERSION)
        panic_code(STOP_BAD_BOOT_INFO, "Invalid boot info from the bootloader");

    display_init(info->framebuffers, info->framebuffer_count);
    kprintf("Aegis kernel %s\n", AEGIS_VERSION);

    cpu_init();

    memcpy(cmdline, (const char *)info->cmdline,
           strnlen((const char *)info->cmdline, sizeof(cmdline) - 1));
    acpi_rsdp = info->acpi_rsdp;
    efi_init(info->efi_system_table);
    ramdisk_base = info->ramdisk_base;
    ramdisk_size = info->ramdisk_size;

    mem_init(info);
    info = NULL;
    display_enable_backbuffers();
    kprintf("Memory: %lu MiB free of %lu MiB\n",
            pmm_free_count() * PAGE_SIZE >> 20, pmm_total_count() * PAGE_SIZE >> 20);

    acpi_init(acpi_rsdp);
    if (!acpi.present)
        panic_code(STOP_NO_ACPI, "No ACPI tables; the APIC cannot be configured");
    mem_reclaim_acpi();
    kprintf("ACPI: %s, %u CPU(s), %u IO APIC(s)\n", acpi.oem, acpi.cpu_count, acpi.ioapic_count);

    apic_init();
    timer_init();
    rtc_init();
    kprintf("Timer: local APIC at %d Hz\n", TIMER_HZ);

    sched_init();
    syscall_init();

    random_init();
    input_init();
    if (!strstr(cmdline, "verbose"))
        splash_start();

    pci_init();
    kprintf("PCI: %lu device(s)\n", pci_device_count());
    ps2_init();
    kprintf("PS/2: %s, %s\n", ps2_keyboard_present() ? "keyboard" : "no keyboard",
            ps2_mouse_present() ? "mouse" : "no mouse");
    kprintf("Displays: %u\n", display_count());
    kprintf("Command line: %s\n", cmdline);

    if (!thread_create("kinit", kinit, NULL))
        panic_code(STOP_THREAD_START, "Cannot start kinit");

    for (;;)
        __asm__ volatile ("sti; hlt");
}
