# SPDX-License-Identifier: Apache-2.0
"""Qualified Work Profile authority cases."""
# ruff: noqa: F401,F403

from _work_authority_surface_support import *


def test_qualified_work_profile_resolves_retained_exact_source(monkeypatch, tmp_path):
    source = tmp_path / "installed" / "work-control"
    source.mkdir(parents=True)
    (source / "profile.json").write_text("{}\n", encoding="utf-8")
    profile_root = f"sha256:{'a' * 64}"
    monkeypatch.setattr(
        profile_lifecycle.storage_service,
        "profile_lifecycle",
        lambda *_args, **_kwargs: {
            "profile_suite_root": profile_root,
            "qualified": True,
            "activated": True,
            "removed": False,
            "latest_event": {"closure": {"profile_path": str(source / "profile.json")}},
        },
    )
    monkeypatch.setattr(
        profile_lifecycle.profile_sdk,
        "validate_source",
        lambda observed_source, observed_runtime: (
            {
                "inspection": {
                    "profile": {"id": "kungfu.work-control"},
                    "profile_suite_root": profile_root,
                }
            }
            if observed_source == source.resolve() and observed_runtime == tmp_path
            else (_ for _ in ()).throw(AssertionError("source/runtime drift"))
        ),
    )
    assert profile_lifecycle.resolve_qualified_work_profile(tmp_path) == {
        "id": "kungfu.work-control",
        "root": profile_root,
        "source": str(source.resolve()),
    }


def test_missing_work_profile_has_specific_fail_closed_diagnosis(monkeypatch, tmp_path):
    def missing(*_args, **_kwargs):
        raise ValueError("Profile not found: kungfu.work-control")

    monkeypatch.setattr(profile_lifecycle.storage_service, "profile_lifecycle", missing)

    with __import__("pytest").raises(
        profile_lifecycle.profile_sdk.ProfileSdkError,
        match="Work Control Profile is not installed",
    ) as error:
        profile_lifecycle.resolve_qualified_work_profile(tmp_path)

    assert error.value.diagnosis["code"] == "work-control-profile-not-installed"

    with __import__("pytest").raises(
        LocalRuntimeError,
        match="Work Control Profile is not installed",
    ) as runtime_error:
        WorkControlAuthority(tmp_path).inspect()

    assert runtime_error.value.code == "backend-unavailable"
    assert runtime_error.value.diagnostics[0]["code"] == (
        "work-control-profile-not-installed"
    )
