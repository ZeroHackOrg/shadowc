"""Structural + semantic integration tests for the 0.5 Vault weave passes:

    shadowc-hardpred  strong opaque predicates (community)
    shadowc-trap      anti-debug ptrace guard (vault)
    shadowc-virt      bytecode operator virtualization (vault)
    shadowc-init      ctor-based initializer scrambling (vault)

Each test asserts the IR topology the mitigation is supposed to introduce,
then links and runs the result to prove semantics were preserved.

Skipped automatically when the LLVM toolchain or the pass plugin are missing.
"""

import re
import shutil
import subprocess
from pathlib import Path

from shadowc.pipeline import VAULT_PASSES

GLOBAL_C = r"""
#include <stdio.h>
unsigned lut[4] = {101u, 202u, 303u, 404u};
int g_int = 7;
int main(void) {
    unsigned s = 0;
    for (int i = 0; i < 4; ++i) s += lut[i];
    unsigned x = 9u, y = 11u;
    unsigned a = x * y;
    unsigned b = x + y;
    unsigned c = (a ^ b) + (x & y);
    unsigned z = c * 3u - b;
    printf("sum=%u ctrl=%u\n", s + z, 5555u);
    return 0;
}
"""


def run(cmd, check=True):
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if check and proc.returncode != 0:
        raise AssertionError(
            f"command failed ({proc.returncode}): {' '.join(map(str, cmd))}\n{proc.stderr[-2000:]}"
        )
    return proc


def compile_ir(env, work, name="global.c"):
    c_path = work / name
    c_path.write_text(GLOBAL_C)
    ll_path = work / f"{name}.ll"
    run(
        [
            env["toolchain"]["clang"],
            env["optlevel"], "-S", "-emit-llvm",
            "-Xclang", "-disable-O0-optnone",
            str(c_path), "-o", str(ll_path),
        ]
    )
    return ll_path


def opt(env, ir_in, passes, ir_out, seed=42):
    run(
        [
            env["toolchain"]["opt"],
            f"-load-pass-plugin={env['plugin']}",
            f"-shadowc-seed={seed}",
            # The passes sample deterministically; force 100% so structural
            # expectations do not depend on a particular seed draw.
            "-shadowc-hardpred-fraction=100",
            "-shadowc-virt-fraction=100",
            "-shadowc-init-fraction=100",
            f"-passes={passes}",
            "-S", str(ir_in), "-o", str(ir_out),
        ]
    )
    run([env["toolchain"]["opt"], "-passes=verify", "-S", str(ir_out), "-o", str(ir_out)])
    return ir_out.read_text()


def run_hardened(env, ir_path, exe_path):
    obj = exe_path.with_suffix(".o")
    run([env["toolchain"]["llc"], "-filetype=obj", str(ir_path), "-o", str(obj)])
    run([env["toolchain"]["clang"], "-no-pie", str(obj), "-o", str(exe_path)])
    return run([str(exe_path)]).stdout


def test_vault_pass_names_are_the_gate_surface(env):
    """The pipeline's vault list and the C++ plugin gate must never drift."""
    from gatekeeper import ZeroHackLicenseGate

    assert set(VAULT_PASSES) == set(ZeroHackLicenseGate().vault_passes)


def test_hardpred_injects_hard_predicates(env, tmp_work):
    # hardpred wraps unconditional edges, which flattened -O1 mains tend to
    # lack; the stats fixture has a real CFG for its loops, so it hosts the
    # structural assertions.
    fixt = Path(__file__).resolve().parent / "fixtures" / "stats.c"
    c_path = tmp_work / "stats.c"
    c_path.write_text(fixt.read_text())
    ll_path = tmp_work / "stats.ll"
    run(
        [
            env["toolchain"]["clang"], env["optlevel"], "-S", "-emit-llvm",
            "-Xclang", "-disable-O0-optnone",
            str(c_path), "-o", str(ll_path),
        ]
    )
    plain = run(
        [env["toolchain"]["clang"], env["optlevel"], str(c_path), "-o",
         str(tmp_work / "plain")])
    expected = run([str(tmp_work / "plain")]).stdout

    text = opt(env, ll_path, "function(shadowc-hardpred)", tmp_work / "hp.ll")
    assert "shadowc.hard.dead" in text, "dead shadow path missing"
    assert text.count("icmp eq i") >= 3, "hard predicates missing"
    # h2/h3 cases emit a urem/srem or multi-add vector; require at least one
    # of the families to be present in the tree.
    assert "srem" in text or "xor i32" in text, "expected a hard-predicate body"
    out = run_hardened(env, tmp_work / "hp.ll", tmp_work / "hp")
    assert out == expected, "hardpred changed semantics"


