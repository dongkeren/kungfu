# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import argparse
import json
import sys
from collections.abc import Mapping
from pathlib import Path
from typing import Any, Callable


def dispatch(runtime_dir: str | Path, operation: str, payload: Any) -> Any:
    from kungfu.agent import action_loop

    if operation == "authority-inspect":
        expected = payload if isinstance(payload, Mapping) else None
        return action_loop.inspect_native_authority(runtime_dir, expected)
    if isinstance(payload, Mapping):
        expected_authority = payload.get("nativeAuthority")
        envelope = payload.get("envelope")
        if expected_authority is None and isinstance(envelope, Mapping):
            expected_authority = envelope.get("nativeAuthority")
        if isinstance(expected_authority, Mapping):
            authority = action_loop.inspect_native_authority(
                runtime_dir, expected_authority
            )
            if authority.get("status") != "current":
                return authority
    operations = {
        "work-profile-bind": action_loop.bind_work_profile,
        "episode-resume-or-begin": action_loop.resume_or_begin_episode,
        "episode-inspect": action_loop.inspect_episode,
        "episode-seal": action_loop.seal_episode,
        "work-profile-atlas-refresh": action_loop.refresh_atlas,
        "completion-review": action_loop.review_completion,
        "fact-settle": action_loop.settle_fact,
        "checkpoint-save": action_loop.save_checkpoint,
        "checkpoint-load": action_loop.load_checkpoint,
        "checkpoint-resolve": action_loop.resolve_fact_ref,
    }
    handler = operations.get(operation)
    if handler is None:
        raise ValueError(f"unsupported Action Loop adapter operation: {operation}")
    return handler(runtime_dir, payload)


def main(
    dispatcher: Callable[[str | Path, str, Any], Any],
    argv: list[str] | None = None,
) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--runtime-dir", required=True)
    parser.add_argument("operation")
    args = parser.parse_args(argv)
    try:
        payload = json.load(sys.stdin)
        result = dispatcher(args.runtime_dir, args.operation, payload)
    except Exception as error:
        result = {
            "status": "denied",
            "code": "adapter-error",
            "message": str(error),
            "writeOccurred": False,
        }
    json.dump(result, sys.stdout, ensure_ascii=False, separators=(",", ":"))
    sys.stdout.write("\n")
    return 0
