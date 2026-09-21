"""shadowc license gatekeeper.

Locks the Enterprise Vault passes (dynamic polymorphism, string
virtualization, anti-debug traps) behind a cryptographically derived access
token. The community repository ships the gate logic but *not* the vault
passes' unlock keys, keeping the public tree safe for academic evaluation
while the private vault stays product-licensed.

Design notes
------------
The token is a deterministic HMAC-SHA256 over ``client_id`` and a server-side
salt. This is intentionally an *authorization handshake demo layer*: the
canonical production binding validates against a ZeroHack license server over
TLS. Never embed a real production salt on a client.
"""

from __future__ import annotations

import hashlib
import hmac
import secrets
from dataclasses import dataclass


class LicenseError(Exception):
    """Raised when a vault unlock cannot be processed."""


@dataclass(frozen=True)
class LicenseToken:
    client_id: str
    salt: str
    token: str

    def was_granted(self) -> bool:
        return bool(self.token)


def derive_token(client_id: str, salt: str) -> str:
    """Deterministic per-client unlock token (demo/offline mode).

    Replaced in production by a server-side challenge signed over TLS; keep
    the offline form for single-machine labs and CI fixtures.
    """
    if not client_id:
        raise LicenseError("client_id must not be empty")
    digest = hmac.new(salt.encode("utf-8"), client_id.encode("utf-8"), hashlib.sha256)
    return digest.hexdigest()


def new_salt() -> str:
    """Fresh 32-byte random salt, used by the token minting tooling."""
    return secrets.token_hex(32)


class ZeroHackLicenseGate:
    """Verifies whether a token unlocks the Enterprise Vault passes.

    Parameters
    ----------
    salt:
        The configured vault salt. Community builds use a public demo salt;
        the private repository swaps in the licensed salt.
    """

    def __init__(self, salt: str = "ZH_COMMUNITY_DEMO_2026"):
        self.salt = salt
        #: Plugin pass names that require a granted token.
        self.vault_passes = (
            "shadowc-string",
            "shadowc-virt",
            "shadowc-trap",
            "shadowc-init",
        )

    # ------------------------------------------------------------------
    # Public API
    # ------------------------------------------------------------------
    def verify_unlock_token(self, client_id: str, provided_token: str) -> bool:
        """True when ``provided_token`` unlocks the vault passes."""
        expected = derive_token(client_id, self.salt)
        if not provided_token:
            return False
        return hmac.compare_digest(expected, provided_token)

    def require(self, client_id: str, provided_token: str, requested: str) -> None:
        """Raise :class:`LicenseError` when a vault pass is requested without a
        valid token."""
        if requested not in self.vault_passes:
            return
        if not self.verify_unlock_token(client_id, provided_token):
            raise LicenseError(
                f"[ZeroHack Labs] pass '{requested}' is Enterprise Vault only: "
                "a valid unlock token is required (see tools/gen_license.py)"
            )

    # ------------------------------------------------------------------
    # Convenience (CI / scripts)
    # ------------------------------------------------------------------
    def mint(self, client_id: str) -> str:
        """Mint a token for ``client_id`` against this gate's salt."""
        return derive_token(client_id, self.salt)

    def describe(self):
        return {
            "mode": "offline-hmac",
            "vault_passes": list(self.vault_passes),
            "salt_configured": bool(self.salt),
        }


__all__ = [
    "LicenseError",
    "LicenseToken",
    "ZeroHackLicenseGate",
    "derive_token",
    "new_salt",
]