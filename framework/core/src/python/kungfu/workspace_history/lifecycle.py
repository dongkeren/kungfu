# SPDX-License-Identifier: Apache-2.0

"""Root-bound lifecycle dispositions for machine-local Work History gaps.

The locator Catalog and every component authority remain immutable inputs.  A
disposition is an additive, content-addressed statement about one exact
Catalog entry or one exact unresolved reference.  It never rewrites a
workspace, Work observation, locator, Fact, seal, or query proof.
"""

from __future__ import annotations

from datetime import datetime, timezone
import json
import os
import re
import tempfile
from typing import Any, Mapping

from kungfu.workspace import (
    load_workspace_catalog,
    semantic_root,
    workspace_catalog_path,
)


LIQUIDATION_EVIDENCE_SCHEMA = "kungfu.work-history.liquidation-evidence/v1"
LIQUIDATION_PLAN_SCHEMA = "kungfu.work-history.liquidation-plan/v1"
DISPOSITION_SCHEMA = "kungfu.work-history.disposition/v1"
DISPOSITION_INDEX_SCHEMA = "kungfu.work-history.disposition-index/v1"
DISPOSITION_INDEX_ROOT_SCHEMA = "kungfu.work-history.disposition-index-root/v1"
COMPONENT_KEY_SCHEMA = "kungfu.work-history.component-key/v1"
REFERENCE_KEY_SCHEMA = "kungfu.work-history.reference-key/v1"

_DERIVED_CATALOG_FIELDS = {"retained", "required", "exclusion_policy"}
_SHA256_ROOT = re.compile(r"sha256:[0-9a-f]{64}")
_GIT_ROOT = re.compile(r"git:[0-9a-f]{40}")


def disposition_index_path(
    config_home: str | None = None,
    *,
    env: Mapping[str, str] | None = None,
) -> str:
    return os.path.join(
        os.path.dirname(workspace_catalog_path(config_home, env=env)),
        "work-history-dispositions.json",
    )


def catalog_entry_root(entry: Mapping[str, Any]) -> str:
    persisted = {
        key: value for key, value in entry.items() if key not in _DERIVED_CATALOG_FIELDS
    }
    return semantic_root({"schema": COMPONENT_KEY_SCHEMA, "catalog_entry": persisted})


def reference_key(reference: Mapping[str, Any]) -> str:
    return semantic_root(
        {
            "schema": REFERENCE_KEY_SCHEMA,
            "kind": str(reference.get("kind") or ""),
            "assignment_subject": str(reference.get("assignment_subject") or ""),
            "dependency_subject": str(reference.get("dependency_subject") or ""),
        }
    )


def load_work_history_dispositions(
    config_home: str | None = None,
    *,
    env: Mapping[str, str] | None = None,
) -> dict[str, Any]:
    """Read and verify the current additive disposition index without writes."""

    path = disposition_index_path(config_home, env=env)
    if not os.path.exists(path):
        return {
            "schema": DISPOSITION_INDEX_SCHEMA,
            "component_dispositions": {},
            "reference_dispositions": {},
            "epoch": 0,
            "issues": [],
            "index_path": path,
            "index_root": "",
            "writes": [],
        }
    issues: list[dict[str, str]] = []
    try:
        with open(path, encoding="utf-8") as stream:
            index = json.load(stream)
        if (
            not isinstance(index, dict)
            or index.get("schema") != DISPOSITION_INDEX_SCHEMA
            or not isinstance(index.get("component_dispositions"), dict)
            or not isinstance(index.get("reference_dispositions"), dict)
        ):
            raise ValueError("Work History disposition index contract mismatch")
        declared_index_root = str(index.get("index_root") or "")
        index_body = {key: value for key, value in index.items() if key != "index_root"}
        expected_index_root = semantic_root(
            {"schema": DISPOSITION_INDEX_ROOT_SCHEMA, "index": index_body}
        )
        if declared_index_root != expected_index_root:
            raise ValueError("Work History disposition index root mismatch")
        components = _load_index_receipts(
            path, index["component_dispositions"], "component", issues
        )
        references = _load_index_receipts(
            path, index["reference_dispositions"], "reference", issues
        )
        return {
            **index,
            "component_dispositions": components,
            "reference_dispositions": references,
            "issues": issues,
            "index_path": path,
            "writes": [],
        }
    except (OSError, ValueError, json.JSONDecodeError) as error:
        return {
            "schema": DISPOSITION_INDEX_SCHEMA,
            "component_dispositions": {},
            "reference_dispositions": {},
            "epoch": 0,
            "issues": [
                {
                    "code": "work-history-disposition-index-invalid",
                    "path": path,
                    "message": str(error),
                }
            ],
            "index_path": path,
            "index_root": "",
            "writes": [],
        }


