# SPDX-License-Identifier: Apache-2.0

"""Bounded project evidence selection and retained Agent-run recovery service."""

from __future__ import annotations

import copy
from datetime import UTC, datetime
import hashlib
import json
import os
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable
import uuid

from kungfu import assignment_orchestration as orchestration
from kungfu.assignment_lifecycle.ports import AssignmentRuntimePort
from kungfu.agent import run_agent
from kungfu.content_hash import compute_content_hash_value
from kungfu.rewind import bundle

JsonObject = dict[str, Any]


@dataclass(frozen=True)
class EvidenceServices:
    runtime: AssignmentRuntimePort
    status: Callable[[str, str, str], JsonObject]
    receipt: Callable[[JsonObject], JsonObject]
    agent_report_summary: Callable[[JsonObject], JsonObject]


def content_root(path: Path) -> str:
    return f"sha256:{hashlib.sha256(path.read_bytes()).hexdigest()}"


_REVIEW_EVIDENCE_SUFFIXES = {
    ".csv",
    ".json",
    ".md",
    ".rst",
    ".toml",
    ".txt",
    ".yaml",
    ".yml",
}
_REVIEW_EVIDENCE_EXCLUDED_DIRECTORIES = {
    ".git",
    ".kungfu",
    ".venv",
    "build",
    "dist",
    "node_modules",
    "target",
}
_REVIEW_EVIDENCE_FILE_LIMIT = 24
_REVIEW_EVIDENCE_BYTES_LIMIT = 1024 * 1024


def project_review_evidence(
    workspace: str | Path,
    report_path: str | Path,
    work_definition: JsonObject,
) -> JsonObject:
    workspace = Path(workspace).resolve()
    report_path = Path(report_path).resolve()
    try:
        report_display_path = report_path.relative_to(workspace).as_posix()
    except ValueError:
        report_display_path = str(report_path)
    explicit = work_definition.get("evidence_paths") or []
    if not isinstance(explicit, list) or any(
        not isinstance(value, str) or not value.strip() for value in explicit
    ):
        raise ValueError("Assignment evidence_paths must be an array of paths")
    candidates = []
    if explicit:
        for value in explicit:
            candidate = (workspace / value).resolve()
            if workspace not in candidate.parents or not candidate.is_file():
                raise ValueError(f"Assignment evidence path is unavailable: {value}")
            candidates.append(candidate)
    else:
        for root, directories, filenames in os.walk(workspace):
            directories[:] = sorted(
                directory
                for directory in directories
                if directory not in _REVIEW_EVIDENCE_EXCLUDED_DIRECTORIES
                and not directory.startswith(".")
            )
            for filename in sorted(filenames):
                candidate = Path(root) / filename
                if candidate.suffix.lower() not in _REVIEW_EVIDENCE_SUFFIXES:
                    continue
                try:
                    size = candidate.stat().st_size
                except OSError:
                    continue
                if size <= _REVIEW_EVIDENCE_BYTES_LIMIT:
                    candidates.append(candidate.resolve())

    def priority(candidate):
        relative = candidate.relative_to(workspace)
        parts = relative.parts
        return (
            0
            if parts and parts[0] == "deliverables"
            else 1
            if relative.as_posix() == "WORK.md"
            else 2
            if relative.as_posix() == "README.md"
            else 3
            if parts and parts[0] == "inputs"
            else 4,
            relative.as_posix(),
        )

    selected: list[Path] = []
    total_bytes = 0
    for candidate in sorted(set(candidates), key=priority):
        size = candidate.stat().st_size
        if len(selected) >= _REVIEW_EVIDENCE_FILE_LIMIT:
            break
        if total_bytes + size > _REVIEW_EVIDENCE_BYTES_LIMIT:
            continue
        selected.append(candidate)
        total_bytes += size
    if selected:
        primary, *supporting = selected
        retained_execution = []
        if report_path not in selected:
            retained_execution.append(
                {
                    "path": report_display_path,
                    "root": content_root(report_path),
                    "content": report_path.read_text(encoding="utf-8"),
                }
            )
        return {
            "mode": "project-files",
            "primary": {
                "path": primary.relative_to(workspace).as_posix(),
                "root": content_root(primary),
                "content": primary.read_text(encoding="utf-8"),
            },
            "supporting": retained_execution
            + [
                {
                    "path": candidate.relative_to(workspace).as_posix(),
                    "root": content_root(candidate),
                }
                for candidate in supporting
            ],
        }
    return {
        "mode": "execution-report",
        "primary": {
            "path": report_display_path,
            "root": content_root(report_path),
            "content": report_path.read_text(encoding="utf-8"),
        },
        "supporting": [],
    }


