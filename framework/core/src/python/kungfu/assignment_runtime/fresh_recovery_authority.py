# SPDX-License-Identifier: Apache-2.0

"""Plan-time discovery and apply-time verification for fresh recovery."""

from __future__ import annotations

from collections.abc import Mapping
import json
from pathlib import Path
from typing import Any

from kungfu import profile_sdk, work_authority
from kungfu.agent import session_surface
from kungfu.assignment_runtime import LocalAssignmentRuntimeApplication

JsonObject = dict[str, Any]


def planned_roles(
    workspace: Mapping[str, Any],
    work_ref: Mapping[str, Any],
    recovery_profile: Mapping[str, Any],
    binding: Mapping[str, Any],
) -> tuple[JsonObject, JsonObject, JsonObject]:
    workspace_value = dict(workspace)
    workspace_root = Path(str(workspace_value.get("root") or "")).expanduser()
    workspace_value.setdefault(
        "runtimeRoot", str((workspace_root / ".kungfu" / "runtime").resolve())
    )
    profile_body = {
        "schema": work_authority.PLANNED_PROFILE_SOURCE_SCHEMA,
        "profileId": str(recovery_profile.get("profileId") or ""),
        "profileRoot": str(recovery_profile.get("profileRoot") or ""),
        "sourceContractRoot": str(recovery_profile.get("sourceContractRoot") or ""),
        "sourceLocator": str(recovery_profile.get("sourceLocator") or ""),
    }
    target_body = {
        "schema": work_authority.PLANNED_TARGET_SCHEMA,
        "workspace": workspace_value,
        "workRef": dict(work_ref),
    }
    session = dict(binding.get("session") or {})
    console = dict(binding.get("console") or {})
    console_runtime = str(
        console.get("consoleRuntimeRoot") or workspace_value["runtimeRoot"]
    )
    console_body = {
        "schema": work_authority.PLANNED_CONSOLE_BINDING_SCHEMA,
        "workConsoleId": str(session.get("workConsoleId") or ""),
        "sessionAttemptId": str(session.get("sessionAttemptId") or ""),
        "sourceWorkspaceId": str(
            console.get("sourceWorkspaceId") or workspace_value.get("id") or ""
        ),
        "consoleRuntimeRoot": console_runtime,
        "consoleEndpoint": str(
            console.get("consoleEndpoint")
            or session_surface.endpoint_for_runtime(console_runtime)
        ),
        "bindingScope": str(console.get("bindingScope") or "same-project"),
    }
    return (
        work_authority.rooted(profile_body, "sourceRoot"),
        work_authority.rooted(target_body, "targetRoot"),
        work_authority.rooted(console_body, "bindingRoot"),
    )


def verify_planned_roles(plan: Mapping[str, Any]) -> None:
    retained = dict(plan.get("retainedAssignmentAuthority") or {})
    profile = work_authority.verify_rooted(
        plan.get("plannedProfileSource") or {},
        schema=work_authority.PLANNED_PROFILE_SOURCE_SCHEMA,
        root_field="sourceRoot",
        label="fresh recovery planned Profile source",
    )
    target = work_authority.verify_rooted(
        plan.get("plannedTarget") or {},
        schema=work_authority.PLANNED_TARGET_SCHEMA,
        root_field="targetRoot",
        label="fresh recovery planned target",
    )
    console = work_authority.verify_rooted(
        plan.get("plannedConsoleBinding") or {},
        schema=work_authority.PLANNED_CONSOLE_BINDING_SCHEMA,
        root_field="bindingRoot",
        label="fresh recovery planned Console binding",
    )
    work = dict(plan.get("work") or {})
    work_ref = dict(plan.get("workRef") or {})
    same_project = console.get("bindingScope") == "same-project"
    checks = (
        retained.get("schema") == work_authority.RETAINED_ASSIGNMENT_AUTHORITY_SCHEMA,
        retained == work_authority.retained_assignment_authority(retained),
        work_authority.semantic_root(retained) == work.get("lifecycleStateRoot"),
        work_authority.semantic_root(retained.get("assignment") or {})
        == work.get("assignmentRoot"),
        profile == plan.get("recoveryProfile"),
        profile.get("profileId") == "kungfu.work-control",
        profile.get("profileRoot") == (plan.get("workRef") or {}).get("profileRoot"),
        bool(str(profile.get("sourceLocator") or "")),
        str(profile.get("sourceContractRoot") or "").startswith("sha256:"),
        target.get("workspace") == plan.get("workspace"),
        target.get("workRef") == plan.get("workRef"),
        work_ref.get("workspaceId") == (target.get("workspace") or {}).get("id"),
        console.get("workConsoleId")
        == (plan.get("attempt") or {}).get("workConsoleId"),
        console.get("sessionAttemptId")
        == (plan.get("attempt") or {}).get("newSessionAttemptId"),
        console.get("consoleEndpoint")
        == session_surface.endpoint_for_runtime(
            str(console.get("consoleRuntimeRoot") or "")
        ),
        console.get("bindingScope") in {"same-project", "explicit-external-project"},
        (
            console.get("sourceWorkspaceId") == work_ref.get("workspaceId")
            if same_project
            else console.get("sourceWorkspaceId") != work_ref.get("workspaceId")
        ),
    )
    if not all(checks):
        raise ValueError("fresh recovery planned authority roles do not agree")


