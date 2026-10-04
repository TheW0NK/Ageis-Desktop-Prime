# Aegis Desktop roadmap

Aegis Core (boot manager, bootloader, kernel, terminal) is the base. This file
records the plan for turning it into a desktop OS and the decisions behind it.

## Decisions

| Topic | Decision |
|---|---|
| Hardware | QEMU first (virtio, e1000, Intel HDA, xHCI, PS/2). Real-PC drivers come in a later phase. |
| Third-party code | Small permissive libraries (MIT, BSD, public domain, Apache 2.0; SIL OFL for fonts) may be vendored under `third_party/` with their licenses. |
| Languages | English, Spanish, French, German, Simplified Chinese. All UI text goes through translation tables; text is UTF-8 everywhere. Chinese input uses a pinyin input method. |
| Updates | Offline only for now: signed update packages installed from a file or USB drive. |
| Graphics | Software rendering to the UEFI framebuffer. No GPU acceleration. |
| UI language | **AUI**: HTML-derived markup without styling. The theme decides appearance. |
| Verbose boot | F13 during boot shows logs. Also available from the boot manager menu and by holding Shift, since most keyboards have no F13. |

### App model

- Apps live in an install folder that is on `PATH`. The supported way to install
  one is an **`.aip`** (Aegis installer package), the Aegis counterpart of `.msi`.
- An `.aip` is a package read by the system installer, not a program. It holds
  the app's files, manifest, icon, shortcuts, PATH entries, default settings and
  requested permissions. It can include setup steps; they run only after the
  user approves, and only with the permissions that were granted.
- Install, repair and uninstall are handled by the installer. Packages can
  install for one user or for the whole machine (machine-wide needs an admin).
- Before installing, the installer asks which permissions to grant, starting
  with the basic ones. What a user may grant depends on their account:

| Tier | Who can grant it | Permissions |
|---|---|---|
| Basic | Any user | The app's own data folder; Documents, Downloads, Music, Images; notifications; clipboard |
| Elevated | Admin accounts | Network, camera, microphone, the whole home folder, other apps' data |
| System | Admin + password | System files, other users, devices, running at startup |
| powersudo | Admin + password, per run | Runs the app completely unrestricted, with no sandbox. Stronger than `sudo`. |

### Per-user layout

```
/userfiles/<name>/
    home/        Downloads, Documents, Music, Images, Desktop (shortcuts)
    system/      settings (theme, language, background, picture, display name)
                 credentials/  (encrypted with a key derived from the login password)
                 appdata/
```

Credentials are encrypted with PBKDF2-SHA256 + ChaCha20-Poly1305. An admin can
reset a forgotten password but cannot recover the encrypted credentials.

## Status

| Phase | State |
|---|---|
| 1. Input and test tooling | Done |
| 2. Kernel services | Done |
| 3. Networking | Done for IPv4 and TLS. IPv6 is deferred until something needs it. |
| 4. Graphics and windowing | Done: compositor, libgfx, AUI and toolkit, sign-in screen, desktop shell, Terminal. Ctrl+Alt+F2 shows the text console, Ctrl+Alt+F1 returns. |
| 5. Audio and camera | Done: Intel HD Audio driver, audio server with per-app streams and volume keys, WAV/Ogg/MP3 playback, Audio Player; a virtual test camera (/osystem/devices/camera) and the Camera app. A USB Video Class driver for real cameras comes later. |
| 6. Users and language | Started: encrypted credential store (PBKDF2-SHA256 sealed master key, ChaCha20-Poly1305 per secret), unlocked at sign-in. Translations not started. |
| 7. Desktop basics | Done: shared clipboard, notifications, screenshots, lock screen, recycle bin, file search in the launcher, window snapping, drag and drop between windows, four workspaces, the command palette (Super+P) and a guest account. |
| 8. App model | Started: `.aip` packages with an installer that asks which permissions to grant (see [PACKAGES.md](PACKAGES.md)), repair and uninstall, `aip` in the terminal. Left: signing, the sandbox that enforces permissions, powersudo. |
| 9. Apps | Done: every app in the four suites. |
| Install media | Done: `make iso` builds a live ISO that boots into a graphical installer (erases one disk). |
| 10.–11. | Planned |

## Phases

1. **Input and test tooling.** Automated QEMU test script; PS/2 and USB mouse and
   tablet; full-size keyboards (numpad, F13-F24, media keys, lock LEDs); one
   event stream at `/osystem/devices/input`; `/osystem/devices` filesystem; QEMU run targets with USB,
   network and audio devices.
2. **Kernel services.** Memory mapping and shared memory, pipes (and `|` / `>` in
   the shell), local sockets, message passing, `poll`, signals, threads, FPU/SSE
   state per thread, kernel log buffer.
3. **Networking.** virtio-net and e1000 drivers; IPv4/IPv6, TCP, UDP, DHCP, DNS;
   socket API; TLS (vendored) for HTTPS, IMAPS and SMTPS.
4. **Graphics and windowing.** Compositor and window system, 2D graphics library,
   TrueType and Unicode text, image decoding, AUI markup and widget toolkit,
   desktop shell with taskbar and backgrounds, graphical boot screen.
5. **Audio and camera.** Intel HD Audio driver, audio server with per-app mixing
   and volume keys; USB Video Class camera driver.
6. **Users and language.** Per-user layout above, encrypted credentials,
   translations for the five languages, keyboard layouts, pinyin input method,
   keyboard-only navigation, high-contrast theme.
7. **Desktop basics.** Clipboard, drag and drop, notifications, file
   associations, recycle bin, lock screen, shutdown/restart UI, screenshots,
   search, workspaces, window snapping, command palette, guest account.
8. **App model.** `.aip` format and installer, permission tiers and sandbox,
   `powersudo`, suites as package groups.
9. **Apps.**
   - System: Terminal, File manager, Settings, Resource manager, Task manager, Feature manager
   - Administrative: Management console, User manager, Cron job manager
   - Default: Notepad, Calculator, Web browser, Clock/timer, Image viewer, Email, Audio player, Camera
   - Development: WindowBuilder, AppMaker, System debugger, System log viewer
10. **Upkeep.** Offline signed updates with rollback, crash reports in the log
    viewer, swap and low-memory handling.
11. **Later.** Recovery-mode GUI stub, real-hardware drivers (NICs, Wi-Fi, USB
    hubs, laptops), online updates.
