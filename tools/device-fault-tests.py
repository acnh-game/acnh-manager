#!/usr/bin/env python3
"""Fault-injection tests for the installer, driven on the console.

The installer promises "either everything lands, or the card is left exactly as it was".  These
cases try to break that promise on purpose and then read the card back.  Every step proves
itself before it is allowed to move on:

  * the page is classified from a screenshot *before* a key is sent, so a key can never land on
    an unknown page;
  * the app's own log.txt is checked for the ``collect: gate=... plan=...`` line, so "the button
    that was pressed was really the enabled install button" is evidence, not an assumption;
  * the card is hashed before and after, so "nothing was left behind" is byte-level.

Prerequisites: the console reachable as ``switch`` (sys-agent on 6000, its FTP on 6001), the app
installed with its three files, and the console on the home menu.  Captures are written to
``build/scratch/device-fault-tests/``.

Usage:  python3 tools/device-fault-tests.py <t1a|t1b|t2|restore|all>
"""

import ftplib
import hashlib
import io
import json
import os
import subprocess
import sys
import time

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
AGENT = os.path.abspath(os.path.join(REPO, "..", "sys-agent", "client", "sysagent.py"))
OUT_DIR = os.path.join(REPO, "build", "scratch", "device-fault-tests")

FTP_HOST = "switch"
FTP_PORT = 6001

APP_DIR = "/switch/ACNH-Manager"
LOG_PATH = APP_DIR + "/log.txt"
STATE_PATH = APP_DIR + "/state.json"
EXEFS_DIR = "/atmosphere/contents/01006F8002326000/exefs"

GAME_FILES = ["subsdk9", "main.npdm", "acnh-agent.version"]

TEMP_SUFFIX = ".acnh-tmp"
OLD_SUFFIX = ".acnh-old"

# A marker appended to every installed file so that a rollback is distinguishable from a plain
# re-install of the same release: without it both outcomes leave identical bytes behind.
HAND_EDIT = b"\nacnh-manager device test: this file was edited by hand\n"


# --------------------------------------------------------------------------- console I/O

def agent(*args, timeout=40):
    return subprocess.run([sys.executable, AGENT, *args], capture_output=True, text=True,
                          timeout=timeout)


def shot(name):
    path = os.path.join(OUT_DIR, name)
    agent("screen", "capture", "--output", path)
    return path


def key(button, pause=1.0):
    agent("input", "click", button)
    time.sleep(pause)


def classify(path):
    """Which screen a capture shows: black / manager / sphaira / user-dialog / home."""
    im = Image.open(path).convert("RGB")
    top_left = im.getpixel((60, 20))
    top_mid = im.getpixel((640, 20))
    if max(im.getpixel((640, 360))) < 25 and max(top_mid) < 25:
        return "black"
    r, g, b = top_mid
    if g > 150 and b > 120 and r < 130:
        return "manager"
    if max(top_mid) < 70:
        return "sphaira"
    if 110 < top_left[0] < 190 and abs(top_left[0] - top_left[2]) < 30:
        return "user-dialog"
    return "home"


def app_page(path):
    """Which page of the app a capture shows, or the screen name when it is not the app.

    home / details / action / unknown.  "action" covers the confirmation and the result page --
    enough to tell "A opens a confirmation" from "A did nothing", which is what the cases need.
    """
    state = classify(path)
    if state != "manager":
        return state
    im = Image.open(path).convert("RGB")
    white = lambda p: min(p) > 225
    # The status tab badge stays bright on the confirmation and result pages too, so the big
    # action button is tested first: the status page's own button row (y=267..415) stops well
    # above y=603, while the confirmation and result pages draw their button across it.
    teal = im.getpixel((334, 603))
    if teal[0] < 120 and teal[1] > 150 and teal[2] > 120:
        return "action"
    if white(im.getpixel((1148, 47))):   # Ⓡ badge bright = details page
        return "details"
    if white(im.getpixel((1012, 47))):   # Ⓛ badge bright = status page
        return "home"
    return "unknown"


