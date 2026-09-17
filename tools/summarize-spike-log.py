#!/usr/bin/env python3
"""解析 M0 spike 日志,打印一份可复核的摘要。

纯标准库;针对 /switch/ACNH-Manager/spike.log 的格式:
    HOS / targets 头、ns content meta 列表、exefs 覆盖快照、dmnt:cht、fsp-ldr 组合。

用法:
    python3 tools/summarize-spike-log.py build/scratch/spike.log

判定:
    - 主判据是 ncm:update 的 latest key 版本 == 2228224 且其内容表里 type=1(Program)的
      content id == E10617820DB06889E1638499478DA0DE;两者都可用 --expect-version /
      --expect-program-content-id 覆盖;
    - dmnt:cht 的 ModuleId(游戏在跑时)作为字节级复核,默认期望
      FF1D1C05670DB6021C85B624A710B963,可用 --expect-id 覆盖;
    - fsp-ldr 相关行只作为历史信息打印:该路径在实测中被 Atmosphere 拒绝,不参与判定。
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys

DEFAULT_EXPECT_MDID = "FF1D1C05670DB6021C85B624A710B963".lower()
DEFAULT_EXPECT_VERSION = 2228224
DEFAULT_EXPECT_PROGRAM_CONTENT_ID = "e10617820db06889e1638499478da0de"
DEFAULT_EXPECT_NPDM = "0fc17cae37a3ff9337301b30cc2dc8a785248b0a0011503ca197f46d996e6a94"

# 标签形如 "code[base/patch-storage attr=None]",内部含空格,所以按第一个 ": " 切分。
# printf 的 %#x 对 0 不输出 "0x" 前缀,所以结果码要同时接受 0x… 与纯十六进制。
RE_CALL = re.compile(r"^(?P<label>.+?): fsldrOpenCodeFileSystem rc=(?P<rc>(?:0x)?[0-9a-f]+)")
RE_MODULE = re.compile(r"^(?P<label>.+?): /main NSO0 module_id=(?P<id>[0-9A-Fa-f]+)")
RE_NPDM = re.compile(r"^(?P<label>.+?): /main\.npdm size=(?P<size>\d+) sha256=(?P<sha>[0-9a-f]+)")
RE_ROOT = re.compile(r"^(?P<label>.+?): code fs root: (?P<count>\d+) entries: (?P<names>.*)$")
RE_META = re.compile(r"^\s+meta\[(?P<i>\d+)\] type=(?P<type>\S+)\((?P<typec>[0-9a-fx]+)\) "
                     r"storage=(?P<storage>\d+) version=(?P<version>\d+)\((?P<versionhex>0x[0-9a-f]+)\) "
                     r"app_id=(?P<app>[0-9A-F]+)")
RE_DMNT = re.compile(r"^dmnt:cht: program_id=(?P<program>[0-9A-F]+) process_id=(?P<pid>\d+) "
                     r"module_id=(?P<id>[0-9A-Fa-f]+)")
RE_NCM_KEY = re.compile(r"^ncm\[(?P<label>[^\]]+)\]: latest key rc=(?P<rc>(?:0x)?[0-9a-f]+) "
                        r"id=(?P<id>[0-9A-F]+) version=(?P<version>\d+)\((?P<versionhex>(?:0x)?[0-9a-f]+)\) "
                        r"type=(?P<type>0x[0-9a-f]+) install=(?P<install>\d+)")
RE_NCM_CONTENT = re.compile(r"^\s+content\[(?P<i>\d+)\] id=(?P<id>[0-9A-Fa-f]+) type=(?P<type>\d+) "
                            r"id_offset=(?P<offset>\d+)")


def id_matches(actual: str, expect: str) -> bool:
    """ModuleId 的 0x20 字节里只有前 16 字节有效(其余补零),按前缀比较。"""
    a, e = actual.lower(), expect.lower()
    return a.startswith(e) or e.startswith(a)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=pathlib.Path)
    parser.add_argument("--expect-id", default=DEFAULT_EXPECT_MDID)
    parser.add_argument("--expect-version", type=int, default=DEFAULT_EXPECT_VERSION)
    parser.add_argument("--expect-program-content-id", default=DEFAULT_EXPECT_PROGRAM_CONTENT_ID)
    parser.add_argument("--expect-npdm-sha256", default=DEFAULT_EXPECT_NPDM)
    args = parser.parse_args(argv)

    if not args.log.is_file():
        print(f"error: {args.log} not found", file=sys.stderr)
        return 1
    lines = args.log.read_text(encoding="utf-8", errors="replace").splitlines()
    if not lines:
        print("log is empty: the app did not write anything (check that the build in use "
              "sets the file size before writing)")
        return 2

    expect_id = args.expect_id.lower()
    calls: list[tuple[str, str]] = []
    module_ids: dict[str, str] = {}
    npdms: dict[str, tuple[int, str]] = {}
    roots: dict[str, tuple[int, str]] = {}
    metas: list[str] = []
    dmnt: list[str] = []
    head: list[str] = []
    ncm_keys: list[re.Match[str]] = []
    ncm_contents: dict[str, list[re.Match[str]]] = {}
    current_ncm_label: str | None = None

    for line in lines:
        if line.startswith(("HOS ", "targets:", "nsInitialize", "application running",
                            "nsListApplicationContentMetaStatus", "exefs override",
                            "fsldrInitialize", "m0:", "=== ", "log:")):
            head.append(line)
        if m := RE_META.match(line):
            metas.append(line.strip())
        if m := RE_CALL.match(line):
            calls.append((m["label"], m["rc"]))
        if m := RE_MODULE.match(line):
            module_ids[m["label"]] = m["id"].lower()
        if m := RE_NPDM.match(line):
            npdms[m["label"]] = (int(m["size"]), m["sha"])
        if m := RE_ROOT.match(line):
            roots[m["label"]] = (int(m["count"]), m["names"])
        if m := RE_DMNT.match(line):
            dmnt.append(line)
            module_ids["dmnt:cht"] = m["id"].lower()
        if m := RE_NCM_KEY.match(line):
            ncm_keys.append(m)
            current_ncm_label = m["label"]
            ncm_contents.setdefault(m["label"], [])
        elif current_ncm_label and (m := RE_NCM_CONTENT.match(line)):
            ncm_contents[current_ncm_label].append(m)

    print("## header")
    for line in head:
        print("  " + line)

    print("\n## ns content meta")
    for line in metas:
        print("  " + line)

    print("\n## fsp-ldr calls")
    for label, rc in calls:
        mark = "OK " if rc == "0x0" else "   "
        print(f"  {mark}{label}: rc={rc}")
    ok = [label for label, rc in calls if rc == "0x0"]
    print(f"\n  successful combinations ({len(ok)}): {', '.join(ok) if ok else 'none'}")

    print("\n## code FS contents / ModuleId / NPDM")
    for label in sorted(set(module_ids) | set(npdms) | set(roots)):
        count, names = roots.get(label, (0, ""))
        mid = module_ids.get(label, "-")
        npdm = npdms.get(label)
        npdm_text = f"size={npdm[0]} sha256={npdm[1]}" if npdm else "-"
        id_ok = "MATCH" if id_matches(mid, expect_id) else "DIFFERENT"
        print(f"  {label}")
        print(f"    root entries ({count}): {names}")
        print(f"    module_id: {mid}")
        print(f"      first 8 bytes (cheat 口径): {mid[:16].upper()}  [{id_ok} vs expected {expect_id.upper()}]")
        print(f"    main.npdm: {npdm_text}"
              + (f"  [MATCH expected {args.expect_npdm_sha256[:16]}…]"
                 if npdm and npdm[1] == args.expect_npdm_sha256 else ""))

    print("\n## dmnt:cht")
    for line in dmnt or ["  (not present: game was not running during the run)"]:
        print("  " + line)

    print("\n## ncm")
    version_ok = False
    content_ok = False
    for m in ncm_keys:
        label = m["label"]
        contents = ncm_contents.get(label, [])
        print(f"  [{label}] id={m['id']} version={m['version']}({m['versionhex']}) type={m['type']}")
        for c in contents:
            print(f"    content type={c['type']} id={c['id'].upper()}")
        if label == "update":
            version_ok = int(m["version"]) == args.expect_version
            program = [c for c in contents if c["type"] == "1"]
            content_ok = any(c["id"].lower() == args.expect_program_content_id
                             for c in program)
            print(f"    version == {args.expect_version}: {'MATCH' if version_ok else 'DIFFERENT'}")
            print(f"    program content id == {args.expect_program_content_id.upper()}: "
                  f"{'MATCH' if content_ok else 'DIFFERENT'}")
    if not ncm_keys:
        print("  (no ncm output in this log)")

    dmnt_ok = any(id_matches(mid, expect_id) for mid in module_ids.values())
    verdict = "PASS" if (version_ok and content_ok) else "FAIL"
    print(f"\n## verdict: {verdict}")
    print(f"   gate ①+② (ncm version + update Program content id): "
          f"{'PASS' if verdict == 'PASS' else 'FAIL'}")
    print(f"   hardening ③ (dmnt:cht ModuleId): "
          f"{'PASS' if dmnt_ok else 'not available in this run'}")
    print(f"   (fsp-ldr is expected to be denied on Atmosphere; it is not part of the verdict)")
    return 0 if verdict == "PASS" else 3


if __name__ == "__main__":
    raise SystemExit(main())
