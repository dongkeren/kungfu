# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import json
import os

from kungfu.workspace import (
    ensure_workspace_data_home,
    inspect_workspace,
    load_workspace_catalog,
    observe_workspace_locator,
    semantic_root,
)
from kungfu.workspace_federation import query_federation
from kungfu.workspace_federation_projection import _compose_global_work
from kungfu.workspace_history.lifecycle import (
    LIQUIDATION_EVIDENCE_SCHEMA,
    apply_work_history_liquidation,
    load_work_history_dispositions,
    plan_work_history_liquidation,
    reference_key,
)


ROOT_A = "sha256:" + "a" * 64
ROOT_B = "sha256:" + "b" * 64
ROOT_C = "sha256:" + "c" * 64


def _qualified_project(tmp_path, name):
    root = tmp_path / name
    root.mkdir()
    env = {"HOME": str(tmp_path)}
    candidate = inspect_workspace(str(root), env=env)
    assert candidate is not None
    ensure_workspace_data_home(candidate, f"{name} fixture")
    identity = inspect_workspace(str(root), env=env)
    assert identity is not None
    assert identity.identity_state == "qualified"
    return identity


def _evidence(query, **extra):
    body = {
        "schema": LIQUIDATION_EVIDENCE_SCHEMA,
        "query_catalog_cut": query["proof"]["catalog_cut"],
        "checked_evidence_sources": [
            "exact-catalog-entry",
            "exact-locator-identity-material",
            "retained-authority-root-index",
        ],
        "component_dispositions": [],
        "reference_dispositions": [],
        **extra,
    }
    return {**body, "evidence_root": semantic_root(body)}


def test_rooted_liquidation_keeps_terminal_component_visible_and_catalog_unchanged(
    tmp_path,
):
    env = {"HOME": str(tmp_path)}
    config_home = tmp_path / "config"
    current = _qualified_project(tmp_path, "current")
    missing = _qualified_project(tmp_path, "missing")
    for identity in (current, missing):
        observe_workspace_locator(
            identity,
            config_home=str(config_home),
            env=env,
        )
    os.rename(missing.workspace_root, tmp_path / "missing-retained-offline")
    before_catalog = load_workspace_catalog(str(config_home), env=env)
    before = query_federation(
        current,
        scope="all",
        config_home=str(config_home),
        env=env,
    )
    assert before["aggregate"]["unavailable_component_count"] == 1
    assert before["aggregate"]["complete"] is False

    plan = plan_work_history_liquidation(
        before,
        before_catalog,
        _evidence(before),
        transitioned_at="2026-08-23T00:00:00Z",
    )
    assert plan["counts"] == {
        "component_disposition_count": 1,
        "reference_disposition_count": 0,
        "unchecked_count": 0,
    }
    assert plan["writes"] == 0
    row = plan["component_dispositions"][0]
    assert row["original_locator"] == missing.workspace_root
    assert row["classification"] == "evidence-insufficient"
    assert row["action"] == "terminal-unavailable"
    assert (
        row["successor_observation_or_terminal_disposition_root"]
        == row["disposition_root"]
    )

    applied = apply_work_history_liquidation(
        plan,
        plan["plan_root"],
        config_home=str(config_home),
        env=env,
    )
    assert applied["executed"] is True
    assert (
        load_workspace_catalog(str(config_home), env=env)["catalog_cut"]
        == (before_catalog["catalog_cut"])
    )
    store = load_work_history_dispositions(str(config_home), env=env)
    assert store["issues"] == []
    assert len(store["component_dispositions"]) == 1

    after = query_federation(
        current,
        scope="all",
        config_home=str(config_home),
        env=env,
    )
    assert after["aggregate"]["complete"] is True
    assert after["aggregate"]["proof_ok"] is True
    assert after["aggregate"]["writes"] == 0
    assert after["aggregate"]["unavailable_component_count"] == 0
    assert after["aggregate"]["terminal_unavailable_component_count"] == 1
    terminal = [
        row
        for row in after["components"]
        if row["availability"] == "terminal-unavailable"
    ]
    assert len(terminal) == 1
    assert terminal[0]["workspace"]["identity_root"] == missing.identity_root
    assert terminal[0]["disposition"]["disposition_root"] == row["disposition_root"]

    os.rename(tmp_path / "missing-retained-offline", missing.workspace_root)
    recovered = query_federation(
        current,
        scope="all",
        config_home=str(config_home),
        env=env,
    )
    assert recovered["aggregate"]["complete"] is True
    assert recovered["aggregate"]["terminal_unavailable_component_count"] == 0
    assert any(
        component["availability"] == "available"
        and component["workspace"]["identity_root"] == missing.identity_root
        for component in recovered["components"]
    )
    assert (
        load_work_history_dispositions(str(config_home), env=env)[
            "component_dispositions"
        ][missing.identity_root]["disposition_root"]
        == row["disposition_root"]
    )