# --------------------------------------------------------------------------- card access

class Card:
    """The SD card, over sys-agent's FTP.  Missing paths come back as 450 on this daemon, so
    every call is written to answer "is it there?" instead of raising."""

    def __init__(self):
        self.ftp = ftplib.FTP()
        self.ftp.connect(FTP_HOST, FTP_PORT, timeout=20)
        self.ftp.login()

    def close(self):
        try:
            self.ftp.quit()
        except Exception:
            pass

    def listing(self, path):
        try:
            return sorted(self.ftp.nlst(path))
        except ftplib.all_errors:
            return None

    def read(self, path):
        chunks = []
        try:
            self.ftp.retrbinary("RETR " + path, chunks.append)
        except ftplib.all_errors:
            return None
        return b"".join(chunks)

    def write(self, path, data):
        self.ftp.storbinary("STOR " + path, io.BytesIO(data))

    def remove(self, path):
        """Delete whatever sits at `path`: file first, then directory (the two-step delete the
        installer itself uses).  True when the path is gone."""
        for attempt in (self.ftp.delete, self.ftp.rmd):
            try:
                attempt(path)
                return True
            except ftplib.error_perm:
                continue
        return False

    def remove_tree(self, path):
        """Delete `path`, recursing one level so a non-empty directory can be cleaned up.

        The daemon answers NLST with bare entry names, so children are rebuilt as absolute paths
        before being deleted."""
        for entry in self.listing(path) or []:
            name = entry.rstrip("/").rsplit("/", 1)[-1]
            if name not in (".", "..", ""):
                self.remove(path.rstrip("/") + "/" + name)
        return self.remove(path)

    def log_text(self):
        data = self.read(LOG_PATH)
        return None if data is None else data.decode("utf-8", "replace")

    def log_remove(self):
        try:
            self.ftp.delete(LOG_PATH)
        except ftplib.error_perm:
            pass

    def snapshot(self):
        """Hashes of the three game files plus the install record: the definition of "the card
        did not change"."""
        out = {"exefs": self.listing(EXEFS_DIR), "files": {}, "state": None}
        for name in GAME_FILES:
            data = self.read("%s/%s" % (EXEFS_DIR, name))
            out["files"][name] = None if data is None else (len(data),
                                                             hashlib.sha256(data).hexdigest())
        state = self.read(STATE_PATH)
        out["state"] = None if state is None else (len(state), hashlib.sha256(state).hexdigest())
        return out


def short(data):
    return "none" if data is None else hashlib.sha256(data).hexdigest()[:16]


def diff_snapshot(before, after):
    """Human-readable differences; an empty list means byte-identical."""
    out = []
    if before["exefs"] != after["exefs"]:
        out.append("exefs listing: %s -> %s" % (before["exefs"], after["exefs"]))
    for name in GAME_FILES:
        if before["files"][name] != after["files"][name]:
            out.append("%s: %s -> %s" % (name, before["files"][name], after["files"][name]))
    if before["state"] != after["state"]:
        out.append("state.json: %s -> %s" % (before["state"], after["state"]))
    return out


# --------------------------------------------------------------------------- navigation

def launch_app(card, tag):
    """Bring the manager to the foreground, proving that it really started.

    Key recipe, confirmed on hardware (the pause is the wait *after* each action):
        HOME(2s) DD(1s) DR(1s) DR(1s) DR(1s) A(2s) DD(1s) DR(1s) A(2s)
    sys-agent only knows the uppercase short names (DD/DR), and the album needs seconds to load:
    with shorter pauses the trailing directions are dropped and the recipe lands elsewhere.
    """
    card.log_remove()
    key("HOME", pause=2.0)
    key("DD", pause=1.0)
    key("DR", pause=1.0)
    key("DR", pause=1.0)
    key("DR", pause=1.0)
    key("A", pause=2.0)
    key("DD", pause=1.0)
    key("DR", pause=1.0)
    key("A", pause=2.0)
    page = app_page(shot("%s-launched.jpg" % tag))
    if page not in ("home", "details", "action", "unknown"):
        return "expected the app, found %s" % page
    if "ui: first frame done" not in (card.log_text() or ""):
        return "the app drew no frame (log.txt has no 'first frame done')"
    return None


