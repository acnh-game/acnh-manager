# hbmenu forwarder loader source

This directory contains the source and NPDM configuration for Sphaira `hbl` 3.0.0, commit
`338348e74b6d278bc570a9be5373d25197bf6c8d`.  No compiled loader binary is kept in the
repository:

```text
source/main.c
source/trampoline.s
hbl.json
```

`tools/build-forwarder.py` compiles them in the pinned devkitPro container, then passes the
freshly produced `main` and `main.npdm` to `tools/make-forwarder-nsp.py`.  The latter patches
the NPDM and constructs the Program, Control and Meta NCAs itself.

`SOURCE.json` records the upstream commit, source hashes and license.  The code is a
third-party MIT-licensed nx-hbloader derivative; see `LICENSE.md`.