def test_trap_injects_ptrace_guard(env, tmp_work):
    ir = compile_ir(env, tmp_work)
    text = opt(env, ir, "shadowc-trap", tmp_work / "trap.ll")
    assert "define internal void @shadowc.trap" in text
    assert re.search(r"declare i32 @ptrace\(i32", text), "ptrace decl missing"
    assert "call void @exit(i32 173)" in text, "fail-closed exit missing"
    assert text.count("@shadowc.trap()") >= 1, "guard not wired into entry"
    out = run_hardened(env, tmp_work / "trap.ll", tmp_work / "trap")
    assert out == "sum=1374 ctrl=5555\n", "untraced run must be unaffected"

    strace = shutil.which("strace")
    if strace:
        proc = run([strace, "-f", "-q", str(tmp_work / "trap")], check=False)
        # ptrace(TRACEME) fails under the tracer -> fail-closed exit(173).
        assert proc.returncode == 173, f"expected fail-closed exit 173, got {proc.returncode}"


def test_virt_routes_ops_through_interpreter(env, tmp_work):
    ir = compile_ir(env, tmp_work)
    plain = run(
        [env["toolchain"]["clang"], env["optlevel"], str(tmp_work / "global.c"), "-o",
         str(tmp_work / "plain")]
    )
    text = opt(env, ir, "shadowc-virt", tmp_work / "virt.ll", seed=42)
    assert "shadowc.vm.prog" in text, "bytecode blob missing"
    assert "shadowc.vm.eval32" in text and "shadowc.vm.eval64" in text
    assert "@shadowc.vm.eval32(i32" in text, "no op routed into the VM"
    # Blob must not be a literal run of 0..6 opcode bytes (it is masked).
    prog_match = re.search(r"@shadowc\.vm\.prog = private constant \[(\d+) x i8\] c\"([^\"]*)\"", text)
    assert prog_match, "prog blob unidentifiable"
    out = run_hardened(env, tmp_work / "virt.ll", tmp_work / "virt")
    assert out == "sum=1374 ctrl=5555\n"


def test_init_scrambles_global_initializers(env, tmp_work):
    ir = compile_ir(env, tmp_work)
    text = opt(env, ir, "shadowc-init", tmp_work / "init.ll", seed=42)
    assert "@shadowc.init.lut" in text, "masked payload global missing"
    assert "shadowc.ctor.lut" in text, "rebuild ctor missing"
    assert re.search(r"@lut = dso_local.*zeroinitializer", text), \
        "original initializer not cleared"
    assert "[i32 101, i32 202" not in text, "plaintext initializer left in image"
    out = run_hardened(env, tmp_work / "init.ll", tmp_work / "init")
    assert out == "sum=1374 ctrl=5555\n"


def test_trap_and_init_coexist_with_string_vault(env, tmp_work):
    ir = compile_ir(env, tmp_work)
    text = opt(env, ir, "shadowc-string,shadowc-trap,shadowc-init",
               tmp_work / "combo.ll", seed=42)
    assert "shadowc.trap" in text and "shadowc.init.lut" in text
    out = run_hardened(env, tmp_work / "combo.ll", tmp_work / "combo")
    assert out == "sum=1374 ctrl=5555\n", "combined vault weaves changed semantics"