"""Equivalence fixture farm: plain vs community vs enterprise hardening.

Every fixture (tests/fixtures/*.c) is compiled straight, hardened through the
*exact* pipeline string the product emits for community and enterprise, then
executed. stdout must equal the fixture's pinned expected file for all three,
which locks the semantic-correctness contract (including the CFF
entry-successor regression class) across the whole interesting feature space:
nested loops, recursion, switch machines, bitfields, strings, stats math.

Skipped automatically when the LLVM toolchain or the pass plugin are missing.
"""

import subprocess
from pathlib import Path

import pytest

from shadowc.pipeline import Settings, ShadowCompiler

FIXTURES = Path(__file__).resolve().parent / "fixtures"
EXPECTED = Path(__file__).resolve().parent / "expected"


def run(cmd, check=True):
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if check and proc.returncode != 0:
        raise AssertionError(
            f"command failed ({proc.returncode}): {' '.join(map(str, cmd))}\n{proc.stderr[-2000:]}"
        )
    return proc


def compile_ir(env, c_path, ll_path):
    run(
        [
            env["toolchain"]["clang"],
            env["optlevel"], "-S", "-emit-llvm",
            "-Xclang", "-disable-O0-optnone",
            str(c_path), "-o", str(ll_path),
        ]
    )


def opt_hardened(env, ir_path, out=None):
    """Runs the product's own community pipeline over the IR in one opt call."""
    pass_list = "function(shadowc-substitution,shadowc-boguscf,shadowc-cff,shadowc-hardpred)"
    cmd = [
        env["toolchain"]["opt"],
        f"-load-pass-plugin={env['plugin']}",
        "-shadowc-seed=42",
        f"-passes={pass_list}",
        "-S", str(ir_path), "-o", str(out or (ir_path.parent / "community.ll")),
    ]
    run(cmd)


def opt_vault(env, ir_path, out=None):
    """Runs the enterprise pipeline string exactly as the CLI would."""
    from gatekeeper import ZeroHackLicenseGate

    comp = ShadowCompiler(
        Settings(source=str(ir_path), level="enterprise", client_id="fx",
                 token=ZeroHackLicenseGate(salt="ZH_COMMUNITY_DEMO_2026").mint("fx"))
    )
    pipeline = comp.pipeline()
    cmd = [
        env["toolchain"]["opt"],
        f"-load-pass-plugin={env['plugin']}",
        "-shadowc-seed=42",
        "-shadowc-string-salt=ZH_COMMUNITY_DEMO_2026",
        f"-passes={pipeline}",
        "-S", str(ir_path), "-o", str(out or (ir_path.parent / "vault.ll")),
    ]
    run(cmd)
    return pipeline


def link_and_run(env, ir_path, exe_path):
    run([env["toolchain"]["opt"], "-passes=verify", "-S", str(ir_path),
         "-o", str(ir_path)])
    obj = exe_path.with_suffix(".o")
    run([env["toolchain"]["llc"], "-filetype=obj", str(ir_path), "-o", str(obj)])
    run([env["toolchain"]["clang"], "-no-pie", str(obj), "-o", str(exe_path)])
    return run([str(exe_path)]).stdout


def fixture_ids():
    return sorted(p.stem for p in FIXTURES.glob("*.c"))


@pytest.mark.parametrize("name", fixture_ids())
def test_fixture_equivalence(env, tmp_work, name):
    plain = run(
        [env["toolchain"]["clang"], env["optlevel"], str(FIXTURES / f"{name}.c"), "-o",
         str(tmp_work / f"{name}.plain")]
    )
    plain_out = run([str(tmp_work / f"{name}.plain")]).stdout

    expected = (EXPECTED / f"{name}.txt").read_text()
    assert plain_out == expected, "fixture's own plain build drifted from its expected file"

    ir = tmp_work / f"{name}.ll"
    compile_ir(env, FIXTURES / f"{name}.c", ir)

    community = tmp_work / f"{name}.community.ll"
    opt_hardened(env, ir, community)
    c_out = link_and_run(env, community, tmp_work / f"{name}.community")
    assert c_out == expected, "community hardening changed the program's output"

    vault = tmp_work / f"{name}.vault.ll"
    opt_vault(env, ir, vault)
    v_out = link_and_run(env, vault, tmp_work / f"{name}.vault")
    assert v_out == expected, "enterprise vault hardening changed the program's output"


def test_cff_regression_fixture_value_is_pinned(env, tmp_work):
    """The nested-loop CRC fixture may never change its digest silently."""
    expected = (EXPECTED / "crc_nested.txt").read_text()
    plain = run(
        [env["toolchain"]["clang"], env["optlevel"], str(FIXTURES / "crc_nested.c"), "-o",
         str(tmp_work / "crcp")]
    )
    assert run([str(tmp_work / "crcp")]).stdout == expected


def test_seed_determinism_through_full_pipeline(env, tmp_work):
    from gatekeeper import ZeroHackLicenseGate

    gate = ZeroHackLicenseGate(salt="ZH_COMMUNITY_DEMO_2026")
    seed = "7"
    artifacts = []
    for tag in ("a", "b"):
        comp = ShadowCompiler(
            Settings(source=str(FIXTURES / "stats.c"),
                     output=str(tmp_work / f"det-{tag}.bin"),
                     level="enterprise", seed=7, client_id="det",
                     token=gate.mint("det")))
        artifacts.append(comp.run())
    a = artifacts[0].read_bytes()
    b = artifacts[1].read_bytes()
    assert a == b, "identical --seed builds must produce identical binaries"


def test_manifest_contains_contract_fields(env, tmp_work):
    from gatekeeper import ZeroHackLicenseGate

    manifest = tmp_work / "manifest.json"
    comp = ShadowCompiler(
        Settings(source=str(FIXTURES / "stats.c"),
                 output=str(tmp_work / "man.bin"),
                 level="community", seed=42, client_id="man",
                 token=ZeroHackLicenseGate(salt="test").mint("man"),
                 manifest=manifest))
    comp.run()
    import json

    data = json.loads(manifest.read_text())
    for key in ("shadowc_version", "source_sha256", "seed", "passes", "target",
                "artifact_sha256", "runtime"):
        assert key in data, f"manifest missing '{key}'"
    assert data["seed"] == 42
    assert data["runtime"]["exit_code"] == 0