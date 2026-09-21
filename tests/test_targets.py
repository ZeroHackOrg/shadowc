"""Cross-target verification: hardened artifacts must link for other ABIs.

Local mirror of the CI cross step. Skipped when the cross toolchain is
missing (that cell is then CI-covered only - see docs/TESTING.md).
"""

import shutil
import subprocess
from pathlib import Path

import pytest

from shadowc.pipeline import PipelineError, Settings, ShadowCompiler

HERE = Path(__file__).resolve().parent


def run(cmd, check=True, cwd=None):
    proc = subprocess.run(cmd, capture_output=True, text=True, cwd=cwd)
    if check and proc.returncode != 0:
        raise AssertionError(
            f"command failed ({proc.returncode}): {' '.join(map(str, cmd))}\n{proc.stderr[-2000:]}"
        )
    return proc


def test_arm64_object_and_exe_link_when_cross_gcc_present(tmp_path, env):
    src = HERE / "fixtures" / "stats.c"
    if not shutil.which("aarch64-linux-gnu-gcc"):
        pytest.skip("gcc-aarch64-linux-gnu not installed (sudo apt install ...)")

    s = Settings(
        source=str(src), output=str(tmp_path / "arm64.o"),
        level="community", seed=7, client_id="cross",
        token="", strip=False, target="arm64", emit="obj", opt_level=1,
    )
    ShadowCompiler(s).run()
    assert tmp_path.joinpath("arm64.o").exists()

    exe = tmp_path / "arm64.bin"
    s2 = Settings(
        source=str(src), output=str(exe),
        level="community", seed=7, client_id="cross",
        token="", strip=False, target="arm64", emit="exe", opt_level=1,
    )
    ShadowCompiler(s2).run()

    magic = exe.read_bytes()
    assert magic[:4] == b"\x7fELF"
    assert magic[18:20] == b"\xb7\x00", "EM_AARCH64 magic missing"

    run([shutil.which("file"), str(exe)])
    pk = run(["aarch64-linux-gnu-objdump", "--file-headers", str(exe)]).stdout
    assert "aarch64" in pk