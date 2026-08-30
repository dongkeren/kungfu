# SPDX-License-Identifier: Apache-2.0

import time
from pathlib import Path
from typing import Any

import kungfu

from kungfu.action_envelope import canonical_json_bytes, payload_hash
from kungfu.storage._service_episode import (
    _episode_close_edge as _episode_close_edge,
    _episode_write_edge as _episode_write_edge,
    _episode_write_options as _episode_write_options,
    episode_abort as episode_abort,
    episode_attach_frame as episode_attach_frame,
    episode_attach_ref as episode_attach_ref,
    episode_begin as episode_begin,
    episode_end as episode_end,
    episode_heartbeat as episode_heartbeat,
    episode_inspect as episode_inspect,
    episode_list as episode_list,
    episode_projection_rebuild as episode_projection_rebuild,
    episode_recover as episode_recover,
    episode_recovery_execute as episode_recovery_execute,
    episode_recovery_plan as episode_recovery_plan,
)
from kungfu.storage._service_fact import (
    action_runtime as action_runtime,
    build_fact_query_definition as build_fact_query_definition,
    compile_fact_query_sql as compile_fact_query_sql,
    fact_changelog as fact_changelog,
    fact_contract as fact_contract,
    fact_declare_contract_world as fact_declare_contract_world,
    fact_declare_surface as fact_declare_surface,
    fact_kernel as fact_kernel,
    fact_kernel_backend_parity as fact_kernel_backend_parity,
    fact_kernel_export as fact_kernel_export,
    fact_kernel_fsck as fact_kernel_fsck,
    fact_kernel_import as fact_kernel_import,
    fact_kernel_rebuild_projections as fact_kernel_rebuild_projections,
    fact_kernel_retention_plan as fact_kernel_retention_plan,
    fact_library_contract as fact_library_contract,
    fact_library_export as fact_library_export,
    fact_library_import as fact_library_import,
    fact_material_list as fact_material_list,
    fact_material_put as fact_material_put,
    fact_observe as fact_observe,
    fact_profile_shadow_compare as fact_profile_shadow_compare,
    fact_profile_shadow_inspect as fact_profile_shadow_inspect,
    fact_profile_shadow_project as fact_profile_shadow_project,
    fact_query as fact_query,
    fact_query_conformance as fact_query_conformance,
    fact_query_definition as fact_query_definition,
    fact_state as fact_state,
    fact_type_create as fact_type_create,
    fact_type_list as fact_type_list,
    kfx_registry as kfx_registry,
    profile_lifecycle as profile_lifecycle,
    query_plan as query_plan,
    saved_query_catalog as saved_query_catalog,
)
from kungfu.storage.kfx_service import (
    kfx_registry as _kfx_registry_impl,
    kfx_runtime_contract as kfx_runtime_contract,
    validate_kfx_runtime_document as validate_kfx_runtime_document,
)
from kungfu.storage.transfer import StorageTransfer, _binding_json, _u64

PAYLOAD_STATE_PRESENT = "present"
PAYLOAD_STATES = ("present", "redacted", "absent", "missing")
CONTENT_TYPE_JSON = "application/json"
SOURCE_REGISTRY_SCHEMA = "kungfu.storage.source-registry/v1"
MANIFEST_CATALOG_SCHEMA = "kungfu.storage.manifest-catalog/v1"
PROJECTION_SOURCE_REGISTRY = "source-registry-sqlite"
PROJECTION_MANIFEST_CATALOG = "manifest-catalog-sqlite"
PROJECTION_ATLAS_JOURNAL_FOLD = "atlas-journal-fold"
RUNTIME_STORAGE_SERVICE_SCHEMA = "kungfu.runtime.storage-service/v1"

_kfx_registry = _kfx_registry_impl


def _runtime():
    return kungfu.__binding__.runtime


