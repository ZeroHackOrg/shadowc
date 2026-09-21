#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Mint Enterprise Vault unlock tokens for the shadowc licence gate.

The salt used here must match the salt configured in the tooling that runs
the vault (--salt on the command line, or SHADOWC_SALT).

Examples:
    python tools/gen_license.py --client demo_corp --salt "$(cat vault/salt.txt)"
    python tools/gen_license.py --client demo_corp            # demo salt
    python tools/gen_license.py --new-salt '>" vault/salt.txt  # fresh salt
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(__file__)), "src"))

from gatekeeper import ZeroHackLicenseGate, new_salt  # noqa: E402

DEMO_SALT = "ZH_COMMUNITY_DEMO_2026"


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description="Mint shadowc Enterprise Vault tokens.")
    p.add_argument("--client", required=True, help="client identifier to mint for")
    p.add_argument("--salt", default=os.environ.get("SHADOWC_SALT", DEMO_SALT))
    p.add_argument("--new-salt", action="store_true", help="print a fresh 32-byte salt")
    args = p.parse_args(argv)

    if args.new_salt:
        print(new_salt())
        return 0

    gate = ZeroHackLicenseGate(salt=args.salt)
    token = gate.mint(args.client)
    print(f"client ....... {args.client}")
    print(f"salt ......... {args.salt}")
    print(f"token ........ {token}")
    print()
    print("usage: shadowc --level enterprise --client-id <client> --token <token>")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())