def to_home(card, tag):
    """Walk the app back to its status page, one screenshot per attempt."""
    for attempt in range(6):
        page = app_page(shot("%s-nav%d.jpg" % (tag, attempt)))
        if page == "home":
            return None
        if page == "details":
            key("DL", pause=2.0)      # L = status page
        elif page == "action":
            key("B", pause=2.0)       # B = back on those pages
        else:
            return "not inside the app (%s)" % page
    return "could not reach the app's home page"


def exit_app(card, tag):
    error = to_home(card, tag)
    if error:
        return error
    key("B", pause=7.0)
    state = classify(shot("%s-exited.jpg" % tag))
    if state != "home":
        return "expected the home menu after exit, found %s" % state
    return None


def plan_line(text):
    for line in (text or "").splitlines():
        if line.startswith("collect: gate="):
            return line
    return None


def cleanup_line(text):
    for line in (text or "").splitlines():
        if line.startswith("cleanup: "):
            return line
    return None


def press_install(card, tag, on_step):
    """Drive one install attempt with a screenshot in front of every key.

    The home page must be the status page *and* its log must already say
    ``gate=supported ... plan=repair``: that is the proof that the button about to be pressed is
    the enabled install/repair one and not the disabled "already up to date".
    """
    page = app_page(shot("%s-home.jpg" % tag))
    on_step("home", page)
    if page != "home":
        return {"error": "expected the app's home page, found %s" % page}
    line = plan_line(card.log_text() or "")
    if line is None:
        return {"error": "the app logged no collect line, so the install button is not proven "
                         "available"}
    if "gate=supported" not in line:
        return {"error": "install is not available: %s" % line}
    if "plan=repair" not in line:
        return {"error": "the edited record did not put the plan in repair: %s" % line}

    key("A", pause=3.0)
    confirm = app_page(shot("%s-confirm.jpg" % tag))
    on_step("confirm", confirm)
    if confirm != "action":
        return {"error": "expected the confirmation page, found %s" % confirm}

    key("A", pause=5.0)
    result = None
    for _ in range(5):
        result = app_page(shot("%s-result.jpg" % tag))
        if result == "action":
            break
        time.sleep(2.0)
    on_step("result", result)
    if result != "action":
        return {"error": "the app is not showing a result page (%s)" % result}
    return {"plan": line}


# --------------------------------------------------------------------------- injections

def temp_path(name):
    return "%s/%s%s" % (EXEFS_DIR, name, TEMP_SUFFIX)


def clear_injections(card):
    """Remove every path any case can inject, so a case never runs on top of the previous one's
    injection -- the first T2 run did exactly that and failed for the wrong reason."""
    removed = []
    for name in GAME_FILES + ["state.json"]:
        base = STATE_PATH if name == "state.json" else "%s/%s" % (EXEFS_DIR, name)
        for suffix in (TEMP_SUFFIX, OLD_SUFFIX):
            path = base + suffix
            if card.read(path) is not None or card.listing(path) is not None:
                if card.remove_tree(path):
                    removed.append(path)
    return removed


def inject_dir(card, path, child=None):
    """Put a directory where the installer expects a file.  `child` adds a file inside, which
    makes the directory impossible to remove with a plain "delete directory" call."""
    card.remove_tree(path)
    card.ftp.mkd(path)
    if child:
        card.write("%s/%s" % (path, child), b"x")


