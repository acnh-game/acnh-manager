#!/usr/bin/env python3
"""Serve one package file over HTTP for the installer's device tests (T4 / T37 / T38).

The download source's contract is narrow and worth testing exactly: it probes with `Range: 0-0`
and keeps the address **only if the answer is 206 with a `Content-Range` naming the total size**;
every byte after that is another exact range request.  This server implements that contract, and
can also break it on purpose, because the interesting cases are the refusals:

    tools/serve-package.py <package> --port 8000              # ranges: the "it works" case (T4)
    tools/serve-package.py <package> --no-range               # 200 + whole file: T37
    tools/serve-package.py <package> --status 404             # the address is wrong: T38
    tools/serve-package.py --redirect <url> --port 8000       # 302 to it: lets a short address
                                                              # (typed on the console) reach a long
                                                              # one, e.g. your own https server

It prints the URLs to type on the console (every non-loopback address of this machine, since the
console needs one it can reach) and logs each request with its status and byte count, so the
console's screen can be checked against what the server actually did.

Standard library only.  Run it from the repository root; stop it with Ctrl-C."""

from __future__ import annotations

import argparse
import http.server
import pathlib
import re
import socket
import sys

RANGE_RE = re.compile(r"^bytes=(\d+)-(\d*)$")


class Handler(http.server.BaseHTTPRequestHandler):
    server_version = "serve-package"

    # Set by main().  Not named `path`/`size`: BaseHTTPRequestHandler already owns `path` (the
    # request's path) and an attribute of ours on the instance would shadow it.
    file_path: pathlib.Path
    file_size: int
    mode: str  # "range" | "no-range" | "status" | "redirect"
    forced_status: int
    redirect_to: str

    def log_message(self, fmt: str, *args: object) -> None:  # noqa: A003 (stdlib signature)
        sys.stderr.write("[serve-package] %s\n" % (fmt % args))

    def do_HEAD(self) -> None:  # noqa: N802 (stdlib signature)
        self.answer(send_body=False)

    def do_GET(self) -> None:  # noqa: N802 (stdlib signature)
        self.answer(send_body=True)

    def answer(self, send_body: bool) -> None:
        if self.mode == "redirect":
            # Everything redirects to `--redirect`: libcurl follows it and re-sends the Range
            # header, so a short address that is easy to type on the console can exercise a target
            # that would be painful to type (an https URL with a long percent-encoded path).
            self.send_response(302)
            self.send_header("Location", self.redirect_to)
            self.send_header("Content-Length", "0")
            self.end_headers()
            self.log_message("GET %s -> 302 %s", self.path, self.redirect_to)
            return
        if self.mode == "status":
            body = b"serve-package: forced status\n"
            self.send_response(self.forced_status)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            if send_body:
                self.wfile.write(body)
            return

        # `/` serves the package as well as its own file name: the address typed on the console
        # then needs digits, a dot and a colon only -- no letters at all, which is the difference
        # between a one-minute test and a five-minute one with the d-pad keyboard.
        if self.path.split("?")[0] not in ("/", "/" + self.file_path.name):
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.end_headers()
            self.log_message("GET %s -> 404 (only / and /%s are served)", self.path,
                             self.file_path.name)
            return

        header = self.headers.get("Range", "")
        match = RANGE_RE.match(header) if self.mode == "range" else None
        if match is None:
            # Either the client did not ask for a range, or this run is the "no range" one: answer
            # 200 with the whole file, which is what a range-less server does.
            self.send_response(200)
            self.send_header("Content-Length", str(self.file_size))
            self.send_header("Content-Type", "application/octet-stream")
            self.end_headers()
            if send_body:
                self.copy(0, self.file_size)
            self.log_message("GET %s -> 200 whole file (%d bytes)", self.path, self.file_size)
            return

        first = int(match.group(1))
        last = int(match.group(2)) if match.group(2) else self.file_size - 1
        if first >= self.file_size or last < first:
            self.send_response(416)
            self.send_header("Content-Range", "bytes */%d" % self.file_size)
            self.send_header("Content-Length", "0")
            self.end_headers()
            self.log_message("GET %s -> 416 (range %d-%d of %d)", self.path, first, last,
                             self.file_size)
            return
        last = min(last, self.file_size - 1)
        count = last - first + 1
        self.send_response(206)
        self.send_header("Content-Range", "bytes %d-%d/%d" % (first, last, self.file_size))
        self.send_header("Content-Length", str(count))
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Type", "application/octet-stream")
        self.end_headers()
        if send_body:
            self.copy(first, count)
        self.log_message("GET %s -> 206 %d-%d/%d", self.path, first, last, self.file_size)

    def copy(self, offset: int, count: int) -> None:
        with self.file_path.open("rb") as handle:
            handle.seek(offset)
            left = count
            while left > 0:
                chunk = handle.read(min(left, 1024 * 1024))
                if not chunk:
                    break
                self.wfile.write(chunk)
                left -= len(chunk)