for _facade_callable in (
    _episode_close_edge,
    _episode_write_edge,
    _episode_write_options,
    episode_abort,
    episode_attach_frame,
    episode_attach_ref,
    episode_begin,
    episode_end,
    episode_heartbeat,
    episode_inspect,
    episode_list,
    episode_projection_rebuild,
    episode_recover,
    episode_recovery_execute,
    episode_recovery_plan,
    action_runtime,
    build_fact_query_definition,
    compile_fact_query_sql,
    fact_changelog,
    fact_contract,
    fact_declare_contract_world,
    fact_declare_surface,
    fact_kernel,
    fact_kernel_backend_parity,
    fact_kernel_export,
    fact_kernel_fsck,
    fact_kernel_import,
    fact_kernel_rebuild_projections,
    fact_kernel_retention_plan,
    fact_library_contract,
    fact_library_export,
    fact_library_import,
    fact_material_list,
    fact_material_put,
    fact_observe,
    fact_profile_shadow_compare,
    fact_profile_shadow_inspect,
    fact_profile_shadow_project,
    fact_query,
    fact_query_conformance,
    fact_query_definition,
    fact_state,
    fact_type_create,
    fact_type_list,
    kfx_registry,
    profile_lifecycle,
    query_plan,
    saved_query_catalog,
):
    _facade_callable.__module__ = __name__
del _facade_callable


def service_capabilities() -> dict[str, Any]:
    return dict(_runtime().storage_service_capabilities())


def _runtime_service_request(
    operation: str,
    runtime_dir: str | Path,
    *,
    scope: str = "all",
    source_id: str | None = None,
    dry_run: bool = True,
    verify: bool = True,
    range_filter: dict[str, Any] | None = None,
    artifact_uri: str | None = None,
) -> dict[str, Any]:
    request = _runtime().make_storage_service_request(
        operation,
        str(runtime_dir),
        {
            "scope": scope,
            "source_id": source_id,
            "dry_run": dry_run,
            "verify": verify,
            "range": range_filter or {},
            "artifact_uri": artifact_uri or "",
        },
    )
    if request.get("schema") != RUNTIME_STORAGE_SERVICE_SCHEMA:
        raise RuntimeError(f"invalid runtime storage service request: {request}")
    return dict(request)


def root_dir(runtime_dir: str | Path) -> Path:
    return Path(runtime_dir) / "storage"


def payload_path(runtime_dir: str | Path, digest: str) -> Path:
    # KF-ADR-019f86da-4f90-7828-9142-46f9bca4b0f5: payload bodies are opaque content-addressed bytes named by the
    # content hash alone (no format-implying extension). Must match the C++
    # runtime storage service payload_path.
    return root_dir(runtime_dir) / "payloads" / digest[:2] / digest


def write_payload_bytes(runtime_dir: str | Path, digest: str, raw: bytes) -> str:
    return str(_runtime().write_storage_payload_bytes(str(runtime_dir), digest, raw))


def verify_import_manifest(manifest: dict[str, Any]) -> list[dict[str, Any]]:
    return [
        dict(issue) for issue in _runtime().verify_storage_import_manifest(manifest)
    ]


def accept_manifest(
    runtime_dir: str | Path, manifest: dict[str, Any]
) -> dict[str, Any]:
    """Accept one import manifest into the kernel journals (KF-ADR-019f86da-4f90-7828-9142-46f9bca4b0f5).

    ``manifest`` is the adapter-edge input document; the accepted facts are
    Hana-core journal records and the return value is their JSON edge
    projection.
    """

    return dict(_runtime().accept_storage_manifest(str(runtime_dir), manifest))


def load_latest_manifest(
    runtime_dir: str | Path, source_id: str
) -> dict[str, Any] | None:
    data = _runtime().load_storage_latest_manifest(str(runtime_dir), source_id)
    if data is None:
        return None
    return data if isinstance(data, dict) else None


def list_sources(runtime_dir: str | Path) -> list[dict[str, Any]]:
    return list(status(runtime_dir).get("sources", []))


def status(
    runtime_dir: str | Path,
    *,
    source_id: str | None = None,
) -> dict[str, Any]:
    return dict(_runtime().storage_status_typed(str(runtime_dir), source_id))


def backend_status(
    runtime_dir: str | Path, *, provider: str | None = None
) -> dict[str, Any]:
    """Inspect the authoritative provider binding and any resumable cut."""

    return dict(
        _runtime().run_storage_service_operation(
            "backend_status",
            str(runtime_dir),
            {"provider": provider} if provider else {},
        )
    )