def plan_work_history_liquidation(
    query: Mapping[str, Any],
    catalog: Mapping[str, Any],
    evidence: Mapping[str, Any],
    *,
    transitioned_at: str | None = None,
) -> dict[str, Any]:
    """Build a complete one-to-one plan from one verified live query."""

    if evidence.get("schema") != LIQUIDATION_EVIDENCE_SCHEMA:
        raise ValueError("liquidation evidence contract mismatch")
    evidence_body = {
        key: value for key, value in evidence.items() if key != "evidence_root"
    }
    evidence_root = semantic_root(evidence_body)
    if evidence.get("evidence_root") != evidence_root:
        raise ValueError("liquidation evidence root mismatch")
    proof = query.get("proof") or {}
    verification = query.get("verification") or {}
    if verification.get("ok") is not True or query.get("writes"):
        raise ValueError(
            "liquidation requires one verified write-free Work History query"
        )
    if proof.get("catalog_cut") != catalog.get("catalog_cut"):
        raise ValueError("query and Catalog cuts do not match")
    if evidence.get("query_catalog_cut") != proof.get("catalog_cut"):
        raise ValueError("liquidation evidence does not bind the live Catalog cut")
    checks = list(evidence.get("checked_evidence_sources") or [])
    if not checks or any(not str(value).strip() for value in checks):
        raise ValueError("liquidation evidence must name every checked evidence source")
    transitioned_at = transitioned_at or _now()
    _require_timestamp(transitioned_at)

    catalog_entries = list(catalog.get("entries") or [])
    by_identity = {
        str(row.get("identity_root") or ""): row
        for row in catalog_entries
        if row.get("identity_root")
    }
    by_locator = {
        (str(row.get("workspace_id") or ""), str(row.get("data_home") or "")): row
        for row in catalog_entries
    }
    component_declarations = list(evidence.get("component_dispositions") or [])
    component_overrides = {
        str(row.get("component_identity") or ""): row for row in component_declarations
    }
    if len(component_overrides) != len(component_declarations):
        raise ValueError("component evidence contains a duplicate identity")
    component_rows: list[dict[str, Any]] = []
    live_component_keys: set[str] = set()
    problem_components = [
        row
        for row in query.get("components") or []
        if row.get("availability") == "unavailable" or bool(row.get("stale"))
    ]
    for component in problem_components:
        workspace = component.get("workspace") or {}
        identity_root = str(workspace.get("identity_root") or "")
        entry = by_identity.get(identity_root) or by_locator.get(
            (
                str(workspace.get("workspace_id") or ""),
                str(workspace.get("data_home") or ""),
            )
        )
        if entry is None:
            raise ValueError("unavailable component has no exact Catalog entry")
        entry_key = str(entry.get("identity_root") or entry.get("locator_key") or "")
        if not entry_key:
            raise ValueError("Catalog entry has no stable component key")
        live_component_keys.add(entry_key)
        override = component_overrides.get(entry_key, {})
        exact_state = _exact_locator_state(entry)
        classification = str(
            override.get("classification")
            or (
                "permanently-retired"
                if exact_state["state"] == "identity-mismatch"
                else "evidence-insufficient"
            )
        )
        if classification not in {"permanently-retired", "evidence-insufficient"}:
            raise ValueError(
                "a still-unavailable component cannot claim a restored or "
                "temporary terminal classification"
            )
        action = str(override.get("action") or "terminal-unavailable")
        if action != "terminal-unavailable":
            raise ValueError(
                "a still-unavailable component requires terminal-unavailable action"
            )
        problems = list(component.get("problems") or [])
        authority_roots = _authority_roots(
            [
                entry_key,
                catalog.get("catalog_cut"),
                proof.get("proof_root"),
                (component.get("envelope") or {}).get("envelope_root"),
                (component.get("envelope") or {}).get("component_result_root"),
                *list(override.get("available_authority_roots") or []),
            ]
        )
        unrecovered_reason = str(
            override.get("unrecovered_reason") or exact_state["reason"]
        ).strip()
        if not unrecovered_reason:
            raise ValueError("component liquidation requires an unrecovered reason")
        core = {
            "schema": DISPOSITION_SCHEMA,
            "target_kind": "component",
            "component_identity": entry_key,
            "catalog_entry_root": catalog_entry_root(entry),
            "original_locator": entry.get("locator"),
            "locator_type": _locator_type(str(entry.get("locator") or "")),
            "original_problem_code": str(
                (problems[0] if problems else {}).get("code") or "workspace-unavailable"
            ),
            "available_authority_roots": authority_roots,
            "classification": classification,
            "action": action,
            "checked_evidence_sources": sorted(
                set(checks + list(override.get("checked_evidence_sources") or []))
            ),
            "exact_locator_verification": exact_state,
            "verification": {
                "query_proof_ok": True,
                "query_writes": 0,
                "catalog_cut": catalog.get("catalog_cut"),
                "component_envelope_root": (component.get("envelope") or {}).get(
                    "envelope_root"
                ),
            },
            "unrecovered_reason": unrecovered_reason,
            "affects_global_completeness": False,
            "transitioned_at": transitioned_at,
            "liquidation_evidence_root": evidence_root,
        }
        component_rows.append(_rooted_disposition(core))

    unresolved = list(
        (query.get("global_work") or {})
        .get("reference_resolution", {})
        .get("unresolved")
        or []
    )
    if set(component_overrides) - live_component_keys:
        raise ValueError("component evidence contains an item outside the live query")
    reference_declarations = list(evidence.get("reference_dispositions") or [])
    declared_references = {reference_key(row): row for row in reference_declarations}
    if len(declared_references) != len(reference_declarations):
        raise ValueError("reference evidence contains a duplicate identity")
    reference_rows: list[dict[str, Any]] = []
    for reference in unresolved:
        key = reference_key(reference)
        declaration = declared_references.get(key)
        if declaration is None:
            raise ValueError(
                "every unresolved reference requires one exact disposition"
            )
        authority_roots = _authority_roots(
            [
                proof.get("proof_root"),
                *list(declaration.get("available_authority_roots") or []),
            ],
            allow_git=True,
        )
        if len(authority_roots) < 2:
            raise ValueError(
                "reference disposition requires a root beyond the query proof"
            )
        classification = str(declaration.get("classification") or "")
        resolution = str(declaration.get("resolution") or "")
        if classification not in {"superseded", "satisfied", "evidence-insufficient"}:
            raise ValueError("unsupported reference disposition classification")
        if resolution not in {"successor-observation", "terminal-disposition"}:
            raise ValueError("unsupported reference disposition resolution")
        reason = str(declaration.get("unrecovered_reason") or "").strip()
        if not reason:
            raise ValueError("reference disposition requires an exact reason")
        core = {
            "schema": DISPOSITION_SCHEMA,
            "target_kind": "reference",
            "reference_key": key,
            "assignment_subject": reference.get("assignment_subject"),
            "dependency_subject": reference.get("dependency_subject"),
            "reference_kind": reference.get("kind"),
            "original_problem_code": reference.get("code"),
            "available_authority_roots": authority_roots,
            "classification": classification,
            "action": "terminal-reference-disposition",
            "resolution": resolution,
            "successor_subject": declaration.get("successor_subject"),
            "checked_evidence_sources": sorted(
                set(checks + list(declaration.get("checked_evidence_sources") or []))
            ),
            "verification": {
                "query_proof_ok": True,
                "query_writes": 0,
                "query_proof_root": proof.get("proof_root"),
            },
            "unrecovered_reason": reason,
            "affects_global_completeness": False,
            "transitioned_at": transitioned_at,
            "liquidation_evidence_root": evidence_root,
        }
        reference_rows.append(_rooted_disposition(core))

    if set(declared_references) != {reference_key(row) for row in unresolved}:
        raise ValueError("reference evidence contains an item outside the live query")
    component_rows.sort(key=lambda row: row["component_identity"])
    reference_rows.sort(key=lambda row: row["reference_key"])
    body = {
        "schema": LIQUIDATION_PLAN_SCHEMA,
        "catalog_cut": catalog.get("catalog_cut"),
        "query_proof_root": proof.get("proof_root"),
        "query_verification_root": verification.get("verification_root"),
        "liquidation_evidence_root": evidence_root,
        "transitioned_at": transitioned_at,
        "component_dispositions": component_rows,
        "reference_dispositions": reference_rows,
        "counts": {
            "component_disposition_count": len(component_rows),
            "reference_disposition_count": len(reference_rows),
            "unchecked_count": 0,
        },
        "writes": 0,
    }
    return {**body, "plan_root": semantic_root(body), "executed": False}