def pin_record_mismatch(card):
    """Make the plan become "repair" without touching the installed files.

    The plan is decided by comparing the *record* against the release manifest, so rewriting one
    recorded digest is enough to turn the home screen into its repair state -- which is what
    gives the harness an enabled primary button.  The installed files stay as they are, so a
    rollback has something real to restore.
    """
    state = json.loads(card.read(STATE_PATH).decode("utf-8"))
    entry = state["files"][-1]
    original = entry["sha256"]
    entry["sha256"] = ("0" if original[0] != "0" else "1") + original[1:]
    card.write(STATE_PATH, json.dumps(state, separators=(",", ":")).encode("utf-8"))
    return original, entry["sha256"]


def hand_edit_installed_files(card):
    """Append a marker to every installed file, so the bytes before an attempt are unique."""
    out = []
    for name in GAME_FILES:
        path = "%s/%s" % (EXEFS_DIR, name)
        data = card.read(path)
        if data is None:
            return "FAIL: %s is missing, cannot edit it" % name
        card.write(path, data + HAND_EDIT)
        out.append("%s %d -> %d bytes" % (name, len(data), len(data) + len(HAND_EDIT)))
    return "; ".join(out)


def record_mismatches(card):
    """After a successful install the record must describe the bytes that are really there."""
    state = json.loads(card.read(STATE_PATH).decode("utf-8"))
    problems = []
    for entry in state["files"]:
        name = entry["target"].rsplit("/", 1)[-1]
        data = card.read("%s/%s" % (EXEFS_DIR, name))
        if data is None:
            problems.append("%s missing" % name)
            continue
        actual = hashlib.sha256(data).hexdigest().upper()
        if actual != entry["sha256"].upper() or len(data) != entry["size"]:
            problems.append("%s: card=%s record=%s" % (name, actual[:16], entry["sha256"][:16]))
    return problems


def residue(card):
    """Installer leftovers still sitting in the game directory."""
    return [n.rsplit("/", 1)[-1] for n in (card.listing(EXEFS_DIR) or []) if ".acnh-" in n]


# --------------------------------------------------------------------------- cases

def run_case(card, tag, injection, expect_ok, expect_cleanup=None):
    print("[%s] injection: %s" % (tag, injection["what"]))
    page = app_page(shot("%s-pre.jpg" % tag))
    if page in ("home", "details", "action", "unknown"):
        error = exit_app(card, tag)
        if error:
            print("[%s] FAIL prelude: %s" % (tag, error))
            return False
    cleared = clear_injections(card)
    if cleared:
        print("[%s] cleared earlier injections: %s" % (tag, ", ".join(cleared)))
    injection["apply"]()
    print("[%s] injected" % tag)
    print("[%s] installed files edited by hand: %s" % (tag, hand_edit_installed_files(card)))
    original, edited = pin_record_mismatch(card)
    print("[%s] record edited so the plan becomes repair: %s -> %s"
          % (tag, original[:16], edited[:16]))

    error = launch_app(card, tag)
    if error:
        print("[%s] FAIL launch: %s" % (tag, error))
        return False
    print("[%s] launched (log.txt has 'first frame done')" % tag)
    text = card.log_text() or ""
    line = cleanup_line(text)
    print("[%s] startup cleanup line: %s" % (tag, line if line else "(none)"))
    if expect_cleanup is not None and (line is None or expect_cleanup not in line):
        print("[%s] FAIL cleanup line does not contain %r" % (tag, expect_cleanup))
        return False
    collect = plan_line(text)
    print("[%s] startup collect line: %s" % (tag, collect))
    if collect is None or "plan=repair" not in collect:
        print("[%s] FAIL the app did not plan a repair" % tag)
        return False

    error = to_home(card, tag)
    if error:
        print("[%s] FAIL navigation: %s" % (tag, error))
        return False
    before = card.snapshot()
    print("[%s] before install: exefs=%s state=%s" % (tag, before["exefs"], sha16(card)))
    steps = []
    result = press_install(card, tag, lambda name, page: steps.append(name))
    if "error" in result:
        print("[%s] FAIL driving the app: %s" % (tag, result["error"]))
        return False
    print("[%s] collect line: %s" % (tag, result["plan"]))
    print("[%s] pages: %s" % (tag, " -> ".join(steps)))
    after = card.snapshot()
    diff = diff_snapshot(before, after)
    print("[%s] card changed by the install: %s" % (tag, diff if diff else "nothing"))

    ok = True
    if expect_ok:
        if diff == []:
            print("[%s] FAIL expected a successful install, the card is unchanged" % tag)
            ok = False
        leftovers = residue(card)
        print("[%s] leftovers in exefs: %s" % (tag, leftovers if leftovers else "none"))
        if leftovers:
            print("[%s] FAIL a leftover survived a successful install" % tag)
            ok = False
        problems = record_mismatches(card)
        print("[%s] record vs card: %s"
              % (tag, problems if problems else "every file matches its recorded digest"))
        if problems:
            print("[%s] FAIL the record does not describe the installed files" % tag)
            ok = False
    else:
        if diff:
            print("[%s] FAIL the failed install changed the card: %s" % (tag, diff))
            ok = False
        else:
            print("[%s] the card is byte-identical to its pre-install state" % tag)
        pre = set(n.rsplit("/", 1)[-1] for n in before["exefs"] or [])
        post = set(n.rsplit("/", 1)[-1] for n in after["exefs"] or [])
        strays = sorted(n for n in post - pre if ".acnh-" in n)
        print("[%s] new leftovers from the failed install: %s"
              % (tag, strays if strays else "none"))
        if strays:
            print("[%s] FAIL the failed install left %s" % (tag, strays))
            ok = False
    return ok


