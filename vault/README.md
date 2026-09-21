# Vault

This directory is the seam between the public Community tree and the licensed
Enterprise Vault.

## Layout

- `README.md` — this file.
- `vault/` is intentionally minimal in the public tree. The licensed salt and
  enterprise-fixture IR lives in the private mirror that vendors pay to
  receive; the gate code that consumes it (`src/gatekeeper.py`) is public so
  review stays honest.

## What is a "vault" here

The Split-Core gate means the Vault is a *decision*, not a folder:

- Community builds run `function(shadowc-substitution,shadowc-boguscf,
  shadowc-cff)`.
- Enterprise builds add `shadowc-string` — but only after
  `ZeroHackLicenseGate.require()` accepts an HMAC token minted against the
  configured salt (`--salt` / `SHADOWC_SALT`).

Without a valid token the pipeline raises `LicenseError` before IR is touched.

## Operating it

```sh
# DevOps / licensed partners:
python3 tools/gen_license.py --client acme-fw --salt <CUSTOMER_SALT>   # mint
SHADOWC_TOKEN=<token> ./bin/shadowc --level enterprise --client-id acme-fw app.c -o out.bin
```

Demo salt (public, unlocks nothing real): `ZH_COMMUNITY_DEMO_2026`.

For the TLS license-server handshake, swap the gate internals only
(`src/gatekeeper.py`); the pipeline interface is stable. See
`docs/COMMERCIAL.md`.