def load_execution_agent_report(
    path: str | Path,
    runtime_dir: str | Path,
    initiative_id: str,
    assignment_id: str,
    *,
    require_success: bool = True,
) -> tuple[Path, JsonObject]:
    report_path = Path(path).expanduser().resolve()
    allowed_root = (Path(runtime_dir) / "agent-runs").resolve()
    if report_path != allowed_root and allowed_root not in report_path.parents:
        raise ValueError("Agent report must belong to this workspace runtime")
    report = json.loads(report_path.read_text(encoding="utf-8"))
    if report.get("schema") != run_agent.REPORT_SCHEMA:
        raise ValueError("Agent report schema is not supported")
    expected_root = run_agent.canonical_root(
        {key: value for key, value in report.items() if key != "reportRoot"}
    )
    if report.get("reportRoot") != expected_root:
        raise ValueError("Agent report root does not match its content")
    work_ref = (report.get("work") or {}).get("workRef") or {}
    if (
        work_ref.get("entityType") != "assignment"
        or work_ref.get("entityId") != assignment_id
    ):
        raise ValueError("Agent report is not bound to this Assignment")
    if require_success and report.get("launch", {}).get("exitCode") != 0:
        raise ValueError("Agent report does not contain a successful execution")
    return report_path, report


def finalize_session_agent_report(
    path: str | Path,
    runtime_dir: str | Path,
    initiative_id: str,
    assignment_id: str,
    *,
    workspace_root: str | Path,
    session_invoke: Callable[[dict[str, Any]], dict[str, Any]] | None = None,
) -> tuple[Path, JsonObject]:
    """Seal the final observable Session state into a new immutable report."""

    source_path, source = load_execution_agent_report(
        path,
        runtime_dir,
        initiative_id,
        assignment_id,
    )
    source_session = source.get("session") or {}
    session_ref = {
        "workConsoleId": str(source_session.get("workConsoleId") or ""),
        "sessionAttemptId": str(source_session.get("sessionAttemptId") or ""),
    }
    if not all(session_ref.values()):
        raise ValueError("Agent report has no finalizable Session reference")
    invoke = session_invoke or (
        lambda request: run_agent.session_surface.invoke_for_project(
            request,
            fallback_runtime_dir=str(runtime_dir),
            cwd=str(workspace_root),
        )
    )
    status = invoke({"operation": "status", "session": session_ref})
    if any(status.get(key) != value for key, value in session_ref.items()):
        raise ValueError("Agent Session status does not match the retained report")
    attention = (status.get("workAgent") or {}).get("attention") or {}
    if status.get("live") is True or attention.get("kind") != "ready-for-review":
        raise ValueError(
            "Agent Session must end at ready-for-review before finalization"
        )
    snapshot = invoke(
        {
            "operation": "snapshot",
            "session": session_ref,
            "requestedSequence": 0,
        }
    )
    terminal = snapshot.get("terminal") or {}
    vt = terminal.get("vt") or {}
    lines = [str(line) for line in vt.get("lines") or []]
    observation_text = "\n".join(lines).strip()
    if not observation_text:
        raise ValueError("Agent Session final snapshot contains no observable output")

    finalization_id = f"{source['runId']}-session-final-{uuid.uuid4().hex}"
    bundle_dir = Path(runtime_dir) / "agent-runs" / finalization_id / "bundle"
    bundle_dir.mkdir(parents=True, exist_ok=False)
    report_path = bundle_dir / "report.json"
    manifest_path = bundle_dir / "manifest.json"
    report_body = copy.deepcopy(
        {key: value for key, value in source.items() if key != "reportRoot"}
    )
    report_body["providerObservation"] = {
        **(source.get("providerObservation") or {}),
        "text": observation_text,
    }
    report_body["session"] = status
    report_body["sessionFinalization"] = {
        "schema": "kungfu.agent-session.final-evidence/v1",
        "sourceReportPath": str(source_path),
        "sourceReportRoot": source["reportRoot"],
        "session": session_ref,
        "statusRoot": run_agent.canonical_root(status),
        "snapshotRoot": run_agent.canonical_root(snapshot),
        "observedAt": datetime.now(UTC).isoformat().replace("+00:00", "Z"),
    }
    report_body["episode"] = {
        **source["episode"],
        "manifestPath": str(manifest_path),
        "reportPath": str(report_path),
    }
    report = {**report_body, "reportRoot": run_agent.canonical_root(report_body)}
    report_path.write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    bundle.emit(
        str(bundle_dir),
        str(runtime_dir),
        {
            "mode": "LIVE",
            "role": "SYSTEM",
            "namespace": "agent-run-finalization",
            "name": finalization_id,
            "dest": 0,
        },
        extra={
            "agent_run": {
                "schema": run_agent.REPORT_SCHEMA,
                "report": "report.json",
                "reportSha256": compute_content_hash_value(report_path.read_bytes()),
                "profileRoot": report["runtimeProfile"]["root"],
                "workRefRoot": run_agent.canonical_root(report["work"]["workRef"]),
                "completionAuthority": False,
                "derivedFromReportRoot": source["reportRoot"],
            }
        },
    )
    return report_path, report


