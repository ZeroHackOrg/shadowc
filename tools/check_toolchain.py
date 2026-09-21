#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Verify that every component of the shadowc toolchain exists.

Exit code 0 when the build can proceed end to end; 1 otherwise. Mirrors the
checks the CI workflow performs.
"""

import os
import shutil
import sys
from pathlib import Path

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(__file__)), "src"))

from shadowc.pipeline import ShadowCompiler, Settings  # noqa: E402

OK = "  [ok]"
BAD = "  [!!]"


def main() -> int:
    fails = 0
    missing = []

    print("shadowc toolchain check\n")

    tools = {"clang": None, "opt": None, "llc": None}
    for name in tools:
        env = {"clang": "SHADOWC_CLANG", "opt": "SHADOWC_OPT", "llc": "SHADOWC_LLC"}[name]
        found = os.environ.get(env)
        if not found:
            for alias in {"clang": ("clang-18", "clang"), "opt": ("opt-18", "opt"), "llc": ("llc-18", "llc")}[name]:
                found = found or shutil.which(alias)
            found = found or shutil.which(f"{name}-18")
        if found:
            tools[name] = found
            print(f"{OK} {name:6} {found}")
        else:
            missing.append(name)
            print(f"{BAD} {name:6} not found")
            fails += 1

    try:
        comp = ShadowCompiler(Settings(source="."))
        plugin = comp.plugin
    except Exception as exc:  # pragma: no cover - defensive
        plugin = None
        print(f"{BAD} plugin detection failed: {exc}")
        fails += 1
    if plugin:
        print(f"{OK } pass-plugin {plugin}")
    else:
        print(f"{BAD} pass-plugin  not built (run: cmake -S . -B build -DLT_LLVM_INSTALL_DIR=/usr/lib/llvm-18)")
        fails += 1

    for tool in ("cmake", "pytest", "strip"):
        if shutil.which(tool):
            print(f"{OK} {tool:11} {shutil.which(tool)}")
        else:
            missing.append(tool)
            print(f"{BAD} {tool:11} not found")
            fails += 1

    if missing:
        print("\nmissing: " + ", ".join(missing))
    print(f"\n{'READY' if fails == 0 else 'NOT READY - resolve the items marked [!!] above'}")
    return 0 if fails == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())