def sha16(card):
    return short(card.read(STATE_PATH))


def case_t1a(card):
    """T1a: an empty leftover directory sits on the temp path (the shape an interrupted install
    left behind on hardware).  Startup cleanup must remove it and the install must then work."""
    path = temp_path("main.npdm")
    return run_case(card, "t1a",
                    {"what": "empty directory at exefs/main.npdm.acnh-tmp",
                     "apply": lambda: inject_dir(card, path)},
                    expect_ok=True, expect_cleanup="main.npdm.acnh-tmp")


def case_t1b(card):
    """T1b: a leftover that refuses to go away (a non-empty directory).  The install must be
    refused, and the card must not change at all."""
    path = temp_path("main.npdm")
    return run_case(card, "t1b",
                    {"what": "non-empty directory at exefs/main.npdm.acnh-tmp",
                     "apply": lambda: inject_dir(card, path, child="child.bin")},
                    expect_ok=False, expect_cleanup="could not remove")


def case_t2(card):
    """T2: the install record cannot be written (a non-empty directory on its temp path) while
    the three game files can.  All three must be rolled back to their previous bytes."""
    return run_case(card, "t2",
                    {"what": "non-empty directory at /switch/ACNH-Manager/state.json.acnh-tmp",
                     "apply": lambda: inject_dir(card, STATE_PATH + TEMP_SUFFIX, child="child.bin")},
                    expect_ok=False)


def case_restore(card):
    """Remove every injection and prove that a normal install works again."""
    print("[restore] removing injections")
    clear_injections(card)
    print("[restore] injections left on the card: %s" % (residue(card) or "none"))
    return run_case(card, "restore", {"what": "no injection", "apply": lambda: None},
                    expect_ok=True)


CASES = {"t1a": case_t1a, "t1b": case_t1b, "t2": case_t2, "restore": case_restore}


def main():
    if len(sys.argv) < 2 or (sys.argv[1] not in CASES and sys.argv[1] != "all"):
        print(__doc__)
        print("commands: %s, all" % ", ".join(sorted(CASES)))
        return 2
    os.makedirs(OUT_DIR, exist_ok=True)
    names = ["t1a", "t1b", "t2", "restore"] if sys.argv[1] == "all" else [sys.argv[1]]
    card = Card()
    failed = []
    try:
        for name in names:
            if not CASES[name](card):
                failed.append(name)
            print("")
    finally:
        card.close()
    print("cases run: %s" % ", ".join(names))
    print("cases failed: %s" % (", ".join(failed) if failed else "none"))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
