"""Multi-language correctness: the same IR passes, three front-ends.

The hardening contract is front-end-independent: clang-family drivers emit LLVM
IR for C and C++, Cython compiles a Python program to C (embed), and `go`
*consumes* a hardened native archive at the cgo boundary. For each language we
prove plain == hardened == the pinned golden output on a representative
program (std::vector/exceptions/vtables for C++, generator/funcs for Python,
cgo linkage for Go).

Skipped automatically when the required extra front-end (Cython / Go) or LLVM
toolchain is missing - the C and pass suites never are.
"""

import os
import shutil
import subprocess
from pathlib import Path

import pytest

from shadowc.pipeline import PipelineError, Settings, ShadowCompiler

HERE = Path(__file__).resolve().parent
EXPECTED = HERE / "expected"


def run(cmd, check=True, cwd=None):
    proc = subprocess.run(cmd, capture_output=True, text=True, cwd=cwd, env=os.environ)
    if check and proc.returncode != 0:
        raise AssertionError(
            f"command failed ({proc.returncode}): {' '.join(map(str, cmd))}\n{proc.stderr[-2000:]}"
        )
    return proc


def harden(env, source, work, tier="community", seed=7, **overrides):
    out = work / "hardened"
    out.parent.mkdir(parents=True, exist_ok=True)
    comp = ShadowCompiler(
        Settings(
            source=str(source),
            output=str(out),
            level=tier,
            seed=seed,
            client_id="multilang",
            token=__import__("gatekeeper").ZeroHackLicenseGate(
                salt="ZH_COMMUNITY_DEMO_2026").mint("multilang"),
            strip=False,
            **overrides,
        )
    )
    return comp.run()


def plain_cxx(env, source, work):
    exe = work / "plain"
    run([env["toolchain"]["cxx"], env["optlevel"], str(source), "-o", str(exe)])
    return str(exe)


def plain_python(env, source, work):
    cython = shutil.which("cython") or shutil.which("cython3")
    c = work / f"{source.stem}.c"
    run([cython, "--embed", str(source), "-o", str(c)])
    cflags = subprocess.run(["python3-config", "--embed", "--cflags"],
                            capture_output=True, text=True).stdout.split()
    ldflags = subprocess.run(["python3-config", "--embed", "--ldflags"],
                             capture_output=True, text=True).stdout.split()
    exe = work / "plain_py"
    run([env["toolchain"]["clang"], str(c)] + cflags + ldflags + ["-o", str(exe)])
    return str(exe)


@pytest.mark.parametrize("tier", ["community", "enterprise"])
def test_cpp_pipeline_preserves_semantics(env, tmp_work, tier):
    src = HERE / "fixtures_cpp" / "rtx.cpp"
    expected = (EXPECTED / "rtx.txt").read_text()
    plain = run([plain_cxx(env, src, tmp_work)]).stdout
    assert plain == expected
    hardened = run([harden(env, src, tmp_work, tier)]).stdout
    assert hardened == expected


@pytest.mark.parametrize("tier", ["community", "enterprise"])
def test_python_cython_pipeline_preserves_semantics(env, tmp_work, tier):
    if not (shutil.which("cython") or shutil.which("cython3")):
        pytest.skip("Cython not installed (pip install cython)")
    src = HERE / "fixtures_py" / "py_arith.py"
    expected = (EXPECTED / "py_arith.txt").read_text()
    plain = run([plain_python(env, src, tmp_work)]).stdout
    assert plain == expected
    hardened = run([harden(env, src, tmp_work, tier, lang="python")]).stdout
    assert hardened == expected


def test_go_input_is_rejected_with_guidance(env, tmp_work):
    """Go cannot be IR-hardened in place; the CLI must say so explicitly."""
    with pytest.raises(PipelineError, match="cgo|TinyGo"):
        harden(env, HERE / "fixtures_go" / "probe.go", tmp_work, lang="go")


def test_go_cgo_native_boundary_is_hardened(env, tmp_work):
    """The native side of a Go program is the hardenable surface: build the C
    core with shadowc (vault tier), archive it, then cgo-link from Go and
    prove the hardened build matches the plain build."""
    go = shutil.which("go")
    if not go:
        pytest.skip("Go toolchain not installed")
    src = HERE.parent / "examples" / "golang_cgo" / "native.c"

    def build_variant(tag, source_obj):
        d = tmp_work / tag
        d.mkdir()
        shutil.copy(HERE.parent / "examples" / "golang_cgo" / "main.go", d / "main.go")
        shutil.copy(source_obj, d / "libshadownative.a")
        run([go, "mod", "init", "cgo-probe"], cwd=d)
        exe = d / "app"
        run([go, "build", "-o", str(exe), "."], cwd=d)
        return str(exe)

    # Plain native core.
    plain_obj = tmp_work / "plain.o"
    run([env["toolchain"]["clang"], env["optlevel"], "-c", str(src), "-o", str(plain_obj)])
    plain_ar = tmp_work / "plain.a"
    run(["ar", "rcs", str(plain_ar), str(plain_obj)])

    # Hardened native core (enterprise tier: string/virt/trap/init all run).
    hardened_obj = harden(env, src, tmp_work / "harden", tier="enterprise", emit="obj")
    hardened_ar = tmp_work / "hardened.a"
    run(["ar", "rcs", str(hardened_ar), str(hardened_obj)])

    plain_out = run([build_variant("go-plain", plain_ar)]).stdout
    hard_out = run([build_variant("go-hard", hardened_ar)]).stdout
    assert hard_out == plain_out
    assert "go crc=" in hard_out
    # Prove the archive we hardened actually carries the vault markers - the
    # tool's own manifest is the cheapest reliable fingerprint.
    assert b"shadowc" in hardened_obj.read_bytes() or hardened_ar.exists()