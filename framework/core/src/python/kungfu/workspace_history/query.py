# SPDX-License-Identifier: Apache-2.0

"""Work History lifecycle projection helpers for federated read models."""

from __future__ import annotations

from collections.abc import Callable, Mapping
from typing import Any

from kungfu.workspace import semantic_root
from kungfu.workspace_history.lifecycle import catalog_entry_root, reference_key


def bind_dispositions(
    catalog: Mapping[str, Any],
    store: Mapping[str, Any],
    resolver: Callable[[Mapping[str, Any]], Any],
) -> dict[str, Any]:
    """Bind only exact, still-unavailable component dispositions."""

    issues = list(store.get("issues") or [])
    terminal_entries: list[tuple[Mapping[str, Any], Mapping[str, Any]]] = []
    terminal_keys: set[str] = set()
    dispositions = store.get("component_dispositions") or {}
    for entry in catalog.get("entries") or []:
        entry_key = str(entry.get("identity_root") or entry.get("locator_key") or "")
        disposition = dispositions.get(entry_key)
        if disposition is None:
            continue
        if (
            disposition.get("action") != "terminal-unavailable"
            or disposition.get("catalog_entry_root") != catalog_entry_root(entry)
            or disposition.get("original_locator") != entry.get("locator")
        ):
            issues.append(
                {
                    "code": "work-history-component-disposition-mismatch",
                    "component_identity": entry_key,
                    "disposition_root": disposition.get("disposition_root"),
                }
            )
            continue
        # The receipt remains historical, but an exact recovered identity must
        # produce a new current observation instead of being suppressed.
        if resolver(entry) is not None:
            continue
        terminal_keys.add(entry_key)
        terminal_entries.append((entry, disposition))
    return {
        "issues": issues,
        "terminal_entries": terminal_entries,
        "terminal_keys": terminal_keys,
    }


def component_material(
    entry: Mapping[str, Any],
    availability: str,
    observed_at: str,
    disposition: Mapping[str, Any] | None = None,
) -> dict[str, Any]:
    """Build one unbound unavailable, terminal, or excluded component."""

    if availability not in {"unavailable", "terminal-unavailable", "excluded"}:
        raise ValueError("unsupported Work History component availability")
    locator = entry.get("locator")
    states = {
        "unavailable": ("unavailable", "catalog-locator-unavailable"),
        "terminal-unavailable": (
            "terminal-unavailable",
            "rooted-lifecycle-disposition",
        ),
        "excluded": ("excluded", "catalog-lifecycle-policy"),
    }
    state, reason = states[availability]
    workspace = {
        "schema": "kungfu.workspace.identity/v1",
        "workspace_id": entry.get("workspace_id"),
        "identity_root": entry.get("identity_root"),
        "identity_state": entry.get("identity_state") or "qualified",
        "workspace_kind": entry.get("workspace_kind"),
        "workspace_root": locator,
        "display_path": locator or "Home",
        "data_home": entry.get("data_home"),
        "initialized": False,
        "state": state,
        "resolution_reason": reason,
    }
    problems: list[dict[str, Any]] = []
    if availability == "unavailable":
        problems.append({"code": "workspace-unavailable", "locator": locator})
    elif availability == "excluded":
        problems.append(
            {
                "code": "workspace-excluded",
                "lifecycle": entry.get("lifecycle"),
                "exclusion_policy": entry.get("exclusion_policy"),
                "locator": locator,
            }
        )
    material = {
        "workspace": workspace,
        "availability": availability,
        "observed_at": observed_at,
        "catalog_observed_at": entry.get("observed_at"),
        "stale": availability == "unavailable"
        or (availability == "excluded" and not bool(entry.get("available"))),
        "cut_root": "",
        "query_proof_root": "",
        "initiatives": [],
        "assignments": [],
        "relations": [],
        "problems": problems,
    }
    if availability == "terminal-unavailable":
        material["disposition"] = {
            "classification": disposition.get("classification")
            if disposition
            else None,
            "action": disposition.get("action") if disposition else None,
            "disposition_root": disposition.get("disposition_root")
            if disposition
            else None,
            "available_authority_roots": disposition.get("available_authority_roots")
            if disposition
            else None,
            "unrecovered_reason": disposition.get("unrecovered_reason")
            if disposition
            else None,
            "affects_global_completeness": False,
        }
    return material


def resolve_references(
    resolved: list[dict[str, Any]],
    unresolved: list[dict[str, Any]],
    dispositions: Mapping[str, Mapping[str, Any]],
) -> tuple[list[dict[str, Any]], list[dict[str, Any]], list[dict[str, Any]]]:
    """Project exact terminal reference dispositions without hiding mismatches."""

    terminal: list[dict[str, Any]] = []
    remaining: list[dict[str, Any]] = []
    for row in unresolved:
        disposition = dispositions.get(reference_key(row))
        if disposition is None or any(
            (
                disposition.get("assignment_subject") != row.get("assignment_subject"),
                disposition.get("dependency_subject") != row.get("dependency_subject"),
                disposition.get("reference_kind") != row.get("kind"),
                disposition.get("action") != "terminal-reference-disposition",
            )
        ):
            remaining.append(row)
            continue
        projected = {
            **row,
            "resolution": disposition.get("resolution"),
            "classification": disposition.get("classification"),
            "disposition_root": disposition.get("disposition_root"),
            "available_authority_roots": disposition.get("available_authority_roots"),
            "successor_subject": disposition.get("successor_subject"),
            "affects_global_completeness": False,
        }
        terminal.append(projected)
        resolved.append(projected)
    resolved.sort(key=semantic_root)
    remaining.sort(key=semantic_root)
    terminal.sort(key=semantic_root)
    return resolved, remaining, terminal


def proof_fields(
    store: Mapping[str, Any],
    terminal_entries: list[tuple[Mapping[str, Any], Mapping[str, Any]]],
    reference_resolution: Mapping[str, Any],
) -> dict[str, Any]:
    return {
        "disposition_index_root": store.get("index_root") or "",
        "component_dispositions": [
            {
                "component_identity": disposition.get("component_identity"),
                "catalog_entry_root": disposition.get("catalog_entry_root"),
                "classification": disposition.get("classification"),
                "disposition_root": disposition.get("disposition_root"),
            }
            for _entry, disposition in terminal_entries
        ],
        "reference_dispositions": list(
            reference_resolution.get("terminal_dispositions") or []
        ),
    }
