# Third-party code

| Directory / file | What | License | Source |
|---|---|---|---|
| `bearssl/` | BearSSL 0.6, TLS 1.0–1.2 and cryptography | MIT (`bearssl/LICENSE.txt`) | Ubuntu archive `bearssl_0.6+dfsg.1.orig.tar.xz` (upstream bearssl.org) |
| `musl-math/` | The math library (`src/math`) of musl 1.2.4, with small Aegis headers in `include/` | MIT (`musl-math/COPYRIGHT`) | Ubuntu archive `musl_1.2.4.orig.tar.gz` |
| `stb/` | stb_truetype, stb_image, stb_image_write, stb_rect_pack, stb_textedit, stb_vorbis | MIT or public domain (end of each file) | Ubuntu `libstb-dev` 0.0~git20230129 |
| `fonts/` | DejaVu Sans, Sans Bold, Sans Mono, Sans Mono Bold, Serif, Serif Bold 2.37 | Bitstream Vera / DejaVu license (`fonts/LICENSE-DejaVu.txt`) | Ubuntu `fonts-dejavu-core`, `fonts-dejavu-mono` |
| `ca-certificates.pem` | Mozilla root certificates, installed as `/etc/ssl/certs/ca-bundle.pem` | MPL 2.0 (Mozilla CA program data) | Ubuntu `ca-certificates` 20260601~24.04.1 |

Only `src/` and `inc/` of BearSSL are kept; they are unmodified. musl's math
sources are unmodified except that `libm.h` also includes `<features.h>`. Aegis-specific
build settings are passed as `-D` flags in `endpoint/Makefile`.