def apply_work_history_liquidation(
    plan: Mapping[str, Any],
    expected_plan_root: str,
    *,
    config_home: str | None = None,
    env: Mapping[str, str] | None = None,
) -> dict[str, Any]:
    """Apply one exact plan as immutable receipts plus a rooted current index."""

    plan_body = {
        key: value
        for key, value in plan.items()
        if key not in {"plan_root", "executed"}
    }
    actual_plan_root = semantic_root(plan_body)
    if plan.get("schema") != LIQUIDATION_PLAN_SCHEMA:
        raise ValueError("liquidation plan contract mismatch")
    if (
        plan.get("plan_root") != actual_plan_root
        or expected_plan_root != actual_plan_root
    ):
        raise ValueError("liquidation plan root mismatch")
    unchecked = (plan.get("counts") or {}).get("unchecked_count")
    if unchecked is None or int(unchecked) != 0:
        raise ValueError("liquidation plan contains unchecked items")
    catalog = load_workspace_catalog(config_home, env=env)
    if catalog.get("catalog_cut") != plan.get("catalog_cut"):
        raise ValueError("Catalog changed after liquidation planning")
    current_entries = {
        str(row.get("identity_root") or row.get("locator_key") or ""): row
        for row in catalog.get("entries") or []
    }
    for row in plan.get("component_dispositions") or []:
        entry = current_entries.get(str(row.get("component_identity") or ""))
        if entry is None or catalog_entry_root(entry) != row.get("catalog_entry_root"):
            raise ValueError("target Catalog entry changed after liquidation planning")
        _verify_rooted_disposition(row)
    for row in plan.get("reference_dispositions") or []:
        _verify_rooted_disposition(row)

    current = load_work_history_dispositions(config_home, env=env)
    if current["issues"]:
        raise ValueError(
            "invalid Work History disposition index must be repaired first"
        )
    component_index = {
        key: value["disposition_root"]
        for key, value in current["component_dispositions"].items()
    }
    reference_index = {
        key: value["disposition_root"]
        for key, value in current["reference_dispositions"].items()
    }
    writes: list[str] = []
    index_path = disposition_index_path(config_home, env=env)
    for row in plan.get("component_dispositions") or []:
        key = str(row["component_identity"])
        _admit_disposition(index_path, row, writes)
        component_index[key] = str(row["disposition_root"])
    for row in plan.get("reference_dispositions") or []:
        key = str(row["reference_key"])
        _admit_disposition(index_path, row, writes)
        reference_index[key] = str(row["disposition_root"])
    index_body = {
        "schema": DISPOSITION_INDEX_SCHEMA,
        "component_dispositions": dict(sorted(component_index.items())),
        "reference_dispositions": dict(sorted(reference_index.items())),
        "epoch": int(current.get("epoch") or 0) + 1,
        "updated_at": plan.get("transitioned_at"),
        "plan_root": actual_plan_root,
    }
    index = {
        **index_body,
        "index_root": semantic_root(
            {"schema": DISPOSITION_INDEX_ROOT_SCHEMA, "index": index_body}
        ),
    }
    _write_json_atomic(index_path, index)
    writes.append(index_path)
    return {
        **plan_body,
        "plan_root": actual_plan_root,
        "executed": True,
        "index_root": index["index_root"],
        "index_path": index_path,
        "writes": writes,
    }


