#!/usr/bin/env python3
"""Play the installer page's role from a terminal: feed one package to the console over HTTP.

The console serves the real page to a browser; this script makes the same three calls that page's
JavaScript makes, so the upload path can be tested (and debugged) without a phone in the loop:

    tools/upload-client.py <package> [--host switch] [--port 4321] [--verbose]

It posts the file's name and size (`POST /upload/begin`), then loops: ask which range the console
wants (`GET /upload/next`), slice exactly that out of the file and post it (`POST /upload/part`).
The console shows its confirm screen first -- press A there to start the install -- and this loop
keeps serving ranges until the console reports the run is over.

Standard library only.  Stop it with Ctrl-C; a console that is not serving answers nothing, and the
script says so instead of retrying forever."""

from __future__ import annotations

import argparse
import json
import pathlib
import socket
import sys
import time
import urllib.error
import urllib.request

DEFAULT_TIMEOUT = 20.0


def call(base: str, method: str, path: str, body: bytes | None = None,
         timeout: float = DEFAULT_TIMEOUT) -> tuple[int, dict | str]:
    request = urllib.request.Request(base + path, data=body, method=method)
    if body is not None:
        request.add_header("Content-Type", "application/octet-stream")
    try:
        with urllib.request.urlopen(request, timeout=timeout) as answer:
            text = answer.read().decode("utf-8", "replace")
            try:
                return answer.status, json.loads(text)
            except json.JSONDecodeError:
                return answer.status, text
    except urllib.error.HTTPError as error:
        text = error.read().decode("utf-8", "replace")
        try:
            return error.code, json.loads(text)
        except json.JSONDecodeError:
            return error.code, text
    except (urllib.error.URLError, socket.timeout, ConnectionError) as error:
        raise SystemExit(f"the console did not answer ({error}); is the install page open?")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("package", type=pathlib.Path, help="the file to upload")
    parser.add_argument("--host", default="switch", help="console host or address (default: switch)")
    parser.add_argument("--port", type=int, default=4321, help="the console's page port (default: 4321)")
    parser.add_argument("--verbose", action="store_true", help="print every range")
    parser.add_argument("--task", default="", help="serve this task id instead of starting a new one "
                                                  "(it is in /status.json, and a page that reloaded "
                                                  "uses it to keep feeding the same install)")
    args = parser.parse_args()

    package = args.package.expanduser()
    if not package.is_file():
        print(f"no such file: {package}", file=sys.stderr)
        return 2
    size = package.stat().st_size
    base = f"http://{args.host}:{args.port}"

    if args.task:
        task = args.task
    else:
        status, answer = call(base, "POST", "/upload/begin",
                              json.dumps({"name": package.name, "size": size}).encode())
        if status != 200 or not isinstance(answer, dict) or "taskId" not in answer:
            print(f"the console refused this file: {status} {answer}", file=sys.stderr)
            return 1
        task = answer["taskId"]
    print(f"serving {package.name} ({size} bytes) as task {task}")
    print("press A on the console's confirm screen to start the install")

    posted = 0
    started = time.time()
    with package.open("rb") as handle:
        while True:
            status, answer = call(base, "GET", f"/upload/next?taskId={task}")
            if args.verbose:
                print(f"  next -> {status} {answer}")
            if status == 409:
                code = answer.get("code") if isinstance(answer, dict) else ""
                if code == "no-task" and time.time() - started < 10:
                    # The console may not have picked the file up yet (it opens the package on its
                    # next frame): wait a moment instead of giving up on it.
                    time.sleep(0.2)
                    continue
                print(f"the console is not taking parts: {answer}", file=sys.stderr)
                return 1
            if isinstance(answer, dict) and answer.get("wait"):
                # Nothing wanted this instant: the package is on the confirm screen, being read, or
                # the run is over.  The status endpoint says which.
                _, status_answer = call(base, "GET", "/status.json")
                state = status_answer.get("state") if isinstance(status_answer, dict) else "?"
                if state in ("done", "failed"):
                    print(f"the run is {state}: {status_answer.get('message', '')}")
                    return 0 if state == "done" else 1
                time.sleep(0.2)
                continue
            offset = int(answer["offset"])
            length = int(answer["size"])
            handle.seek(offset)
            chunk = handle.read(length)
            if len(chunk) != length:
                print(f"the console asked past the end of the file ({offset}+{length})", file=sys.stderr)
                return 1
            status, answer = call(base, "POST", f"/upload/part?taskId={task}&offset={offset}", chunk)
            if status != 200:
                print(f"part refused: {status} {answer}", file=sys.stderr)
                return 1
            posted += len(chunk)
            if args.verbose:
                print(f"  sent {len(chunk)} bytes at {offset} ({posted}/{size})")
            elif posted % (32 * 1024 * 1024) < len(chunk):
                print(f"  {posted * 100 // size}% ({posted}/{size})")


if __name__ == "__main__":
    raise SystemExit(main())