def test_invalid_disposition_receipt_fails_closed(tmp_path):
    env = {"HOME": str(tmp_path)}
    config_home = tmp_path / "config"
    current = _qualified_project(tmp_path, "current")
    missing = _qualified_project(tmp_path, "missing")
    for identity in (current, missing):
        observe_workspace_locator(
            identity,
            config_home=str(config_home),
            env=env,
        )
    os.rename(missing.workspace_root, tmp_path / "missing-retained-offline")
    query = query_federation(
        current,
        scope="all",
        config_home=str(config_home),
        env=env,
    )
    plan = plan_work_history_liquidation(
        query,
        load_workspace_catalog(str(config_home), env=env),
        _evidence(query),
        transitioned_at="2026-08-23T00:00:00Z",
    )
    applied = apply_work_history_liquidation(
        plan,
        plan["plan_root"],
        config_home=str(config_home),
        env=env,
    )
    receipt = next(
        path
        for path in applied["writes"]
        if path.endswith(".json") and "history-dispositions" in path
    )
    payload = json.loads(open(receipt, encoding="utf-8").read())
    payload["classification"] = "permanently-retired"
    with open(receipt, "w", encoding="utf-8") as stream:
        json.dump(payload, stream)

    result = query_federation(
        current,
        scope="all",
        config_home=str(config_home),
        env=env,
    )
    assert result["aggregate"]["complete"] is False
    assert result["aggregate"]["unavailable_component_count"] == 1
    assert result["aggregate"]["terminal_unavailable_component_count"] == 0
    assert any(
        row["code"] == "work-history-disposition-invalid"
        for row in result["proof"]["catalog_issues"]
    )


def test_terminal_candidate_is_not_recovered_through_parent_locator(tmp_path):
    env = {"HOME": str(tmp_path)}
    config_home = tmp_path / "config"
    current = _qualified_project(tmp_path, "current")
    missing = tmp_path / "removed-candidate"
    missing.mkdir()
    candidate = inspect_workspace(str(missing), env=env)
    assert candidate is not None
    assert candidate.identity_state == "locator-candidate"
    observe_workspace_locator(candidate, config_home=str(config_home), env=env)
    os.rmdir(missing)

    before = query_federation(
        current,
        scope="all",
        config_home=str(config_home),
        env=env,
    )
    assert before["aggregate"]["unavailable_component_count"] == 1
    plan = plan_work_history_liquidation(
        before,
        load_workspace_catalog(str(config_home), env=env),
        _evidence(before),
        transitioned_at="2026-08-23T00:00:00Z",
    )
    apply_work_history_liquidation(
        plan,
        plan["plan_root"],
        config_home=str(config_home),
        env=env,
    )

    after = query_federation(
        current,
        scope="all",
        config_home=str(config_home),
        env=env,
    )
    assert after["aggregate"]["complete"] is True
    assert after["aggregate"]["terminal_unavailable_component_count"] == 1
    assert after["aggregate"]["unavailable_component_count"] == 0


def test_unresolved_reference_can_only_close_through_exact_rooted_disposition():
    component = {
        "workspace": {"identity_root": ROOT_A, "workspace_id": "project:a"},
        "envelope": {},
        "initiatives": [],
        "assignments": [],
        "relations": [],
        "problems": [
            {
                "code": "unresolved-assignment-dependency",
                "assignment_subject": "kungfu:source",
                "dependency_id": "missing",
            }
        ],
    }
    unresolved_projection = _compose_global_work([component])
    unresolved = unresolved_projection["reference_resolution"]["unresolved"][0]
    key = reference_key(unresolved)
    disposition = {
        "target_kind": "reference",
        "reference_key": key,
        "assignment_subject": "kungfu:source",
        "dependency_subject": "kungfu:missing",
        "reference_kind": "legacy-assignment-dependency",
        "action": "terminal-reference-disposition",
        "resolution": "successor-observation",
        "classification": "superseded",
        "disposition_root": ROOT_B,
        "available_authority_roots": [ROOT_C],
        "successor_subject": "kungfu:successor",
    }
    resolved_projection = _compose_global_work(
        [component], reference_dispositions={key: disposition}
    )

    resolution = resolved_projection["reference_resolution"]
    assert resolution["unresolved"] == []
    assert resolution["terminal_dispositions"][0]["disposition_root"] == ROOT_B
    assert resolution["terminal_dispositions"][0]["successor_subject"] == (
        "kungfu:successor"
    )
