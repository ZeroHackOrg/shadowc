# Commercial / licensing model

## The problem we solve for product teams

A firmware .bin or a client app ships to customers. The reverse engineer has
full binaries, an x64/arm64 machine, and no build secrets. Shadow-Compile makes
*static analysis unpleasant at scale*: the CFG is flattened, strings are out of
`strings`, signatures no longer match upstream SDK builds, and each shipment is
a new morph.

## Split-Core: how `shadowc` licences itself

One executable, two tiers, decided *inside the build* by a gate:

| Tier | Passes | Enforced by |
|------|--------|-------------|
| Community | `cff`, `boguscf`, `substitution` | nothing (open source) |
| Enterprise Vault | + `string` encryption | `ZeroHackLicenseGate` |

The gate is token-gated, not folder-gated: the pass code exists and compiles in
this tree, but the *unlock* is an HMAC-SHA256 token derived as:

```
token = HMAC-SHA256(salt, client_id)
```

`tools/gen_license.py` mints tokens for a client id:

```sh
python3 tools/gen_license.py --client acme-fw --salt <CUSTOMER_SALT>
```

and the CLI consumes them as `--token`/`SHADOWC_TOKEN`:

```sh
SHADOWC_TOKEN=$TOKEN ./bin/shadowc --level enterprise --client-id acme-fw app.c -o out.bin
```

Without a matching token, `pipeline()` raises a `LicenseError` before any IR
touches the toolchain — a leaked Community build cannot silently upgrade.

## Salt lifecycle (production guidance)

1. Generate `--new-salt` → `tools/gen_license.py --salt <X> --new-salt`.
2. Store it out-of-band (secret manager / HSMs / a private repo). The demo salt
   `ZH_COMMUNITY_DEMO_2026` is public by design and unlocks nothing of value.
3. Per-customer salt per SKU: rotate at renewal, mint tokens at provisioning.
4. Rotate any leaked salt: tokens are deterministic per salt, so a leaked salt
   allows minting — rotate immediately and re-hardened.

## Production hardening (beyond the demo gate)

The offline HMAC path is a *handshake demo*. For real deployments wire
`ZeroHackLicenseGate` to a TLS license server (challenge not subject to replay,
offline grace periods, floating seats). The gate interface is designed so this
swap is internal to `gatekeeper.py` only.

## Why the Vault is not more than string encryption today

The honest lane: `string` XOR-injection removes the *cheap* harvest. The
ROADMAP lists the reserved-weave order (virtualization, opaque security, IR
hiding) as licensed additions. Keep the threat-model framing from
`docs/ARCHITECTURE.md`: obfuscation raises cost; it never deletes the
attacker's budget.

## Pricing / SKU note (no pricing shipped in-tree)

Ship three SKUs: Community (free), Enterprise Vault (per-seat/per-SKU annual),
and Mezzanine (CI/team) — the gate already separates the enforcement; the
storefront simply moves which salt + pass list a build gets.

## Legal

Dual license in-tree: Apache-2.0 for the community surface; the Vault
(`shadowc-string`) is product-licensed. See `LICENSE` and
`vault/README.md`.