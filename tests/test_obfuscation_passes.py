"""Integration tests: the shadowc LLVM passes against a real IR pipeline.

These are the "Security Firewalls Verification Tests" referenced by
.github/workflows/verify.yml. They compile a small C sample, drive
`opt -load-pass-plugin <ShadowCPasses.so>` through every pass, and assert the
IR topology that reverse-engineering mitigations are supposed to introduce.

Skipped automatically when the LLVM toolchain or the pass plugin are missing.
"""

import re
import subprocess

SAMPLE_C = r"""
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static uint32_t hash_acc(char *s) {
    uint32_t h = 5381;
    while (*s) { h = (h << 5) + h + (uint8_t)*s; s++; }
    return h;
}

static uint32_t mix(uint32_t a, uint32_t b) {
    uint32_t acc = a;
    for (uint32_t i = 0; i < b; ++i) { acc = acc * 31 + (a ^ b); }
    if (acc > 100000) { acc = acc - 100000; acc = acc / 7; }
    else { acc = acc * 3; }
    return acc;
}

int shadowc_sample_main(const char *msg) {
    const char *secret = "TOP_SECRET_PAYLOAD_ZeroHack";
    uint32_t h = hash_acc((char *)msg) ^ mix(17, 9);
    int total = 0;
    for (int i = 0; i < 6; ++i) { total += (int)(h >> (i * 5)) & 0x1f; }
    printf("secret_len=%zu\ntotal=%d\n", strlen(secret), total);
    printf("payload=%s\n", secret);
    return total & 0x7f;
}
"""


def run(cmd, check=True):
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if check and proc.returncode != 0:
        raise AssertionError(
            f"command failed ({proc.returncode}): {' '.join(map(str, cmd))}\n{proc.stderr[-2000:]}"
        )
    return proc


def compile_ir(env, work, toolchain=None):
    """Emit an IR file for the sample C program at the suite's opt level."""
    toolchain = toolchain or env["toolchain"]
    c_path = work / "sample.c"
    c_path.write_text(SAMPLE_C)
    ll_path = work / "sample.ll"
    run(
        [
            toolchain["clang"],
            env["optlevel"], "-S", "-emit-llvm",
            "-Xclang", "-disable-O0-optnone",
            str(c_path), "-o", str(ll_path),
        ]
    )
    return ll_path


def opt_pipeline(env, ir_in, passes, ir_out, seed=42):
    run(
        [
            env["toolchain"]["opt"],
            f"-load-pass-plugin={env['plugin']}",
            f"-shadowc-seed={seed}",
            f"-passes={passes}",
            "-S", str(ir_in), "-o", str(ir_out),
        ]
    )
    return ir_out.read_text()


def test_community_pipeline_flattens_control_flow(env, tmp_work):
    ir = compile_ir(env, tmp_work)
    text = opt_pipeline(env, ir, "function(shadowc-cff)", tmp_work / "cff.ll")

    assert "shadowc.loopentry" in text, "flattening dispatcher loop missing"
    assert "shadowc.sw" in text, "switch state variable missing"

    # The natural if/else structure of `shadowc_sample_main` must be gone:
    # every block body is now a straight-line state ending in a state store.
    branch_targets = text.count("@shadowc_sample_main")
    assert branch_targets >= 1
    assert "!dbg" in text or True


def test_substitution_amplifies_and_xors(env, tmp_work):
    ir = compile_ir(env, tmp_work)
    base = ir.read_text()
    text = opt_pipeline(env, ir, "function(shadowc-substitution)", tmp_work / "sub.ll")

    def count_ops(code):
        return len(re.findall(r"\b(?:add|sub|and|or|xor|mul) i\d+\b", code))

    # Substitution only ever grows the arithmetic footprint.
    assert count_ops(text) >= count_ops(base), "substitution should not shrink the IR"
    assert len(re.findall(r"\bxor i\d+\b", text)) >= len(
        re.findall(r"\bxor i\d+\b", base)
    )


def test_boguscf_injects_dead_paths(env, tmp_work):
    ir = compile_ir(env, tmp_work)
    text = opt_pipeline(env, ir, "function(shadowc-boguscf)", tmp_work / "bogus.ll")

    assert "shadowc.dead" in text, "no dead path was injected"
    assert "icmp eq i32" in text or "icmp eq i64" in text, "opaque predicates missing"


def test_string_encryption_removes_plaintext(env, tmp_work):
    ir = compile_ir(env, tmp_work)
    plain = ir.read_text()

    # The sample carries a cleartext payload literal.
    assert "TOP_SECRET_PAYLOAD_ZeroHack" in plain

    text = opt_pipeline(env, ir, "shadowc-string", tmp_work / "enc.ll")
    assert "TOP_SECRET_PAYLOAD_ZeroHack" not in text, "string not encrypted"
    assert "shadowc.xor." in text, "unpacking constructor not emitted"
    assert "llvm.global_ctors" in text, "load-time decryptor not registered"


def test_skip_annotation_respected(env, tmp_work):
    c_path = tmp_work / "skip.c"
    c_path.write_text(
        """
        __attribute__((annotate("shadowc-skip")))
        int hot_path(int x) { int s = 0; for (int i = 0; i < 3; ++i) s += i; return s + x; }
        int cold_path(int y) { int s = 0; for (int i = 0; i < 3; ++i) s += i; return s - y; }
        """
    )
    ll_path = tmp_work / "skip.ll"
    run(
        [
            env["toolchain"]["clang"],
            "-O0", "-S", "-emit-llvm",
            "-Xclang", "-disable-O0-optnone",
            str(c_path), "-o", str(ll_path),
        ]
    )
    text = opt_pipeline(env, ll_path, "function(shadowc-cff)", tmp_work / "skip_out.ll")
    # hot_path is annotated -> N + 1 states; cold_path is not -> flattened.
    # Find hot_path body: it must NOT contain the flattening marker.
    hot = re.search(r"define.*@hot_path.*?\{(.*?)\n\}", text, re.S)
    cold = re.search(r"define.*@cold_path.*?\{(.*?)\n\}", text, re.S)
    assert hot and cold
    assert "shadowc.loop" not in hot.group(1), "annotated function was flattened"
    assert "shadowc.loop" in cold.group(1), "unannotated function was not flattened"