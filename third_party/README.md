# Third-party code

| Directory / file | What | License | Source |
|---|---|---|---|
| `bearssl/` | BearSSL 0.6, TLS 1.0–1.2 and cryptography | MIT (`bearssl/LICENSE.txt`) | Ubuntu archive `bearssl_0.6+dfsg.1.orig.tar.xz` (upstream bearssl.org) |
| `ca-certificates.pem` | Mozilla root certificates, installed as `/etc/ssl/certs/ca-bundle.pem` | MPL 2.0 (Mozilla CA program data) | Ubuntu `ca-certificates` 20260601~24.04.1 |

Only `src/` and `inc/` of BearSSL are kept; they are unmodified. Aegis-specific
build settings are passed as `-D` flags in `endpoint/Makefile`.
