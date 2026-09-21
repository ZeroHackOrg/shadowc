"""Unit tests for the python orchestration layer (no toolchain required)."""

import os
import subprocess
import sys

from pathlib import Path

import pytest

from gatekeeper import LicenseError
from shadowc.pipeline import (
    COMMUNITY_PASSES,
    VAULT_PASSES,
    PipelineError,
    Settings,
    ShadowCompiler,
)


def make_compiler(**overrides):
    defaults = dict(source="sample.c", salt="test-salt")
    defaults.update(overrides)
    return ShadowCompiler(Settings(**defaults))


def test_community_pipeline_is_community_only():
    comp = make_compiler(level="community")
    pipeline = comp.pipeline()
    assert pipeline.startswith("function(")
    assert "shadowc-string" not in pipeline
    assert all(name in pipeline for name in COMMUNITY_PASSES)


def test_min_pipeline_is_minimal():
    comp = make_compiler(level="min")
    assert comp.pipeline() == "function(shadowc-cff)"


def test_enterprise_pipeline_requires_token():
    comp = make_compiler(level="enterprise", client_id="acme", token="")
    with pytest.raises(LicenseError):
        comp.pipeline()

    token = __import__("gatekeeper").ZeroHackLicenseGate(salt="test-salt").mint("acme")
    comp = make_compiler(level="enterprise", client_id="acme", token=token)
    pipeline = comp.pipeline()
    for name in VAULT_PASSES:
        assert name in pipeline


def test_force_passes_override_level_gating():
    # Even enterprise selection cannot sneak the vault pass past an empty token
    comp = make_compiler(level="enterprise", force_passes=list(COMMUNITY_PASSES))
    assert "shadowc-string" not in comp.pipeline()


def test_unsupported_input_raises():
    comp = make_compiler(source="thing.py")
    with pytest.raises(PipelineError):
        comp._emit_ir(Path("thing.py"), None)


def test_plugin_discovery():
    comp = make_compiler()
    here = Path(__file__).resolve().parent.parent
    root = comp._repo_root()
    assert root == here or root.is_dir()


def test_tool_env_overrides(monkeypatch):
    comp = make_compiler()
    monkeypatch.setenv("SHADOWC_CLANG", "/opt/clang")
    assert comp._find_tool("clang") == "/opt/clang"


def test_cli_status_exits_zero_when_ready(monkeypatch, tmp_path):
    monkeypatch.setenv("SHADOWC_CLANG", "/bin/echo")
    from shadowc import cli

    plugin_file = tmp_path / "ShadowCPasses.so"
    plugin_file.write_bytes(b"")
    monkeypatch.setenv("SHADOWC_PLUGIN", str(plugin_file))
    rc = cli.main(["--status"])
    assert rc == 0


def test_cli_requires_source():
    from shadowc import cli

    assert cli.main([]) == 2