def backend_switch(
    runtime_dir: str | Path,
    *,
    target_provider: str,
    expected_generation: int | None = None,
    qualification_fail_after_copied_objects: int | None = None,
) -> dict[str, Any]:
    """Copy, verify, and atomically bind a different embedded provider."""

    options: dict[str, Any] = {"target_provider": target_provider}
    if expected_generation is not None:
        options["expected_generation"] = _u64(expected_generation)
    if qualification_fail_after_copied_objects is not None:
        options["qualification_fail_after_copied_objects"] = _u64(
            qualification_fail_after_copied_objects
        )
    return dict(
        _runtime().run_storage_service_operation(
            "backend_switch", str(runtime_dir), options
        )
    )


def backend_rollback(
    runtime_dir: str | Path, *, expected_generation: int | None = None
) -> dict[str, Any]:
    """Reverse-sync to the retained provider and publish a new generation."""

    options: dict[str, Any] = {}
    if expected_generation is not None:
        options["expected_generation"] = _u64(expected_generation)
    return dict(
        _runtime().run_storage_service_operation(
            "backend_rollback", str(runtime_dir), options
        )
    )


def layout(
    runtime_dir: str | Path,
    *,
    runtime_home: str | Path | None = None,
    config_home: str | Path | None = None,
    provider: str | None = None,
) -> dict[str, Any]:
    return dict(
        _runtime().storage_layout_typed(
            str(runtime_dir),
            runtime_home=str(runtime_home) if runtime_home is not None else "",
            config_home=str(config_home) if config_home is not None else "",
            provider=provider or "",
        )
    )


def _entries_for_manifest(
    manifest: dict[str, Any], range_filter: dict[str, Any] | None = None
) -> list[dict[str, Any]]:
    entries = manifest.get("entries", [])
    if range_filter:
        entries = _runtime().filter_storage_manifest_entries(entries, range_filter)
    return [dict(entry) for entry in entries if isinstance(entry, dict)]


def fsck(
    runtime_dir: str | Path,
    *,
    source_id: str | None = None,
    episode_id: int | None = None,
    verify_frames: bool = False,
) -> dict[str, Any]:
    # verify_frames re-opens the event journals the Episode manifest claims
    # frames from and verifies each attached receipt (presence, header fields,
    # recomputed checksums). Episode-scope only; it reads every referenced
    # journal, so it stays opt-in.
    return dict(
        _runtime().storage_fsck_typed(
            str(runtime_dir),
            source_id=source_id,
            episode_id=episode_id or 0,
            verify_frames=verify_frames,
        )
    )


def repair_plan(
    runtime_dir: str | Path,
    *,
    source_id: str | None = None,
    episode_id: int | None = None,
    dry_run: bool = True,
) -> dict[str, Any]:
    return dict(
        _runtime().storage_repair_plan_typed(
            str(runtime_dir),
            source_id=source_id,
            episode_id=episode_id or 0,
            dry_run=dry_run,
        )
    )


def repair_fetch(
    runtime_dir: str | Path,
    *,
    source_id: str | None = None,
    episode_id: int | None = None,
    out_path: str | Path | None = None,
    dry_run: bool = True,
) -> dict[str, Any]:
    scope = "episode" if episode_id else ("source" if source_id else "all")
    return dict(
        _runtime().run_storage_service_operation(
            "repair_fetch",
            str(runtime_dir),
            {
                "scope": scope,
                "source_id": source_id,
                "episode_id": _u64(episode_id),
                "dry_run": dry_run,
                "artifact_uri": str(out_path) if out_path else "",
            },
        )
    )


def repair_apply(
    runtime_dir: str | Path,
    repair_input: dict[str, Any],
    *,
    source_id: str | None = None,
    episode_id: int | None = None,
    dry_run: bool = True,
) -> dict[str, Any]:
    scope = "episode" if episode_id else ("source" if source_id else "all")
    return dict(
        _runtime().run_storage_service_operation(
            "repair_apply",
            str(runtime_dir),
            {
                "scope": scope,
                "source_id": source_id,
                "episode_id": _u64(episode_id),
                "dry_run": dry_run,
                "bundle": _binding_json(repair_input),
            },
        )
    )


