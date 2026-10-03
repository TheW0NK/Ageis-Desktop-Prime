# Aegis packages (.aip)

An `.aip` (Aegis installer package) holds an app's files and a manifest. The
system reads it; it is never run. Open one from Files, or use `aip` in the
terminal.

## Making one

Put the app's files and a `manifest` in a folder, then:

    tools/mkaip.py FOLDER OUTPUT.aip

`tools/samples/aip/dice` is a complete example (an AegisScript app).

### Manifest

One `key=value` per line.

| Key | Meaning |
|---|---|
| `id` | Lowercase letters, digits, `-` and `.`; at most 31 characters. Required. |
| `name` | Shown in the launcher. Required. |
| `version`, `publisher`, `description` | Shown before installing. |
| `exec` | The program and its arguments. Required. A relative path is inside the package; an absolute one is a system program (`/bin/apprun`). `%d` is the folder the app is installed in. |
| `icon` | A PNG in the package, or the name of a built-in icon. |
| `suite` | `Default`, `System`, `Administrative` or `Development`. |
| `opens` | File types it opens: `.txt;.md`. |
| `permissions` | Comma-separated: see below. |
| `command` | A command name put on everyone's `PATH` (`/apps/bin`) when installed for everyone. |
| `scope` | `user`, `machine` or `either` (default). |

### Permissions

| Tier | Ids | Who can grant |
|---|---|---|
| Basic | `appdata`, `documents`, `notifications`, `clipboard` | Anyone (granted by default) |
| Elevated | `network`, `camera`, `microphone`, `home`, `apps-data` | Administrators |
| System | `system-files`, `users`, `devices`, `startup` | Administrators, with their password |
| powersudo | `powersudo` | Administrators, with their password, each run |

The installer records what was granted (in the app's launcher entry and its
install record). Enforcing it is the sandbox's job, which is not built yet.

## Where things go

| | Just me | Everyone (administrator) |
|---|---|---|
| Files | `~/../system/appdata/apps/<id>` | `/apps/<id>` |
| Launcher entry | `system/appdata/applications/aip-<id>.app` | `/usr/share/applications/aip-<id>.app` |
| Install record | `system/appdata/aip/<id>` | `/var/lib/aip/<id>` |

The record keeps the manifest, the granted permissions, the file list and a
copy of the package, which **Repair** reinstalls from.

## Format

Little endian:

    "AEGISAIP"  u32 version (1)  u32 flags (0)  u32 manifest length  u32 file count
    manifest
    per file:   u16 path length  u16 0  u32 mode  u64 size  path  data
    SHA-256 of everything above (32 bytes)  "AIPEND\0\0"

Paths are relative, with no `.` or `..` parts. Packages are checked against
their SHA-256 but not yet signed; the installer says so.