def save_work_history_liquidation_plan(
    path: str, plan: Mapping[str, Any]
) -> dict[str, Any]:
    """Persist one exact dry-run plan for later expected-root execution."""

    plan_body = {
        key: value
        for key, value in plan.items()
        if key not in {"plan_root", "executed"}
    }
    if (
        plan.get("schema") != LIQUIDATION_PLAN_SCHEMA
        or plan.get("plan_root") != semantic_root(plan_body)
        or plan.get("executed") is not False
    ):
        raise ValueError("only a verified dry-run liquidation plan can be saved")
    resolved = os.path.realpath(os.path.abspath(path))
    _write_json_atomic(resolved, plan)
    return {"plan_path": resolved, "plan_root": plan["plan_root"], "writes": [resolved]}


def _rooted_disposition(core: Mapping[str, Any]) -> dict[str, Any]:
    root = semantic_root(core)
    return {
        **core,
        "disposition_root": root,
        "successor_observation_or_terminal_disposition_root": root,
    }


def _verify_rooted_disposition(row: Mapping[str, Any]) -> None:
    core = {
        key: value
        for key, value in row.items()
        if key
        not in {
            "disposition_root",
            "successor_observation_or_terminal_disposition_root",
        }
    }
    root = semantic_root(core)
    if (
        row.get("schema") != DISPOSITION_SCHEMA
        or row.get("disposition_root") != root
        or row.get("successor_observation_or_terminal_disposition_root") != root
    ):
        raise ValueError("Work History disposition root mismatch")


