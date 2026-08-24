# SPDX-License-Identifier: Apache-2.0

"""Exact native-binding provenance for Assignment admission."""

from __future__ import annotations

import json
import os
import re
import subprocess
from collections.abc import Callable
from pathlib import Path
from typing import Any

from kungfu.initiative_family import canonical as assignment_canonical

PRODUCT_MANIFEST_SCHEMA = "kungfu.product-upgrade.manifest/v1"
GIT_REVISION = re.compile(r"^[0-9a-f]{40}$")

SourceResolver = Callable[..., Path]
DescendantCheck = Callable[[Path, Path], bool]
EntrypointResolver = Callable[[Path], str]


def installed_runtime_entrypoint(binding_file: Path) -> str:
    """Bind a manifest entrypoint to the packaged native platform."""

    return "kungfu.exe" if binding_file.suffix.lower() == ".pyd" else "kungfu"


def inspect_binding(
    *,
    allow_foreign: bool,
    source_resolver: SourceResolver,
    descendant_check: DescendantCheck,
    entrypoint_resolver: EntrypointResolver,
) -> dict[str, Any]:
    """Classify one binding as exact source, installed product, or degraded."""

    import kungfu

    binding_file = Path(str(getattr(kungfu.__binding__, "__file__", ""))).resolve()
    checkout = source_resolver(binding_file)
    allowed_roots = [
        (checkout / "framework" / "core" / "build").resolve(),
        (checkout / "framework" / "core" / "dist").resolve(),
    ]
    compiled = binding_file.suffix.lower() in {".so", ".dylib", ".pyd"}
    build_info_path = binding_file.parent / "kungfubuildinfo.json"
    try:
        build_info = json.loads(build_info_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        build_info = {}
    build_revision = str(build_info.get("git", {}).get("revision") or "")
    source_layout = compiled and any(
        binding_file == root or root in binding_file.parents for root in allowed_roots
    )
    try:
        checkout_revision = subprocess.run(
            ["git", "-C", str(checkout), "rev-parse", "HEAD"],
            check=True,
            capture_output=True,
            text=True,
        ).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        checkout_revision = ""
    current = bool(
        source_layout
        and GIT_REVISION.fullmatch(build_revision)
        and build_revision == checkout_revision
        and build_info.get("git", {}).get("pristine") is True
    )

    install_source = os.environ.get("KUNGFU_INSTALL_SOURCE", "")
    runtime_value = os.environ.get("KUNGFU_DIR", "")
    manifest_value = os.environ.get("KUNGFU_UPGRADE_MANIFEST", "")
    runtime_root = Path(runtime_value).expanduser().resolve() if runtime_value else None
    manifest_path = (
        Path(manifest_value).expanduser().resolve() if manifest_value else None
    )
    manifest: dict[str, Any] = {}
    if manifest_path is not None:
        try:
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            pass
    manifest_revision = str(manifest.get("sourceCommit") or "")
    installed = bool(
        compiled
        and install_source in {"archive", "desktop-companion"}
        and runtime_root is not None
        and descendant_check(binding_file, runtime_root)
        and manifest.get("schema") == PRODUCT_MANIFEST_SCHEMA
        and GIT_REVISION.fullmatch(manifest_revision)
        and manifest_revision == build_revision
        and str(manifest.get("runtimeEntrypoint") or "")
        == entrypoint_resolver(binding_file)
        and str(manifest.get("runtimeArtifactDigest") or "").startswith(
            assignment_canonical.ROOT
        )
    )
    override = (
        allow_foreign
        or os.environ.get("KUNGFU_ASSIGNMENT_ADMIT_ALLOW_FOREIGN_BINDING") == "1"
    )
    result = {
        "schema": "kungfu.assignment-orchestration.binding-provenance/v1",
        "ok": bool(current or installed or override),
        "state": (
            "current-checkout"
            if current
            else "installed-product"
            if installed
            else "degraded"
        ),
        "binding_file": str(binding_file),
        "checkout": str(checkout) if current else None,
        "compiled": compiled,
        "install_source": install_source or None,
        "runtime_root": str(runtime_root) if installed else None,
        "manifest_path": str(manifest_path) if installed else None,
        "source_revision": build_revision or None,
        "manifest_root": (
            assignment_canonical.semantic_root(manifest) if installed else None
        ),
        "build_info_root": (
            assignment_canonical.semantic_root(build_info) if build_info else None
        ),
        "override": bool(override and not current and not installed),
        "fail_closed": not current and not installed and not override,
    }
    result["provenance_root"] = assignment_canonical.semantic_root(result)
    return result
