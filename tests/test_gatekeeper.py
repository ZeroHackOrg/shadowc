"""Unit tests for the licence gatekeeper (no toolchain required)."""

import pytest

from gatekeeper import LicenseError, ZeroHackLicenseGate, derive_token, new_salt


def test_derive_token_is_deterministic():
    assert derive_token("corp-a", "salt") == derive_token("corp-a", "salt")
    assert derive_token("corp-a", "salt") != derive_token("corp-a", "other")
    assert derive_token("corp-a", "salt") != derive_token("corp-b", "salt")


def test_verify_accepts_minted_token():
    gate = ZeroHackLicenseGate(salt="test-salt")
    token = gate.mint("alice")
    assert gate.verify_unlock_token("alice", token)
    assert not gate.verify_unlock_token("alice", "bogus")
    assert not gate.verify_unlock_token("bob", token)


def test_vault_requires_token_for_string_pass():
    gate = ZeroHackLicenseGate(salt="test-salt")
    # community passes pass through the gate untouched
    gate.require("alice", "", "shadowc-cff")
    # vault pass without token -> LicenseError
    with pytest.raises(LicenseError):
        gate.require("alice", "", "shadowc-string")
    # vault pass with a valid token -> ok
    token = gate.mint("alice")
    gate.require("alice", token, "shadowc-string")


def test_salt_rotation_invalidates_old_tokens():
    old = ZeroHackLicenseGate(salt="old-salt").mint("carol")
    assert not ZeroHackLicenseGate(salt="new-salt").verify_unlock_token("carol", old)


def test_new_salt_produces_hex_bytes():
    salt = new_salt()
    assert len(salt) == 64
    assert all(c in "0123456789abcdef" for c in salt)


def test_empty_client_id_rejected():
    with pytest.raises(LicenseError):
        derive_token("", "salt")