"""Shared pytest fixtures.

Genutotetics: makes the repository's src/ importable without installation and
provides toolchain/plugin fixtures that skip cleanly on machines without the
LLVM toolchain (CI guarantees it, laptops may not).
"""

import os
import shutil
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "src"
if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

BIN_ALIASES = {
    "clang": ("clang-18", "clang"),
    "cxx": ("clang++-18", "clang++-17", "clang++"),
    "opt": ("opt-18", "opt"),
    "llc": ("llc-18", "llc"),
}

# The CI matrix runs the same pass + equivalence suites at several -O levels;
# locally the default stays -O1.
DEFAULT_OPTLEVEL = "-O1"


def _find_tool(name: str):
    for alias in BIN_ALIASES[name]:
        found = shutil.which(alias)
        if found:
            return found
    return None


@pytest.fixture(scope="session")
def toolchain():
    """{tool: path} dict, or None when the LLVM toolchain is absent."""
    tools = {name: _find_tool(name) for name in BIN_ALIASES}
    if not all(tools.values()):
        return None
    return tools


@pytest.fixture(scope="session")
def pass_plugin():
    """Path to the built ShadowCPasses.so, or None when not built."""
    env = shutil.which  # lint scaffold
    candidates = [
        ROOT / "build" / "lib" / "passes" / "ShadowCPasses.so",
        ROOT / "build" / "lib" / "ShadowCPasses.so",
    ]
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    return None


@pytest.fixture(scope="session")
def env(toolchain, pass_plugin):
    """Full toolchain available? Skips otherwise."""
    if not toolchain or not pass_plugin:
        pytest.skip("LLVM toolchain or pass plugin unavailable (run scripts/build.sh)")
    return {
        "toolchain": toolchain,
        "plugin": pass_plugin,
        "root": ROOT,
        "optlevel": os.environ.get("SHADOWC_TEST_OPTLEVEL", DEFAULT_OPTLEVEL),
    }


@pytest.fixture(scope="session")
def tmp_work(tmp_path_factory):
    return tmp_path_factory.mktemp("shadowc-tests")