def source_register(
    runtime_dir: str | Path,
    *,
    source_id: str,
    kind: str = "local",
    coordinate: str = "",
    head: str = "",
) -> dict[str, Any]:
    """Register a source in the source-registry kernel journal (KF-ADR-019f86da-4f90-7828-9142-46f9bca4b0f5)."""

    return dict(
        _runtime().storage_source_register_typed(
            str(runtime_dir),
            source_id=source_id,
            kind=kind,
            coordinate=coordinate,
            head=head,
        )
    )


def source_inspect(runtime_dir: str | Path, *, source_id: str) -> dict[str, Any]:
    """Fold the source-registry journal into one source's edge view."""

    return dict(
        _runtime().storage_source_inspect_typed(str(runtime_dir), source_id=source_id)
    )


def rebuild_index(
    runtime_dir: str | Path,
    *,
    source_id: str | None = None,
    dry_run: bool = False,
) -> dict[str, Any]:
    """Rebuild the source registry projection from accepted manifests."""

    return dict(
        _runtime().storage_rebuild_index_typed(
            str(runtime_dir),
            source_id=source_id,
            dry_run=dry_run,
        )
    )


def gc_plan(
    runtime_dir: str | Path,
    *,
    source_id: str | None = None,
    dry_run: bool = True,
) -> dict[str, Any]:
    return dict(
        _runtime().storage_gc_plan_typed(
            str(runtime_dir),
            source_id=source_id,
            dry_run=dry_run,
        )
    )


def compact_plan(
    runtime_dir: str | Path,
    *,
    source_id: str | None = None,
    dry_run: bool = True,
) -> dict[str, Any]:
    return dict(
        _runtime().storage_compact_plan_typed(
            str(runtime_dir),
            source_id=source_id,
            dry_run=dry_run,
        )
    )


def verify_local_sync(
    runtime_dir: str | Path,
    *,
    source_id: str,
) -> dict[str, Any]:
    return dict(
        _runtime().run_storage_service_operation(
            "verify_sync",
            str(runtime_dir),
            {
                "scope": "source",
                "source_id": source_id,
            },
        )
    )


def _typed_query_edge_projection(result: dict[str, Any]) -> dict[str, Any]:
    query_names = {
        0: "sources",
        1: "manifests",
        2: "entries",
    }
    query = query_names[int(result["query"])]
    rows = list(result.get("rows", []))
    rendered = {
        "ok": bool(result["ok"]),
        "scope": result["scope"],
        "projection": {
            "name": result["projection_name"],
            "schema": result["projection_schema"],
            "authority": result["authority"],
            "rebuildable": bool(result["rebuildable"]),
        },
        "query": query,
        "limit": int(result["limit"]),
        "rows": rows,
        "row_count": len(rows),
        "source_id": result.get("source_id"),
        "kind": result.get("entry_kind"),
        "range": {
            key: value for key, value in dict(result.get("range", {})).items() if value
        },
    }
    errors = [
        {key: value for key, value in dict(error).items() if value is not None}
        for error in result.get("errors", [])
    ]
    if errors:
        rendered["errors"] = errors
    return rendered


def query_projection(
    runtime_dir: str | Path,
    *,
    query: str = "entries",
    source_id: str | None = None,
    episode_id: int | None = None,
    kind: str | None = None,
    range_filter: dict[str, Any] | None = None,
    limit: int = 100,
) -> dict[str, Any]:
    scope = "episode" if episode_id else ("source" if source_id else "all")
    if query in {"sources", "manifests", "entries"}:
        range_filter = range_filter or {}
        return _typed_query_edge_projection(
            dict(
                _runtime().storage_query_typed(
                    str(runtime_dir),
                    query,
                    source_id=source_id,
                    entry_kind=kind,
                    limit=limit,
                    since=str(range_filter.get("since") or ""),
                    until=str(range_filter.get("until") or ""),
                )
            )
        )
    return dict(
        _runtime().run_storage_service_operation(
            "query",
            str(runtime_dir),
            {
                "scope": scope,
                "source_id": source_id,
                "episode_id": _u64(episode_id),
                "query": query,
                "kind": kind,
                "range": range_filter or {},
                "limit": limit,
            },
        )
    )


