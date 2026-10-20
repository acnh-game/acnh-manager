# Third-party code in this directory

Every library here is vendored as **whole upstream files, unmodified**, with the exact revision
they came from.  Nothing in this directory may be edited locally: a fix belongs upstream or in a
wrapper next to our own code (see `source/net/http_server.*` and `source/util/qr_code.*`).
`tools/verify-release.py` checks these sha256 values, so a silent edit fails the release check.

| Directory | Files | Upstream | Commit | Fetched | License |
|---|---|---|---|---|---|
| `picohttpparser/` | `picohttpparser.h`, `picohttpparser.c` | https://github.com/h2o/picohttpparser | `465a7ff09fbd3432fe56c673451f5460154d1f07` | 2026-10-12 | MIT **or** Perl (we take MIT) |
| `qrcodegen/` | `qrcodegen.h`, `qrcodegen.c` | https://github.com/nayuki/QR-Code-generator (`c/`) | `3c6d0b3cefb4e049dc337e82237c9644399716a8` | 2026-10-12 | MIT |

## sha256

```
1fc9074dd12418b2b91e55ef3a8279bccf8d3577b790dea6f2f908ab2a157f1c  picohttpparser/picohttpparser.h
6480689711996bc9a219e26df27ac32ebbb7dce2f24a63a316d041b08f646199  picohttpparser/picohttpparser.c
e82df4bff37d18b5863b9e7486fe6bda1b6cda8c3b9ecebfec473907265cb589  qrcodegen/qrcodegen.h
6a2b9cc65176f2345dde260c74b6d352627e8a0a6385d086ae0e9c5d0913c70c  qrcodegen/qrcodegen.c
```

## Why these two

**picohttpparser** parses the request line and headers of the upload page's requests.  It is
~810 lines, depends only on `<stdint.h>` / `<sys/types.h>` / `<string.h>` and compiles as-is for
devkitA64.  Everything else of the HTTP server (accept loop, routing, body streaming, lifetime)
is ours: the heavier alternatives do not build here.  Measured on the pinned image:
`cpp-httplib` needs `<ifaddrs.h>`, `<sys/mman.h>`, `<sys/un.h>` and `timegm`/`accept4` from libc,
none of which the Switch toolchain has; `civetweb` needs `<sys/utsname.h>`; `mongoose` is
GPLv2-only, which cannot be linked into this GPLv3 project; `tinyhttp` reads the whole request
body into memory, which is exactly what a 4.65 GB upload must not do.

**qrcodegen** turns the `http://<ip>:<port>` address into the QR code on the install page.  The
**C** flavour is vendored rather than the C++ one: this project builds with `-fno-exceptions
-fno-rtti`, and the C++ version throws.  The C version allocates nothing, needs no C++ runtime
and is called from `source/util/qr_code.cpp`, which owns the buffers.

## Embedded keep-awake media

The page's keep-awake fallback uses the two short silent WebM/MP4 data URIs from
[NoSleep.js](https://github.com/richtr/NoSleep.js) **v0.12.0** (commit
`1d15cc88dd8c1b62fe5b5f910c6ff7396b280d68`, 2020-12-16).  No library code is vendored; the page
implements only the native Wake Lock attempt, the video playback/random-seek loop and the
page-lifecycle handling it needs.  The upstream license is MIT and is reproduced in
`nosleep-media-LICENSE` next to this file.

## Updating

1. Fetch the file from a **pinned commit** (never `master`) and record that commit in the table
   above.
2. Recompute the sha256 values and update both the table's date and this file's block.
3. Update the same values in `tools/verify-release.py`.
4. Read the upstream diff before accepting it: these libraries run on untrusted input (a browser
   on the local network for the parser; the console's own address for the encoder).