def _load_index_receipts(
    index_path: str,
    index: Mapping[str, Any],
    target_kind: str,
    issues: list[dict[str, str]],
) -> dict[str, dict[str, Any]]:
    loaded: dict[str, dict[str, Any]] = {}
    for key, root in sorted(index.items()):
        if not _SHA256_ROOT.fullmatch(str(root)):
            issues.append(
                {
                    "code": "work-history-disposition-invalid",
                    "path": index_path,
                    "message": "disposition index contains an invalid receipt root",
                }
            )
            continue
        receipt_path = _receipt_path(index_path, str(root))
        try:
            with open(receipt_path, encoding="utf-8") as stream:
                receipt = json.load(stream)
            _verify_rooted_disposition(receipt)
            receipt_key = (
                receipt.get("component_identity")
                if target_kind == "component"
                else receipt.get("reference_key")
            )
            if (
                receipt.get("target_kind") != target_kind
                or receipt_key != key
                or receipt.get("disposition_root") != root
            ):
                raise ValueError("disposition index binding mismatch")
            loaded[str(key)] = receipt
        except (OSError, ValueError, json.JSONDecodeError) as error:
            issues.append(
                {
                    "code": "work-history-disposition-invalid",
                    "path": receipt_path,
                    "message": str(error),
                }
            )
    return loaded


