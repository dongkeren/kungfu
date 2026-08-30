# SPDX-License-Identifier: Apache-2.0

import pickle
from pathlib import Path

from kungfu.storage import _service_episode, _service_fact, service


EXTRACTED_EXPORTS = {
    _service_episode: (
        "episode_abort",
        "episode_attach_frame",
        "episode_attach_ref",
        "episode_begin",
        "episode_end",
        "episode_heartbeat",
        "episode_inspect",
        "episode_list",
        "episode_projection_rebuild",
        "episode_recover",
        "episode_recovery_execute",
        "episode_recovery_plan",
    ),
    _service_fact: (
        "action_runtime",
        "build_fact_query_definition",
        "compile_fact_query_sql",
        "fact_changelog",
        "fact_contract",
        "fact_declare_contract_world",
        "fact_declare_surface",
        "fact_kernel",
        "fact_kernel_backend_parity",
        "fact_kernel_export",
        "fact_kernel_fsck",
        "fact_kernel_import",
        "fact_kernel_rebuild_projections",
        "fact_kernel_retention_plan",
        "fact_library_contract",
        "fact_library_export",
        "fact_library_import",
        "fact_material_list",
        "fact_material_put",
        "fact_observe",
        "fact_profile_shadow_compare",
        "fact_profile_shadow_inspect",
        "fact_profile_shadow_project",
        "fact_query",
        "fact_query_conformance",
        "fact_query_definition",
        "fact_state",
        "fact_type_create",
        "fact_type_list",
        "kfx_registry",
        "profile_lifecycle",
        "query_plan",
        "saved_query_catalog",
    ),
}


def test_storage_service_responsibility_modules_stay_below_1000_lines() -> None:
    paths = [Path(service.__file__)] + [
        Path(owner.__file__) for owner in EXTRACTED_EXPORTS
    ]
    line_counts = {path.name: len(path.read_text().splitlines()) for path in paths}
    assert set(line_counts) == {
        "service.py",
        "_service_episode.py",
        "_service_fact.py",
    }
    assert all(line_count < 1000 for line_count in line_counts.values())


def test_storage_service_keeps_extracted_callables_on_the_public_facade() -> None:
    assert service.write_jsonl is service.StorageTransfer.write_jsonl
    for owner, names in EXTRACTED_EXPORTS.items():
        for name in names:
            value = getattr(service, name)
            assert value is getattr(owner, name)
            assert value.__module__ == service.__name__
            assert value.__name__ == name
            assert value.__qualname__ == name
            assert pickle.loads(pickle.dumps(value)) is value


def test_fact_helpers_resolve_facade_dependencies_after_monkeypatch(
    monkeypatch,
) -> None:
    saved = service.fact_query
    definition = {"schema": "test-definition"}
    sentinel = {"schema": "test-result"}
    monkeypatch.setattr(
        service,
        "build_fact_query_definition",
        lambda **_kwargs: definition,
    )
    monkeypatch.setattr(
        service,
        "fact_query_definition",
        lambda runtime_dir, value, *, engine: (
            sentinel
            if (runtime_dir, value, engine) == ("runtime", definition, "sqlite")
            else None
        ),
    )

    assert saved("runtime", engine="sqlite") is sentinel


def test_episode_helpers_resolve_facade_runtime_and_edge_after_monkeypatch(
    monkeypatch,
) -> None:
    class Runtime:
        def run_storage_service_operation(self, operation, runtime_dir, options):
            assert (operation, runtime_dir) == ("episode_begin", "runtime")
            assert options["episode_id"] == 42
            return {"status": "open"}

    sentinel = {"schema": "test-edge"}
    monkeypatch.setattr(service, "_runtime", lambda: Runtime())
    monkeypatch.setattr(service, "_episode_write_edge", lambda _value: sentinel)

    assert service.episode_begin("runtime", episode_id=42) is sentinel