def assessment_contract(runtime_dir: str | Path = "") -> dict[str, Any]:
    """Return the C++-owned KF-ADR-019f86da-4f90-7b3f-9ef3-84f5a878f302 assessment contract."""

    return dict(
        _runtime().run_storage_service_operation(
            "assessment_contract", str(runtime_dir), {}
        )
    )


def assessment_request(
    runtime_dir: str | Path,
    request: dict[str, Any],
    *,
    system_time: int = 0,
) -> dict[str, Any]:
    """Persist an assessment intent without blocking the work Episode seal."""

    return dict(
        _runtime().run_storage_service_operation(
            "assessment_request",
            str(runtime_dir),
            {"request": request, "system_time": system_time},
        )
    )


def assessment_execute(
    runtime_dir: str | Path,
    assessment_key: str,
    *,
    executor_profile: str = "process",
    system_time: int = 0,
) -> dict[str, Any]:
    """Execute or deduplicate a durable assessment job."""

    return dict(
        _runtime().run_storage_service_operation(
            "assessment_execute",
            str(runtime_dir),
            {
                "assessment_key": assessment_key,
                "executor_profile": executor_profile,
                "system_time": system_time,
            },
        )
    )


def assessment_status(runtime_dir: str | Path, assessment_key: str) -> dict[str, Any]:
    """Fold the durable lifecycle of one assessment key."""

    return dict(
        _runtime().run_storage_service_operation(
            "assessment_status",
            str(runtime_dir),
            {"assessment_key": assessment_key},
        )
    )


def assessment_list(runtime_dir: str | Path) -> dict[str, Any]:
    """List durable assessment lifecycle folds for workspace scheduling."""

    return dict(
        _runtime().run_storage_service_operation(
            "assessment_list", str(runtime_dir), {}
        )
    )


def assessment_invalidate(
    runtime_dir: str | Path,
    assessment_key: str,
    *,
    changed_root: str,
    reason: str = "",
    system_time: int = 0,
) -> dict[str, Any]:
    """Mark a report stale only when the changed root is one of its inputs."""

    return dict(
        _runtime().run_storage_service_operation(
            "assessment_invalidate",
            str(runtime_dir),
            {
                "assessment_key": assessment_key,
                "changed_root": changed_root,
                "reason": reason,
                "system_time": system_time,
            },
        )
    )


def trust_require(
    runtime_dir: str | Path, assessment_key: str, *, purpose: str
) -> dict[str, Any]:
    """Fail closed unless a fresh report is bound to the requested purpose."""

    return dict(
        _runtime().run_storage_service_operation(
            "trust_require",
            str(runtime_dir),
            {"assessment_key": assessment_key, "purpose": purpose},
        )
    )


def trust_await(
    runtime_dir: str | Path,
    assessment_key: str,
    *,
    purpose: str,
    timeout_seconds: float,
    poll_interval_seconds: float = 0.05,
) -> dict[str, Any]:
    """Wait a bounded time, then fail closed without changing Episode seal state."""

    deadline = time.monotonic() + max(timeout_seconds, 0.0)
    while True:
        result = trust_require(runtime_dir, assessment_key, purpose=purpose)
        if result["allowed"] or result["reason"] not in {
            "assessment-not-found",
            "assessment-not-fresh",
        }:
            return result
        status = assessment_status(runtime_dir, assessment_key)
        if status.get("state") not in {None, "pending", "running"}:
            return result
        if time.monotonic() >= deadline:
            return {
                "schema": "kungfu.trust.assessment/v1",
                "allowed": False,
                "reason": "trust-timeout",
                "assessment_key": assessment_key,
                "purpose": purpose,
                "state": status.get("state", "missing"),
            }
        time.sleep(max(poll_interval_seconds, 0.001))


write_jsonl = StorageTransfer.write_jsonl