def validated_recovery_profile(source: Path, runtime_dir) -> JsonObject:
    exact_source = source.expanduser().resolve()
    inspection = profile_sdk.validate_source(exact_source, runtime_dir)["inspection"]
    source_contract = dict(
        (inspection.get("closure") or {}).get("source_contract") or {}
    )
    body = {
        "schema": work_authority.PLANNED_PROFILE_SOURCE_SCHEMA,
        "profileId": str(inspection["profile"]["id"]),
        "profileRoot": str(inspection["profile_suite_root"]),
        "sourceContractRoot": str(source_contract.get("root") or ""),
        "sourceLocator": str(exact_source),
    }
    return work_authority.rooted(body, "sourceRoot")


def verify_recovery_profile_source(
    plan: Mapping[str, Any], recovery_profile_source: Path, runtime_dir
) -> None:
    planned = dict(plan.get("plannedProfileSource") or {})
    exact_source = recovery_profile_source.expanduser().resolve()
    if str(exact_source) != str(planned.get("sourceLocator") or ""):
        raise ValueError("fresh recovery Profile source locator differs from the plan")
    if validated_recovery_profile(exact_source, runtime_dir) != planned:
        raise ValueError("fresh recovery Profile source differs from the plan")


def verify_planned_workspace(plan: Mapping[str, Any]) -> tuple[Path, JsonObject]:
    workspace = dict((plan.get("plannedTarget") or {}).get("workspace") or {})
    workspace_root = Path(str(workspace.get("root") or "")).expanduser().resolve()
    runtime_dir = Path(str(workspace.get("runtimeRoot") or "")).expanduser().resolve()
    if runtime_dir != workspace_root / ".kungfu" / "runtime":
        raise ValueError("fresh recovery planned workspace runtime changed")
    identity_path = runtime_dir.parent / "workspace-identity.json"
    try:
        material = json.loads(identity_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(
            "fresh recovery planned workspace identity is unavailable"
        ) from error
    body = {key: value for key, value in material.items() if key != "identityRoot"}
    identity_root = str(material.get("identityRoot") or "")
    workspace_id = f"project:{identity_root.removeprefix('sha256:')[:16]}"
    checks = (
        material.get("schema") == "kungfu.workspace.identity-material/v1",
        material.get("workspaceKind") == "project",
        identity_root == work_authority.semantic_root(body),
        identity_root == workspace.get("identityRoot"),
        workspace_id == workspace.get("id"),
    )
    if not all(checks):
        raise ValueError("fresh recovery planned workspace identity changed")
    observation = {
        "workspaceId": workspace_id,
        "identityRoot": identity_root,
        "runtimeRoot": str(runtime_dir),
        "available": runtime_dir.is_dir(),
    }
    if not observation["available"]:
        raise ValueError("fresh recovery planned workspace runtime is unavailable")
    return runtime_dir, observation


def observe_planned_console(plan: Mapping[str, Any]) -> tuple[JsonObject, JsonObject]:
    console = dict(plan.get("plannedConsoleBinding") or {})
    session = {
        "workConsoleId": str(console.get("workConsoleId") or ""),
        "sessionAttemptId": str(console.get("sessionAttemptId") or ""),
    }
    current = session_surface.invoke(
        {"operation": "show", "client": "kfd3-agent", "session": session},
        endpoint=str(console.get("consoleEndpoint") or ""),
    )
    current_console = dict(current.get("console") or {})
    current_attempt = dict(current.get("attempt") or {})
    observed_console = str(
        current.get("workConsoleId") or current_console.get("consoleId") or ""
    )
    observed_attempt = str(
        current.get("sessionAttemptId") or current_attempt.get("sessionAttemptId") or ""
    )
    lifecycle = str(current.get("lifecycleState") or "")
    if (
        observed_console != session["workConsoleId"]
        or observed_attempt != session["sessionAttemptId"]
        or lifecycle in {"ended", "unavailable", "unrecoverable", "orphaned"}
        or current.get("live") is False
    ):
        raise ValueError("fresh recovery planned Console or SessionAttempt is not live")
    return session, {
        "workConsoleId": observed_console,
        "sessionAttemptId": observed_attempt,
        "lifecycleState": lifecycle,
        "live": current.get("live", True),
        "generation": current.get("generation"),
        "revision": current.get("revision"),
    }


def status_from_planned_source(
    runtime_dir: Path,
    source: Path,
    initiative_id: str,
    assignment_id: str,
) -> JsonObject:
    return LocalAssignmentRuntimeApplication(
        runtime_dir,
        client_id="kungfu.work.fresh-recovery",
        kind="cli",
        source=source,
    ).status(initiative_id, assignment_id)


def current_binding_context(runtime_dir: str, work_workspace_id: str) -> JsonObject:
    """Discover the Console once while creating the rooted recovery plan."""

    current = session_surface.current_native_console(
        runtime_dir, adopt=True, project_work_binding=False
    )
    if current is None:
        raise ValueError("fresh recovery requires a current native Agent Console")
    if str(current["source"]) not in {
        "injected-native-console",
        "ambient-provider-session",
    }:
        raise ValueError("fresh recovery requires an exact native Console source")
    envelope = dict(current["envelope"])
    source_workspace_id = str(envelope.get("workspaceId") or "")
    return {
        "session": {
            "workConsoleId": str(envelope["consoleId"]),
            "sessionAttemptId": str(envelope["attemptId"]),
        },
        "console": {
            "sourceWorkspaceId": source_workspace_id,
            "consoleRuntimeRoot": str(Path(runtime_dir).expanduser().resolve()),
            "consoleEndpoint": session_surface.endpoint_for_runtime(runtime_dir),
            "bindingScope": (
                "same-project"
                if source_workspace_id == work_workspace_id
                else "explicit-external-project"
            ),
        },
    }
