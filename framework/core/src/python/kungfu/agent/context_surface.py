# SPDX-License-Identifier: Apache-2.0

"""Read-only Agent context projection independent from Click registration."""

from __future__ import annotations

import json
import os
from collections.abc import Callable
from pathlib import Path
from typing import Any

from kungfu import agent as agent_pack
from kungfu import config as kungfu_config
from kungfu.agent import documentation as documentation_pack
from kungfu.agent import session_surface
from kungfu.agent.kfd3 import registry_summary
from kungfu.config import resolve_config


def _native_context() -> dict[str, Any] | None:
    raw = os.environ.get("KUNGFU_AGENT_CONTEXT", "").strip()
    if not raw:
        return None
    native = json.loads(raw)
    if (
        not isinstance(native, dict)
        or native.get("schema") != "kungfu.native-agent-context/v1"
        or native.get("environment") != "native-interactive"
    ):
        raise ValueError("invalid native Agent context envelope")
    console_raw = os.environ.get("KUNGFU_AGENT_CONSOLE_ENVELOPE", "").strip()
    if not console_raw:
        return native
    envelope = json.loads(console_raw)
    kungfu_config.validate_value("agentConsoleEnvelope", envelope)
    work_binding = dict(native.get("workBinding") or {})
    effective_work_ref = session_surface.effective_work_ref(envelope)
    work_binding["launchState"] = (
        "bound" if effective_work_ref is not None else "unbound"
    )
    work_binding["workRef"] = effective_work_ref
    native["workBinding"] = work_binding
    return native


def project_agent_context(
    ctx: Any,
    repo_root_finder: Callable[[], Path | None],
) -> dict[str, Any]:
    """Return the native envelope or a deterministic local discovery context."""

    native = _native_context()
    if native is not None:
        return native
    config = resolve_config(runtime_home=ctx.home)
    index = agent_pack.index()
    return {
        "schema": "kungfu.agent-context/v1",
        "entrypoint": "kungfu agent",
        "config": config,
        "runtime": {
            "home": ctx.home,
            "runtimeDir": ctx.runtime_dir,
        },
        "interfaces": {
            "config": "kungfu config show --json",
            "skills": "kungfu skill list --json",
            "skillCatalog": "kungfu skill catalog --json",
            "skillRegistry": "kungfu skill inspect --json",
            "kfx": "kungfu kfx list --json",
        },
        "skillRegistry": agent_pack.skill_registry(ctx.home),
        "docs": documentation_pack.discovery_context(repo_root_finder()),
        "agentPack": {
            "packRoot": str(agent_pack.pack_root()),
            "documents": index["documents"],
            "skills": index["skills"],
            "commands": agent_pack.commands(),
            "collaborationInterface": registry_summary(),
        },
    }
