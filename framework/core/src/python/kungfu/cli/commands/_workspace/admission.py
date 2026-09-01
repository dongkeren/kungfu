# SPDX-License-Identifier: Apache-2.0

import json
from functools import wraps

import click


def admission_command(initiator: str):
    def decorate(function):
        @wraps(function)
        def wrapped(
            source_runtime,
            destination_runtime,
            episode_ids,
            transport,
            action,
            plan_root,
            project_cut_root,
            bundle_files,
            source_id,
            as_json,
        ):
            from kungfu.storage import service

            if (
                action in {"plan", "execute", "resume"}
                and transport == "local-direct"
                and (not source_runtime or not episode_ids)
            ):
                raise click.UsageError(
                    "--source-runtime and at least one --episode-id are required"
                )
            if (
                action in {"plan", "execute", "resume"}
                and transport != "local-direct"
                and (not bundle_files or not source_id)
            ):
                raise click.UsageError(
                    "--bundle-file and --source-id are required for bundle and remote-stream"
                )
            if action in {"inspect", "resume", "reconcile", "cancel"} and not plan_root:
                raise click.UsageError(f"--plan-root is required for {action}")
            episode_bundles = []
            for bundle_file in bundle_files:
                with open(bundle_file, encoding="utf-8") as input_file:
                    episode_bundles.append(json.load(input_file))
            values = {
                "source_runtime_dir": source_runtime,
                "episode_ids": list(episode_ids),
                "transport": transport,
                "initiator": initiator,
                "plan_root": plan_root,
                "project_cut_roots": list(project_cut_root),
                "episode_bundles": episode_bundles or None,
                "source_identity": (
                    {
                        "schema": "kungfu.workspace.identity/v1",
                        "kind": "declared",
                        "id": source_id,
                    }
                    if source_id
                    else None
                ),
            }
            if action == "execute":
                plan = service.episode_admission(
                    destination_runtime, action="plan", **values
                )
                payload = service.episode_admission(
                    destination_runtime, action="execute", plan=plan, **values
                )
            else:
                payload = service.episode_admission(
                    destination_runtime, action=action, **values
                )
            if as_json:
                click.echo(json.dumps(payload, indent=2, sort_keys=True))
                return
            click.echo(
                f"{initiator} {payload.get('status', 'planned')} "
                f"{payload.get('plan_root', plan_root)}"
            )

        wrapped = click.option(
            "--json", "as_json", is_flag=True, help="machine-readable output"
        )(wrapped)
        wrapped = click.option(
            "--source-id",
            default="",
            help="declared source identity for non-local transports",
        )(wrapped)
        wrapped = click.option(
            "--bundle-file",
            "bundle_files",
            multiple=True,
            type=click.Path(exists=True, dir_okay=False),
            help="self-contained Episode bundle for bundle or remote-stream",
        )(wrapped)
        wrapped = click.option(
            "--project-cut-root", multiple=True, help="related Project Cut root"
        )(wrapped)
        wrapped = click.option(
            "--plan-root", default="", help="existing plan root for lifecycle actions"
        )(wrapped)
        wrapped = click.option(
            "--action",
            type=click.Choice(
                ["plan", "execute", "inspect", "resume", "reconcile", "cancel"]
            ),
            default="plan",
            show_default=True,
        )(wrapped)
        wrapped = click.option(
            "--transport",
            type=click.Choice(["local-direct", "bundle", "remote-stream"]),
            default="local-direct",
            show_default=True,
        )(wrapped)
        wrapped = click.option("--episode-id", "episode_ids", multiple=True, type=int)(
            wrapped
        )
        wrapped = click.option(
            "--destination-runtime",
            required=True,
            type=click.Path(file_okay=False),
            help="destination workspace runtime directory",
        )(wrapped)
        wrapped = click.option(
            "--source-runtime",
            type=click.Path(file_okay=False),
            help="source workspace runtime directory",
        )(wrapped)
        return wrapped

    return decorate
