"""shadowc build pipeline: clang -> opt(+plugin) -> llc.

The pass plugin is the C++ core built by CMake (``build/lib/passes/
ShadowCPasses.so``). This module is the split-core decision layer: it picks
which passes run (community vs enterprise), enforces the licence gate, and
drives the LLVM command-line toolchain.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import List, Optional

from .gatekeeper import LicenseError, ZeroHackLicenseGate

VERSION = "0.5.0"

#: Community Core passes, in recommended order. Each name is registered by the
#: C++ plugin under the new pass manager.
COMMUNITY_PASSES = (
    "shadowc-substitution",
    "shadowc-boguscf",
    "shadowc-cff",
    "shadowc-hardpred",
)

#: Enterprise Vault passes - licence-gated module-level passes.
VAULT_PASSES = (
    "shadowc-string",
    "shadowc-virt",
    "shadowc-trap",
    "shadowc-init",
)

# Prefer version-suffixed binaries so every stage of the pipeline targets the
# same LLVM release as the C++ pass plugin. A mismatched front-end can emit IR
# syntax the pinned opt/llc cannot parse.
_TOOL_ALIASES = {
    "clang": ["clang-18", "clang", "clang-17"],
    "opt": ["opt-18", "opt", "opt-17"],
    "llc": ["llc-18", "llc", "llc-17"],
}

_TARGET_TRIPLES = {
    "host": None,  # native
    "arm64": "aarch64-unknown-linux-gnu",
    "aarch64": "aarch64-unknown-linux-gnu",
    "amd64": "x86_64-unknown-linux-gnu",
    "x86_64": "x86_64-unknown-linux-gnu",
}


class PipelineError(RuntimeError):
    """Raised for toolchain or compilation failures."""


@dataclass
class Settings:
    """One end-to-end hardening run."""

    source: Path
    output: Optional[Path] = None
    target: str = "host"
    level: str = "community"  # community | enterprise | min
    emit: str = "exe"  # exe | obj | ll
    seed: Optional[int] = None
    opt_level: int = 1
    strip: bool = True
    keep_temps: bool = False
    client_id: str = "anonymous"
    token: str = ""
    salt: str = "ZH_COMMUNITY_DEMO_2026"
    force_passes: Optional[List[str]] = None
    manifest: Optional[Path] = None


class ShadowCompiler:
    def __init__(self, settings: Settings, gateway: Optional[ZeroHackLicenseGate] = None):
        self.s = settings
        self.gate = gateway or ZeroHackLicenseGate(salt=settings.salt or "ZH_COMMUNITY_DEMO_2026")
        self._tools: dict = {}
        self.plugin = self._locate_plugin()

    # ------------------------------------------------------------------
    # Toolchain discovery
    # ------------------------------------------------------------------
    def _find_tool(self, name: str) -> str:
        env = {"clang": "SHADOWC_CLANG", "opt": "SHADOWC_OPT", "llc": "SHADOWC_LLC"}.get(name)
        if env and os.environ.get(env):
            return os.environ[env]
        for candidate in _TOOL_ALIASES[name]:
            found = shutil.which(candidate)
            if found:
                return found
        raise PipelineError(
            f"tool '{name}' not found. Install an LLVM toolchain (CI installs "
            "llvm-18) or point SHADOWC_{name.upper()} at the binary."
        )

    def tool(self, name: str) -> str:
        if name not in self._tools:
            self._tools[name] = self._find_tool(name)
        return self._tools[name]

    def _locate_plugin(self) -> Optional[Path]:
        if os.environ.get("SHADOWC_PLUGIN"):
            return Path(os.environ["SHADOWC_PLUGIN"])
        root = self._repo_root()
        candidates = [
            root / "build" / "lib" / "passes" / "ShadowCPasses.so",
            root / "build" / "lib" / "ShadowCPasses.so",
            root / "lib" / "passes" / "ShadowCPasses.so",
        ]
        for candidate in candidates:
            if candidate.is_file():
                return candidate
        return None

    @staticmethod
    def _repo_root() -> Path:
        here = Path(__file__).resolve()
        for parent in here.parents:
            if (parent / "CMakeLists.txt").is_file() and (parent / "lib" / "passes").is_dir():
                return parent
        return here.parent

    # ------------------------------------------------------------------
    # Pass selection (Split-Core decision layer)
    # ------------------------------------------------------------------
    def pipeline(self) -> str:
        if self.s.force_passes:
            names = list(self.s.force_passes)
        elif self.s.level == "min":
            names = ["shadowc-cff"]
        elif self.s.level == "enterprise":
            names = list(COMMUNITY_PASSES) + list(VAULT_PASSES)
        else:
            names = list(COMMUNITY_PASSES)

        # Licence gate: enforce before touching the toolchain.
        for name in names:
            if name in VAULT_PASSES:
                self.gate.require(self.s.client_id, self.s.token, name)

        fn = [n for n in names if n not in VAULT_PASSES]
        mod = [n for n in names if n in VAULT_PASSES]
        parts = []
        if fn:
            parts.append("function(" + ",".join(fn) + ")")
        parts.extend(mod)
        return ",".join(parts)

    # ------------------------------------------------------------------
    # Execution
    # ------------------------------------------------------------------
    def run(self) -> Path:
        if not self.plugin or not self.plugin.is_file():
            raise PipelineError(
                "pass plugin not found - run 'cmake -S . -B build -DLT_LLVM_INSTALL_DIR=/usr/lib/llvm-18 && cmake --build build' first"
            )
        source = Path(self.s.source)
        if not source.is_file():
            raise PipelineError(f"source not found: {source}")

        tmpdir = Path(tempfile.mkdtemp(prefix="shadowc-"))
        try:
            ir = self._emit_ir(source, tmpdir)
            hardened = tmpdir / "hardened.ll"
            passes = self.pipeline()
            self._run_opt(ir, hardened, passes=passes)
            result = self._emit_target(hardened, tmpdir)
            if self.s.manifest:
                self._write_manifest(source, result, passes, tmpdir)
            return result
        finally:
            if not self.s.keep_temps:
                shutil.rmtree(tmpdir, ignore_errors=True)

    def _write_manifest(self, source: Path, result: Path, passes: str,
                        tmpdir: Path) -> None:
        """Emits a deterministic build manifest (JSON) next to --manifest."""
        import hashlib
        import json
        import time

        def sha256_of(path: Path) -> str:
            h = hashlib.sha256()
            with open(path, "rb") as fh:
                for chunk in iter(lambda: fh.read(65536), b""):
                    h.update(chunk)
            return h.hexdigest()

        triple = _TARGET_TRIPLES.get(self.s.target, _TARGET_TRIPLES["host"])
        manifest = {
            "shadowc_version": VERSION,
            "source": str(source),
            "source_sha256": sha256_of(source),
            "target": self.s.target,
            "triple": triple or "host",
            "level": self.s.level,
            "emit": self.s.emit,
            "seed": self.s.seed if self.s.seed is not None else 0,
            "polymorphic": self.s.seed is None,
            "opt_level": self.s.opt_level,
            "passes": passes,
            "plugin": str(self.plugin),
            "tools": {name: self.tool(name) for name in ("clang", "opt", "llc")},
            "artifact": str(result),
            "artifact_sha256": sha256_of(result),
            "built_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        }

        # Best-effort runtime smoke of a host executable: records whether the
        # hardened binary actually runs to completion (exit 0) and the hash of
        # its stdout, giving CI a cheap "still alive" signal.
        if self.s.emit == "exe" and not triple:
            try:
                proc = subprocess.run(
                    [str(result)], capture_output=True, text=True, timeout=30,
                )
                manifest["runtime"] = {
                    "exit_code": proc.returncode,
                    "stdout_sha256": hashlib.sha256(proc.stdout.encode()).hexdigest(),
                }
            except subprocess.TimeoutExpired:
                manifest["runtime"] = {"exit_code": None, "stdout_sha256": None}

        out = Path(self.s.manifest)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(json.dumps(manifest, indent=2) + "\n")

    def _emit_ir(self, source: Path, tmpdir: Path) -> Path:
        suffixes = (".c", ".cc", ".cpp", ".cxx")
        if source.suffix in suffixes:
            out = tmpdir / "input.ll"
            cmd = [
                self.tool("clang"),
                f"-O{self.s.opt_level}",
                "-S", "-emit-llvm",
                "-Xclang", "-disable-O0-optnone",
                str(source), "-o", str(out),
            ]
            self._run(cmd)
            return out
        if source.suffix in (".ll", ".bc"):
            return source
        if source.suffix == ".rs":
            raise PipelineError(
                "Rust input requires an IR front-end step: compile with "
                "'rustc --emit=llvm-ir' and pass the .ll to shadowc."
            )
        raise PipelineError(f"unsupported input type: {source.suffix}")

    def _opt_args(self) -> List[str]:
        args = [self.tool("opt"), f"-load-pass-plugin={self.plugin}"]
        if self.s.seed is not None:
            args.append(f"-shadowc-seed={self.s.seed}")
        # Per-vendor salt rotates the string/vault keystreams; the default
        # demo salt keeps default builds deterministic for tests.
        if self.s.salt:
            args.append(f"-shadowc-string-salt={self.s.salt}")
        return args

    def _run_opt(self, ir: Path, out: Path, passes: Optional[str] = None) -> None:
        pipeline = passes or self.pipeline()
        cmd = self._opt_args() + ["-passes=" + pipeline, "-S", str(ir), "-o", str(out)]
        self._run(cmd)

    def _emit_target(self, hardened: Path, tmpdir: Path) -> Path:
        triple = _TARGET_TRIPLES.get(self.s.target, _TARGET_TRIPLES["host"])
        obj = tmpdir / "hardened.o"

        llc = [self.tool("llc"), "-filetype=obj"]
        if triple:
            llc += [f"-mtriple={triple}"]
        elif self.s.target not in ("host", "native"):
            raise PipelineError(f"unknown target '{self.s.target}'")
        llc += [str(hardened), "-o", str(obj)]
        self._run(llc)

        if self.s.emit == "obj":
            out = self._final_path(obj, "o")
            shutil.copyfile(obj, out)
            return out
        if self.s.emit == "ll":
            out = self._final_path(hardened, "ll")
            shutil.copyfile(hardened, out)
            return out

        # Link a positional, hardened executable.
        # `-no-pie` is required once string globals become writable (.data).
        out = self._final_path(obj, "exe")
        cmd = [self.tool("clang"), "-no-pie", str(obj), "-o", str(out)]
        if triple:
            # Cross-link through clang: on Debian/Ubuntu it discovers the cross
            # GCC toolchain (e.g. gcc-aarch64-linux-gnu) automatically.
            cmd[1:1] = [f"--target={triple}"]
        try:
            self._run(cmd)
        except PipelineError as exc:
            if triple:
                raise PipelineError(
                    "cross-link failed. Install the matching cross toolchain "
                    f"(e.g. gcc-aarch64-linux-gnu) or emit an object instead "
                    f"(--emit obj).\n{exc}"
                )
            raise
        if self.s.strip:
            strip = shutil.which("strip")
            if strip:
                self._run([strip, str(out)], check=False)
        return out

    def _final_path(self, fallback: Path, ext: str) -> Path:
        if self.s.output:
            return Path(self.s.output)
        stem = Path(self.s.source).stem
        mapping = {"o": "o", "ll": "ll", "exe": ""}
        name = f"{stem}.hardened" if ext == "exe" else f"{stem}.hardened.{ext}"
        return Path(name).resolve()

    # ------------------------------------------------------------------
    # Subprocess plumbing
    # ------------------------------------------------------------------
    def _run(self, cmd: List[str], check: bool = True) -> subprocess.CompletedProcess:
        proc = subprocess.run(cmd, capture_output=True, text=True)
        if proc.returncode != 0 and check:
            detail = " ".join(str(c) for c in cmd)
            stderr = (proc.stderr or "").strip()
            raise PipelineError(
                f"command failed ({proc.returncode}): {detail}\n{stderr[-2000:]}"
            )
        return proc


def build(settings: Settings) -> Path:
    return ShadowCompiler(settings).run()