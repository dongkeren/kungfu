# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import json
from pathlib import Path

import click


def authority_notice(package):
    grant = package.get("authority") or {}
    roles = package.get("productRoles") or []
    return [
        "[kfx] authority: "
        f"supply-chain={package.get('supplyChainGrade', 'unverified')}, "
        f"admission={package.get('admissionGrade', 'unverified')}, "
        f"runtime={package.get('runtimeTier', 'isolated')}",
        "[kfx]   grants: "
        + (", ".join(package.get("grantedCapabilities") or []) or "none"),
        "[kfx]   capabilityGrantRoot: "
        + str(grant.get("capabilityGrantRoot") or "none"),
        "[kfx]   product roles are assembly metadata only: "
        + (", ".join(roles) or "none"),
    ]


def native_roots(values):
    roots = []
    for value in values:
        kind, separator, path = value.partition("=")
        if not separator or kind not in {"product", "user", "workspace"} or not path:
            raise click.BadParameter(
                "roots use product=PATH, user=PATH, or workspace=PATH",
                param_hint="--root",
            )
        roots.append({"kind": kind, "path": str(Path(path).expanduser().resolve())})
    return roots


def native_json_file(path, label):
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise click.BadParameter(f"cannot read {label}: {error}") from error
    if not isinstance(value, dict):
        raise click.BadParameter(f"{label} must contain one JSON object")
    return value


def profile_member_roots(values):
    roots = {}
    for value in values:
        key, separator, root = value.partition("=")
        if not separator or not key or not root:
            raise click.BadParameter(
                "member roots use KEY=sha256:...", param_hint="--member-root"
            )
        if key in roots:
            raise click.BadParameter(
                f"duplicate member root: {key}", param_hint="--member-root"
            )
        roots[key] = root
    return roots