def local_addresses(port: int) -> list[str]:
    """Every address this machine answers on: the console needs one it can route to, and which one
    that is depends on whether both are on the same Wi-Fi or on a phone hotspot."""
    addresses = set()
    try:
        hostname = socket.gethostname()
        for info in socket.getaddrinfo(hostname, None, socket.AF_INET):
            addresses.add(info[4][0])
    except OSError:
        pass
    try:
        # The address the default route would use, which is usually the useful one.
        probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            probe.connect(("8.8.8.8", 80))
            addresses.add(probe.getsockname()[0])
        finally:
            probe.close()
    except OSError:
        pass
    return sorted(address for address in addresses if not address.startswith("127."))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("package", type=pathlib.Path, nargs="?",
                        help="the file to serve (not needed with --redirect)")
    parser.add_argument("--port", type=int, default=8000)
    parser.add_argument("--bind", default="0.0.0.0")
    parser.add_argument("--no-range", action="store_true",
                        help="ignore Range and always answer 200 with the whole file (T37)")
    parser.add_argument("--status", type=int, default=0,
                        help="answer every request with this status (T38, e.g. 404)")
    parser.add_argument("--redirect", metavar="URL", default="",
                        help="answer every request with 302 to URL: lets a short, typeable "
                             "address reach a target that is not (TLS, long paths)")
    args = parser.parse_args()

    if args.redirect:
        # Nothing is served from disk in this mode, so no file is needed: any path redirects.
        Handler.file_path = pathlib.Path("/dev/null")
        Handler.file_size = 0
    else:
        if args.package is None:
            print("give a package file, or use --redirect", file=sys.stderr)
            return 2
        if not args.package.is_file():
            print("not a file: %s" % args.package, file=sys.stderr)
            return 2
        Handler.file_path = args.package.resolve()
        Handler.file_size = args.package.stat().st_size
    if args.redirect:
        Handler.mode = "redirect"
        Handler.redirect_to = args.redirect
    elif args.status:
        Handler.mode = "status"
        Handler.forced_status = args.status
    elif args.no_range:
        Handler.mode = "no-range"
    else:
        Handler.mode = "range"

    server = http.server.ThreadingHTTPServer((args.bind, args.port), Handler)
    if args.redirect:
        print("mode redirect -> %s" % args.redirect)
    else:
        print("serving %s (%d bytes) in %s mode" % (args.package.name, Handler.file_size,
                                                    Handler.mode))
    addresses = local_addresses(args.port)
    if args.redirect:
        # The address to type on the console: shortest possible, any path redirects.
        print("type this on the console:")
        for address in addresses:
            print("  %s:%d" % (address, args.port))
    else:
        for address in addresses:
            print("  http://%s:%d/%s" % (address, args.port, args.package.name))
    if not addresses:
        print("  (no non-loopback address found: check the machine's network)")
    print("stop with Ctrl-C")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print()
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