def latest_starter_agent_report(
    runtime_dir: str | Path, initiative_id: str, assignment_id: str
) -> JsonObject | None:
    reports = sorted(
        (Path(runtime_dir) / "agent-runs").glob("*/bundle/report.json"),
        key=lambda path: path.stat().st_mtime_ns,
        reverse=True,
    )
    for report_path in reports:
        try:
            _, report = load_execution_agent_report(
                report_path,
                runtime_dir,
                initiative_id,
                assignment_id,
                require_success=False,
            )
        except (OSError, ValueError, json.JSONDecodeError):
            continue
        work_ref = (report.get("work") or {}).get("workRef") or {}
        if work_ref.get("purpose") in {
            "complete-starter-deliverable",
            "complete-project-assignment",
        }:
            return report
    return None


def resume_starter_work(
    *,
    workspace_root: str | None,
    home: bool,
    initiative_id: str,
    assignment_id: str,
    services: EvidenceServices,
) -> JsonObject:
    runtime = services.runtime(workspace_root, home, "read-only")
    identity, runtime_dir = runtime.identity, runtime.runtime_dir
    status_value = services.status(runtime_dir, initiative_id, assignment_id)
    report = latest_starter_agent_report(runtime_dir, initiative_id, assignment_id)
    if report is None:
        return {
            "schema": "kungfu.work-start.resume/v1",
            "status": "no-retained-agent-run",
            "workReceipt": None,
            "writeOccurred": False,
        }
    assignment_value = status_value["assignment"]
    request_root = assignment_value["request_root"]
    request_digest = request_root.removeprefix("sha256:")
    request_path = (
        Path(identity.data_home)
        / "inbox"
        / "assignment-requests"
        / "sha256"
        / request_digest[:2]
        / request_digest
        / "request.json"
    )
    runtime_profile = report["runtimeProfile"]
    work = {
        "requestPath": str(request_path),
        "requestRoot": request_root,
        "initiativeId": initiative_id,
        "assignmentId": assignment_id,
        "title": assignment_value["title"],
        "objective": assignment_value["objective"],
        "acceptanceChecks": list(
            assignment_value["work_definition"].get("acceptance_criteria") or []
        ),
    }
    plan_root = orchestration.semantic_root(
        {
            "schema": "kungfu.work-start.resume-plan/v1",
            "queryProofRoot": status_value["query_proof_root"],
            "reportRoot": report["reportRoot"],
        }
    )
    exit_code = int(report["launch"]["exitCode"])
    receipt = services.receipt(
        {
            "schema": "kungfu.work-start.receipt/v1",
            "ok": exit_code == 0,
            "status": "agent-finished" if exit_code == 0 else "agent-failed",
            "planRoot": plan_root,
            "workPhase": status_value["phase"],
            "workspace": identity.as_dict(),
            "workRef": report["work"]["workRef"],
            "work": work,
            "agent": {
                "id": runtime_profile["id"],
                "label": runtime_profile["id"],
                "provider": runtime_profile["provider"],
                "profileRoot": runtime_profile["root"],
                "selection": runtime_profile["selection"],
                "verification": {
                    "ok": runtime_profile["verified"],
                    "available": runtime_profile["verified"],
                    "version": runtime_profile["version"],
                    "error": None,
                },
            },
            "agentReport": services.agent_report_summary(report),
            "nextActions": (
                ["run-independent-review"]
                if exit_code == 0
                else ["inspect-retained-agent-report", "retry-agent-run"]
            ),
            "nonClaims": [
                "Restoring this receipt does not rerun the Agent.",
                "Agent exit does not settle Work.",
            ],
            "writeOccurred": True,
        }
    )
    return {
        "schema": "kungfu.work-start.resume/v1",
        "status": "retained-agent-run",
        "workReceipt": receipt,
        "writeOccurred": False,
    }