def _admit_disposition(
    index_path: str, row: Mapping[str, Any], writes: list[str]
) -> None:
    receipt_path = _receipt_path(index_path, str(row["disposition_root"]))
    if os.path.exists(receipt_path):
        with open(receipt_path, encoding="utf-8") as stream:
            existing = json.load(stream)
        if existing != row:
            raise ValueError("existing disposition receipt bytes do not match")
        return
    _write_json_atomic(receipt_path, row)
    writes.append(receipt_path)


def _receipt_path(index_path: str, root: str) -> str:
    digest = root.removeprefix("sha256:")
    return os.path.join(
        os.path.dirname(index_path),
        "history-dispositions",
        "sha256",
        digest[:2],
        digest + ".json",
    )


def _exact_locator_state(entry: Mapping[str, Any]) -> dict[str, Any]:
    locator = str(entry.get("locator") or "")
    data_home = str(entry.get("data_home") or "")
    identity_path = os.path.join(data_home, "workspace-identity.json")
    if not locator or not os.path.isdir(locator):
        return {
            "state": "locator-absent",
            "locator_exists": False,
            "data_home_exists": os.path.isdir(data_home),
            "identity_material_exists": os.path.isfile(identity_path),
            "reason": "the exact original locator is absent and no matching readable workspace identity is available",
        }
    actual_root = ""
    try:
        with open(identity_path, encoding="utf-8") as stream:
            actual_root = str(json.load(stream).get("identityRoot") or "")
    except (OSError, json.JSONDecodeError):
        actual_root = ""
    expected_root = str(entry.get("identity_root") or "")
    if not actual_root:
        return {
            "state": "identity-material-absent",
            "locator_exists": True,
            "data_home_exists": os.path.isdir(data_home),
            "identity_material_exists": False,
            "actual_identity_root": "",
            "reason": "the exact locator exists but has no verifiable workspace identity material",
        }
    if expected_root and actual_root != expected_root:
        return {
            "state": "identity-mismatch",
            "locator_exists": True,
            "data_home_exists": os.path.isdir(data_home),
            "identity_material_exists": True,
            "actual_identity_root": actual_root,
            "reason": "the exact locator now contains a different workspace identity and cannot be rebound to the historical component",
        }
    return {
        "state": "identity-readable",
        "locator_exists": True,
        "data_home_exists": os.path.isdir(data_home),
        "identity_material_exists": True,
        "actual_identity_root": actual_root,
        "reason": "the component remained unreadable despite exact matching identity material",
    }


def _locator_type(locator: str) -> str:
    if locator.startswith("/Volumes/"):
        return "removable-media"
    if locator.startswith("/private/var/") or locator.startswith("/private/tmp/"):
        return "temporary-runtime"
    if "/Worktrees/" in locator:
        return "isolated-worktree"
    return "local-path"


def _authority_roots(values: list[Any], *, allow_git: bool = False) -> list[str]:
    roots: set[str] = set()
    for value in values:
        text = str(value or "")
        if _SHA256_ROOT.fullmatch(text):
            roots.add(text)
        elif allow_git and _GIT_ROOT.fullmatch(text):
            roots.add(text)
    return sorted(roots)


def _require_timestamp(value: str) -> None:
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError as error:
        raise ValueError("liquidation timestamp must be ISO-8601") from error
    if parsed.tzinfo is None:
        raise ValueError("liquidation timestamp must include a timezone")


def _now() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def _write_json_atomic(path: str, payload: Mapping[str, Any]) -> None:
    directory = os.path.dirname(path)
    os.makedirs(directory, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(prefix=".tmp-", dir=directory)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
            json.dump(
                payload,
                stream,
                ensure_ascii=False,
                sort_keys=True,
                separators=(",", ":"),
            )
            stream.write("\n")
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)
