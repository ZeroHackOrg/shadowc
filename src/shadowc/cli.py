"""shadowc command-line front-end.

Build entry::

    python -m shadowc.cli firmware.c --target arm64 -o secure.bin
"""

from __future__ import annotations

import argparse
import os
import sys

from . import __version__
from .gatekeeper import LicenseError, ZeroHackLicenseGate
from .pipeline import PipelineError, Settings, ShadowCompiler


def _enabled_levels():
    return ["community", "enterprise", "min"]


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="shadowc",
        description="Shadow-Compile: LLVM IR obfuscation engine (Split-Core).",
    )
    p.add_argument("source", nargs="?", help="input: .c/.cpp source, or .ll IR")
    p.add_argument("-o", "--output", help="output path (default: <stem>.hardened)")
    p.add_argument(
        "--target",
        default="host",
        choices=["host", "arm64", "aarch64", "amd64", "x86_64"],
        help="code generation target (host = native)",
    )
    p.add_argument(
        "--level",
        default="community",
        choices=_enabled_levels(),
        help="tier to run: community (public), enterprise (licence), min",
    )
    p.add_argument(
        "--lang",
        default="auto",
        choices=["auto", "c", "cpp", "python", "ir"],
        help="pipeline language (default: from source extension)",
    )
    p.add_argument(
        "--emit",
        default="exe",
        choices=["exe", "obj", "ll"],
        help="output artifact (exe links; obj/ll are intermediate stages)",
    )
    p.add_argument(
        "--seed",
        type=int,
        default=None,
        help="deterministic scramble seed (default: random per build = polymorphism)",
    )
    p.add_argument("--opt-level", type=int, default=1, help="clang -O level for IR emission")
    p.add_argument("--no-strip", action="store_true", help="keep the symbol table")
    p.add_argument("--keep-temps", action="store_true", help="preserve intermediate files")
    p.add_argument(
        "--passes",
        default=None,
        help="explicit comma-separated pass list (overrides --level selection)",
    )
    p.add_argument("--client-id", default="anonymous", help="licence client identifier")
    p.add_argument(
        "--token",
        default=os.environ.get("SHADOWC_TOKEN", ""),
        help="Enterprise Vault unlock token (or SHADOWC_TOKEN env var)",
    )
    p.add_argument(
        "--salt",
        default=os.environ.get("SHADOWC_SALT", "ZH_COMMUNITY_DEMO_2026"),
        help="gatekeeper salt (or SHADOWC_SALT env var; community demo default)",
    )
    p.add_argument(
        "--manifest",
        default=None,
        metavar="PATH",
        help="write a JSON build manifest (input hash, seed, passes, artifact hash, runtime smoke)",
    )
    p.add_argument(
        "--link-flags",
        default=None,
        metavar="FLAGS",
        help="extra native link flags for the final exe (e.g. python lane linkage)",
    )
    p.add_argument("--status", action="store_true", help="print toolchain status and exit")
    p.add_argument("-V", "--version", action="version", version=f"shadowc {__version__}")
    p.add_argument("-v", "--verbose", action="store_true")
    return p


def _status() -> int:
    gate = ZeroHackLicenseGate()
    print(f"shadowc {__version__} (Split-Core: community / enterprise)")
    print(f"  vault mode ........ {gate.describe()['mode']}")
    print(f"  vault passes ...... {', '.join(gate.vault_passes)}")
    try:
        comp = ShadowCompiler(Settings(source="."))
    except PipelineError as exc:
        print(f"  pass plugin ....... not found (run the CMake build first)")
        print(f"  hint .............. {exc}")
        return 1
    for name in ("clang", "opt", "llc"):
        try:
            print(f"  {name:15} {comp.tool(name)}")
        except PipelineError as exc:
            print(f"  {name:15} (missing) - {exc}")
    plugin = comp.plugin
    print(f"  pass plugin ....... {plugin if plugin else '(missing - build with cmake)'}")
    return 0 if plugin else 1


def main(argv=None) -> int:
    args = build_parser().parse_args(argv)

    if args.status:
        return _status()

    if not args.source:
        print("error: a source file is required (or use --status)", file=sys.stderr)
        return 2

    settings = Settings(
        source=args.source,
        output=args.output,
        target=args.target,
        level=args.level,
        lang=args.lang,
        emit=args.emit,
        seed=args.seed,
        opt_level=args.opt_level,
        strip=not args.no_strip,
        keep_temps=args.keep_temps,
        client_id=args.client_id,
        token=args.token,
        salt=args.salt,
        force_passes=args.passes.split(",") if args.passes else None,
        manifest=args.manifest,
        link_flags=args.link_flags.split() if args.link_flags else None,
    )

    try:
        compiler = ShadowCompiler(settings)
        if args.verbose:
            print(f"[shadowc] pipeline : {compiler.pipeline()}", file=sys.stderr)
        result = compiler.run()
    except (PipelineError, ValueError, LicenseError) as exc:
        print(f"shadowc: error: {exc}", file=sys.stderr)
        return 1

    print(f"shadowc: hardened artifact written to {result}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())