def export_records(
    runtime_dir: str | Path,
    *,
    source_id: str,
    range_filter: dict[str, Any] | None = None,
) -> list[dict[str, Any]]:
    return [
        dict(row)
        for row in _runtime().export_storage_records(
            str(runtime_dir), source_id, range_filter or {}
        )
    ]


export_jsonl = StorageTransfer.export_jsonl
export_bundle_json = StorageTransfer.export_bundle_json
build_export_bundle = StorageTransfer.build_export_bundle
import_bundle = StorageTransfer.import_bundle


def episode_admission(
    destination_runtime_dir: str | Path,
    *,
    action: str = "plan",
    source_runtime_dir: str | Path | None = None,
    episode_ids: list[int] | None = None,
    transport: str = "local-direct",
    initiator: str = "destination-pull",
    plan: dict[str, Any] | None = None,
    plan_root: str = "",
    episode_bundles: list[dict[str, Any]] | None = None,
    source_identity: dict[str, Any] | None = None,
    destination_identity: dict[str, Any] | None = None,
    project_cut_roots: list[str] | None = None,
) -> dict[str, Any]:
    """Run the destination-owned Episode Admission protocol in libkungfu."""

    options: dict[str, Any] = {
        "action": action,
        "transport": transport,
        "initiator": initiator,
        "episode_ids": [_u64(value) for value in (episode_ids or [])],
        "project_cut_roots": project_cut_roots or [],
    }
    if source_runtime_dir is not None:
        options["source_runtime_dir"] = str(source_runtime_dir)
    if plan is not None:
        options["plan"] = _binding_json(plan)
    if plan_root:
        options["plan_root"] = plan_root
    if episode_bundles is not None:
        options["episode_bundles"] = _binding_json(episode_bundles)
    if source_identity is not None:
        options["source_identity"] = _binding_json(source_identity)
    if destination_identity is not None:
        options["destination_identity"] = _binding_json(destination_identity)
    return dict(
        _runtime().run_storage_service_operation(
            "episode_admission", str(destination_runtime_dir), options
        )
    )


def write_synthetic_source(
    runtime_dir: str | Path,
    *,
    source_id: str,
    records: list[dict[str, Any]],
    manifest_id: str = "synthetic-import",
    source_head: str = "synthetic-head",
    range_filter: dict[str, Any] | None = None,
) -> dict[str, Any]:
    """Write a qualification-only synthetic adapter fixture.

    The canonical payload protocol and exact manifest projection are pinned by
    ``tests/fixtures/storage-synthetic-source/vectors.json``. Production source
    adapters must provide their own manifest rather than call this helper.
    """

    entries = []
    for index, record in enumerate(records):
        state = str(record.get("payload_state") or PAYLOAD_STATE_PRESENT)
        if state not in PAYLOAD_STATES:
            raise ValueError(f"unsupported synthetic payload state: {state}")
        if state == PAYLOAD_STATE_PRESENT:
            payload = record.get("payload", record)
            raw = canonical_json_bytes(payload)
            digest = payload_hash(raw)
            write_payload_bytes(runtime_dir, digest, raw)
            byte_len = len(raw)
        else:
            # Honest non-present states never serialize a body. A redacted
            # entry may carry the hash/length computed before withholding;
            # an absent entry carries neither; a recorded-missing entry keeps
            # whatever identity is known for the lost body.
            digest = str(record.get("payload_hash") or "")
            byte_len = int(record.get("byte_len") or 0)
        entries.append(
            {
                "kind": str(record.get("kind") or "record"),
                "source_id": str(record.get("source_id") or f"record-{index}"),
                "source_path": str(
                    record.get("source_path") or f"synthetic/{index}.json"
                ),
                "source_time": str(record.get("source_time") or ""),
                "schema_version": int(record.get("schema_version") or 1),
                "content_type": CONTENT_TYPE_JSON,
                "payload_hash": digest,
                "byte_len": byte_len,
                "payload_state": state,
            }
        )
    return accept_manifest(
        runtime_dir,
        {
            "manifest_id": manifest_id,
            "storage_source_id": source_id,
            "source_type": "synthetic",
            "source_coordinate": f"synthetic:{source_id}",
            "source_head": source_head,
            "scope": "source",
            "range": range_filter or {},
            "entries": entries,
        },
    )
