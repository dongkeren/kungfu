# SPDX-License-Identifier: Apache-2.0
"""Fresh recovery authority and drift-protection cases."""
# ruff: noqa: F401,F403

from _work_authority_surface_support import *


def _fresh_recovery_fixture():
    def root(digit):
        return f"sha256:{digit * 64}"

    status = {
        "schema": "kungfu.assignment-orchestration.status/v1",
        "phase": "completion-claimed",
        "query_proof_root": root("5"),
        "completion_claim_count": 1,
        "completion_claims": [{"claim_id": "claim:one", "root": root("6")}],
        "independent_review_count": 0,
        "independent_reviews": [],
        "continuation_decision_count": 0,
        "continuation_decisions": [],
        "next_actions": [{"action": "review"}],
        "assignment": {
            "initiative_id": "initiative:test",
            "assignment_id": "assignment:test",
            "request_root": root("1"),
            "work_definition_root": root("2"),
            "evidence_episode_roots": [root("7")],
        },
    }
    work_ref = {
        "schema": "kungfu.work-ref/v1",
        "workspaceId": "project:test",
        "profileId": "kungfu.work-control",
        "profileRoot": root("3"),
        "entityType": "assignment",
        "entityId": "assignment:test",
        "entityRoot": assignment_fresh_recovery._root(status["assignment"]),
        "purpose": "continue-project-assignment",
        "systemTimeCut": root("5"),
        "initiativeId": "initiative:test",
    }
    binding = {
        "workRef": work_ref,
        "session": {
            "workConsoleId": "assistant:project:test",
            "sessionAttemptId": "native:new",
        },
    }
    plan = assignment_fresh_recovery.build_plan(
        workspace={
            "id": "project:test",
            "root": "/project",
            "identityRoot": root("4"),
        },
        status=status,
        binding=binding,
        previous_attempt_id="native:old",
        expected_request_root=root("1"),
        expected_work_definition_root=root("2"),
        expected_profile_root=root("3"),
        recovery_profile={
            "profileId": "kungfu.work-control",
            "profileRoot": root("3"),
            "sourceContractRoot": root("6"),
        },
        profile_active=False,
        now="2026-08-25T09:00:00Z",
    )
    return status, binding, plan


def test_fresh_recovery_plan_is_resume_new_attempt_without_lifecycle_replay():
    status, binding, plan = _fresh_recovery_fixture()

    assert plan["continuationMode"] == "resume/new-attempt"
    assert plan["attempt"] == {
        "previousSessionAttemptId": "native:old",
        "newSessionAttemptId": "native:new",
        "workConsoleId": "assistant:project:test",
    }
    assert plan["workRef"] == binding["workRef"]
    assert plan["recoveryProfile"] == {
        "profileId": "kungfu.work-control",
        "profileRoot": binding["workRef"]["profileRoot"],
        "sourceContractRoot": f"sha256:{'6' * 64}",
    }
    assert [effect["stage"] for effect in plan["effects"]] == [
        "activate-profile",
        "bind-new-attempt",
    ]
    assert set(plan["forbiddenEffects"]) == {"admit", "claim", "kickoff"}
    assert plan["work"]["phase"] == status["phase"]
    assert plan["writeOccurred"] is False


def test_fresh_recovery_records_exact_non_authoritative_continuation(
    tmp_path,
):
    status, binding, _plan = _fresh_recovery_fixture()
    status["phase"] = "executing"
    status["active_lease"] = None
    status["next_actions"] = [{"action": "fresh-recovery-plan"}]
    plan = assignment_fresh_recovery.build_plan(
        workspace={
            "id": "project:test",
            "root": "/project",
            "identityRoot": f"sha256:{'4' * 64}",
        },
        status=status,
        binding=binding,
        previous_attempt_id="native:old",
        expected_request_root=f"sha256:{'1' * 64}",
        expected_work_definition_root=f"sha256:{'2' * 64}",
        expected_profile_root=f"sha256:{'3' * 64}",
        recovery_profile={
            "profileId": "kungfu.work-control",
            "profileRoot": f"sha256:{'3' * 64}",
            "sourceContractRoot": f"sha256:{'6' * 64}",
        },
        profile_active=True,
        now="2026-08-25T09:00:00Z",
    )

    assert [effect["stage"] for effect in plan["effects"]] == [
        "bind-new-attempt",
        "record-recovery-continuation",
    ]
    assert set(plan["forbiddenEffects"]) == {"admit", "claim", "kickoff"}
    receipt_body = {
        "schema": assignment_fresh_recovery.RECEIPT_SCHEMA,
        "ok": True,
        "status": "recovered",
        "continuationMode": assignment_fresh_recovery.CONTINUATION_MODE,
        "planRoot": plan["planRoot"],
        "authorizedBy": "maintainer:test",
        "workRef": plan["workRef"],
        "attempt": plan["attempt"],
        "profile": {},
        "binding": {"receipt": {"receiptRoot": f"sha256:{'8' * 64}"}},
        "preservation": {
            "assignmentRoot": plan["work"]["assignmentRoot"],
            "lifecycleStateRoot": plan["work"]["lifecycleStateRoot"],
            "phase": "executing",
            "queryProofRoot": plan["work"]["systemTimeCut"],
        },
        "writeOccurred": True,
        "assignmentWrites": [],
        "nextActions": [],
    }
    receipt = {
        **receipt_body,
        "receiptRoot": assignment_fresh_recovery._root(receipt_body),
    }
    continuation = assignment_fresh_recovery.register_continuation(
        tmp_path, plan, receipt
    )
    assert continuation is not None
    assert continuation["writeAuthority"] == "none"
    assert continuation["assignmentWrites"] == []
    assert continuation["allowedNextActions"] == ["claim-completion"]
    projected = json.loads(json.dumps(status))
    projected["work_semantics"] = {
        "schema": "kungfu.work-semantics.status/v1",
        "phase": "completion-claimed",
        "next_actions": [{"action": "record-input-snapshot"}],
    }
    assert (
        recovery_continuation.resolve(
            tmp_path, "initiative:test", "assignment:test", projected
        )
        == continuation
    )

    drifted = json.loads(json.dumps(projected))
    drifted["completion_claim_count"] = 2
    with __import__("pytest").raises(ValueError, match="does not match retained Work"):
        recovery_continuation.resolve(
            tmp_path, "initiative:test", "assignment:test", drifted
        )


def test_status_verifies_recovery_continuation_before_adding_display_identity(
    tmp_path,
    monkeypatch,
):
    status, binding, _plan = _fresh_recovery_fixture()
    status["phase"] = "executing"
    status["active_lease"] = None
    status["next_actions"] = [{"action": "fresh-recovery-plan"}]
    plan = assignment_fresh_recovery.build_plan(
        workspace={
            "id": "project:test",
            "root": "/project",
            "identityRoot": f"sha256:{'4' * 64}",
        },
        status=status,
        binding=binding,
        previous_attempt_id="native:old",
        expected_request_root=f"sha256:{'1' * 64}",
        expected_work_definition_root=f"sha256:{'2' * 64}",
        expected_profile_root=f"sha256:{'3' * 64}",
        recovery_profile={
            "profileId": "kungfu.work-control",
            "profileRoot": f"sha256:{'3' * 64}",
            "sourceContractRoot": f"sha256:{'6' * 64}",
        },
        profile_active=True,
        now="2026-08-25T09:00:00Z",
    )
    receipt_body = {
        "schema": assignment_fresh_recovery.RECEIPT_SCHEMA,
        "ok": True,
        "status": "recovered",
        "continuationMode": assignment_fresh_recovery.CONTINUATION_MODE,
        "planRoot": plan["planRoot"],
        "authorizedBy": "maintainer:test",
        "workRef": plan["workRef"],
        "attempt": plan["attempt"],
        "profile": {},
        "binding": {"receipt": {"receiptRoot": f"sha256:{'8' * 64}"}},
        "preservation": {
            "assignmentRoot": plan["work"]["assignmentRoot"],
            "lifecycleStateRoot": plan["work"]["lifecycleStateRoot"],
            "phase": "executing",
            "queryProofRoot": plan["work"]["systemTimeCut"],
        },
        "writeOccurred": True,
        "assignmentWrites": [],
        "nextActions": [],
    }
    receipt = {
        **receipt_body,
        "receiptRoot": assignment_fresh_recovery._root(receipt_body),
    }
    monkeypatch.setattr(
        assignment_command,
        "_profile_read",
        lambda *_args, **_kwargs: json.loads(json.dumps(status)),
    )
    unavailable = assignment_command._status(
        tmp_path, "initiative:test", "assignment:test"
    )
    assert unavailable["next_actions"][0]["action"] == "fresh-recovery-plan"

    continuation = assignment_fresh_recovery.register_continuation(
        tmp_path, plan, receipt
    )
    recovered = assignment_command._status(
        tmp_path, "initiative:test", "assignment:test"
    )
    assert recovered["next_actions"][0]["action"] == "claim-completion"
    assert recovered["recovery_continuation"] == {
        "continuationRoot": continuation["continuationRoot"],
        "newSessionAttemptId": "native:new",
        "writeAuthority": "none",
        "allowedNextActions": ["claim-completion"],
    }


def test_fresh_recovery_separates_retained_authority_from_target_profile(
    tmp_path, monkeypatch
):
    retained = tmp_path / "retained"
    retained.mkdir()
    (retained / "profile.json").write_text("{}", encoding="utf-8")
    target = tmp_path / "target"
    target.mkdir()
    retained_root = f"sha256:{'a' * 64}"
    target_root = f"sha256:{'b' * 64}"
    source_contract_root = f"sha256:{'c' * 64}"

    def lifecycle(_runtime, operation, **_values):
        assert operation == "get"
        return {
            "profile_suite_root": retained_root,
            "latest_event": {
                "closure": {"profile_path": str(retained / "profile.json")}
            },
        }

    def validate(source, _runtime):
        resolved = source.resolve()
        if resolved == retained.resolve():
            return {"inspection": {"profile_suite_root": retained_root}}
        assert resolved == target.resolve()
        return {
            "inspection": {
                "profile": {"id": "kungfu.work-control"},
                "profile_suite_root": target_root,
                "closure": {"source_contract": {"root": source_contract_root}},
            }
        }

    monkeypatch.setattr(
        assignment_fresh_recovery.storage_service, "profile_lifecycle", lifecycle
    )
    monkeypatch.setattr(
        assignment_fresh_recovery.profile_sdk, "validate_source", validate
    )

    assert assignment_fresh_recovery._retained_profile_source(tmp_path) == retained
    assert assignment_fresh_recovery._validated_recovery_profile(target, tmp_path) == {
        "profileId": "kungfu.work-control",
        "profileRoot": target_root,
        "sourceContractRoot": source_contract_root,
    }


def test_resume_prepare_reconciles_the_explicit_recovery_source(tmp_path, monkeypatch):
    source = tmp_path / "historical-work-control"
    source.mkdir()
    desired_root = f"sha256:{'d' * 64}"
    previous_root = f"sha256:{'e' * 64}"
    reconciled = []

    monkeypatch.setattr(
        assignment_command.profile_sdk,
        "validate_source",
        lambda actual, _runtime: (
            {
                "inspection": {
                    "profile": {"id": "kungfu.work-control"},
                    "profile_suite_root": desired_root,
                }
            }
            if actual == source.resolve()
            else (_ for _ in ()).throw(AssertionError("unexpected Profile source"))
        ),
    )

    def lifecycle(_runtime, operation, **_values):
        if operation == "list":
            return {
                "profiles": [
                    {
                        "profile_id": "kungfu.work-control",
                        "profile_suite_root": previous_root,
                        "removed": False,
                    }
                ]
            }
        assert operation == "get"
        return {
            "profile_suite_root": desired_root,
            "qualified": True,
            "activated": True,
        }

    monkeypatch.setattr(
        assignment_command.storage_service, "profile_lifecycle", lifecycle
    )
    monkeypatch.setattr(
        assignment_command.profile_lifecycle,
        "ensure_work_profile",
        lambda actual, runtime, actor: (
            reconciled.append((actual, runtime, actor)) or [{"status": "activated"}]
        ),
    )

    receipt = assignment_command._prepare_resume_profile(
        tmp_path / "runtime", "maintainer:test", source
    )

    assert reconciled == [(source.resolve(), tmp_path / "runtime", "maintainer:test")]
    assert receipt["previousProfileSuiteRoot"] == previous_root
    assert receipt["profileSuiteRoot"] == desired_root


def test_fresh_recovery_prepare_does_not_require_newer_profile_work_hooks(
    tmp_path, monkeypatch
):
    source = tmp_path / "historical-work-control"
    source.mkdir()
    desired_root = f"sha256:{'d' * 64}"
    reconciled = []

    monkeypatch.setattr(
        assignment_command.profile_sdk,
        "validate_source",
        lambda actual, _runtime: {
            "inspection": {
                "profile": {"id": "kungfu.work-control"},
                "profile_suite_root": desired_root,
            }
        },
    )

    def lifecycle(_runtime, operation, **_values):
        if operation == "list":
            return {"profiles": []}
        assert operation == "get"
        return {
            "profile_suite_root": desired_root,
            "qualified": True,
            "activated": True,
        }

    monkeypatch.setattr(
        assignment_command.storage_service, "profile_lifecycle", lifecycle
    )
    monkeypatch.setattr(
        assignment_command.profile_lifecycle,
        "ensure_profile_lifecycle",
        lambda actual, runtime, actor: (
            reconciled.append((actual, runtime, actor)) or [{"status": "activated"}]
        ),
    )
    monkeypatch.setattr(
        assignment_command.profile_lifecycle,
        "ensure_work_profile",
        lambda *_args: (_ for _ in ()).throw(
            AssertionError("fresh recovery must not invoke newer Profile Work hooks")
        ),
    )

    receipt = assignment_command.profile_lifecycle.prepare_fresh_recovery_profile(
        tmp_path / "runtime", "maintainer:test", source
    )

    assert reconciled == [(source.resolve(), tmp_path / "runtime", "maintainer:test")]
    assert receipt["profileSuiteRoot"] == desired_root
    assert receipt["profileContractMutation"] == "not-permitted"


@pytest.mark.parametrize(
    ("console_source", "expects_override"),
    [
        ("ambient-provider-session", True),
        ("injected-native-console", False),
    ],
)
def test_fresh_recovery_apply_passes_exact_console_context_to_binder(
    tmp_path, monkeypatch, console_source, expects_override
):
    status, binding, plan = _fresh_recovery_fixture()
    workspace_root = tmp_path / "project"
    workspace_root.mkdir()
    profile_source = tmp_path / "retained-work-control"
    profile_source.mkdir()
    plan["workspace"]["root"] = str(workspace_root)
    plan["generatedAt"] = "2099-01-01T00:00:00Z"
    plan["expiresAt"] = "2099-01-01T00:10:00Z"
    plan["planRoot"] = assignment_fresh_recovery._root(
        {key: value for key, value in plan.items() if key != "planRoot"}
    )
    plan_file = tmp_path / "plan.json"
    plan_file.write_text(json.dumps(plan), encoding="utf-8")
    observed = {}

    monkeypatch.setattr(
        assignment_fresh_recovery,
        "_verify_recovery_profile_source",
        lambda *_args: None,
    )
    monkeypatch.setattr(
        assignment_fresh_recovery,
        "_retained_status",
        lambda *_args: json.loads(json.dumps(status)),
    )
    console_envelope = {
        "consoleId": binding["session"]["workConsoleId"],
        "attemptId": binding["session"]["sessionAttemptId"],
    }
    monkeypatch.setattr(
        assignment_fresh_recovery.session_surface,
        "current_native_console",
        lambda _runtime_dir: {
            "source": console_source,
            "envelope": console_envelope,
            "workspaceRoot": str(workspace_root),
        },
    )

    def bind_current_native_work(*_args, **kwargs):
        observed.update(kwargs)
        return {
            "workRef": dict(plan["workRef"]),
            "receipt": {"receiptRoot": f"sha256:{'8' * 64}"},
        }

    monkeypatch.setattr(
        assignment_fresh_recovery.run_agent,
        "bind_current_native_work",
        bind_current_native_work,
    )
    identity = SimpleNamespace(
        workspace_id=plan["workspace"]["id"],
        identity_root=plan["workspace"]["identityRoot"],
    )
    receipt = assignment_fresh_recovery._apply_from_ports(
        ctx=SimpleNamespace(runtime_dir=tmp_path / "console-runtime"),
        plan_file=plan_file,
        expected_plan_root=plan["planRoot"],
        authorized_by="maintainer:test",
        recovery_profile_source=profile_source,
        runtime=lambda *_args: (identity, workspace_root / ".kungfu/runtime", {}),
        status=lambda *_args: status,
        prepare_resume_profile=lambda *_args: {
            "status": "ready",
            "profileSuiteRoot": plan["workRef"]["profileRoot"],
        },
    )

    assert receipt["ok"] is True
    assert observed["work_profile_source"] == profile_source
    if expects_override:
        assert observed["envelope_override"] == console_envelope
        assert observed["console_workspace_root"] == str(workspace_root)
    else:
        assert "envelope_override" not in observed
        assert "console_workspace_root" not in observed
    assert observed["expected_binding"] == {
        "workRef": plan["workRef"],
        "session": binding["session"],
    }


def test_fresh_recovery_failure_keeps_public_executable_next_actions(
    tmp_path, monkeypatch
):
    emitted = []
    source = tmp_path / "missing-profile-source"
    failure = assignment_fresh_recovery.FreshRecoveryError(
        "WorkRef is unavailable",
        assignment_fresh_recovery._profile_recovery_actions(source),
    )
    monkeypatch.setattr(assignment_command, "_emit", emitted.append)

    with __import__("pytest").raises(__import__("click").exceptions.Exit):
        assignment_command._run(lambda: (_ for _ in ()).throw(failure))

    assert emitted[0]["ok"] is False
    assert emitted[0]["message"] == "WorkRef is unavailable"
    assert emitted[0]["next_actions"] == failure.next_actions
    assert emitted[0]["next_actions"][0]["command"] == [
        "kungfu",
        "profile",
        "history",
        "kungfu.work-control",
        "--json",
    ]
    assert emitted[0]["next_actions"][1]["command"] == [
        "kungfu",
        "profile",
        "validate",
        str(source),
        "--json",
    ]


def test_fresh_recovery_apply_preserves_complete_lifecycle_state():
    status, binding, plan = _fresh_recovery_fixture()
    events = []

    receipt = assignment_fresh_recovery.apply_plan(
        plan,
        expected_plan_root=plan["planRoot"],
        authorized_by="maintainer:test",
        status_reader=lambda: json.loads(json.dumps(status)),
        session_reader=lambda: dict(binding["session"]),
        prepare_profile=lambda actor: (
            events.append(("profile", actor))
            or {
                "status": "reconciled",
                "profileSuiteRoot": plan["workRef"]["profileRoot"],
            }
        ),
        bind_work=lambda expected: (
            events.append(("bind", expected))
            or {
                "workRef": dict(expected["workRef"]),
                "receipt": {"receiptRoot": f"sha256:{'8' * 64}"},
            }
        ),
        now="2026-08-25T09:01:00Z",
    )

    assert receipt["ok"] is True
    assert receipt["continuationMode"] == "resume/new-attempt"
    assert receipt["assignmentWrites"] == []
    assert receipt["preservation"]["phase"] == "completion-claimed"
    assert [event[0] for event in events] == ["profile", "bind"]


def test_fresh_recovery_ignores_profile_reader_work_semantics_projection():
    status, binding, plan = _fresh_recovery_fixture()
    projected = json.loads(json.dumps(status))
    projected["work_semantics"] = {
        "schema": "kungfu.work-semantics.status/v1",
        "phase": "completion-claimed",
        "next_actions": [{"action": "record-input-snapshot"}],
    }
    observations = iter(
        [
            projected,
            json.loads(json.dumps(status)),
            json.loads(json.dumps(status)),
        ]
    )

    receipt = assignment_fresh_recovery.apply_plan(
        plan,
        expected_plan_root=plan["planRoot"],
        authorized_by="maintainer:test",
        status_reader=lambda: next(observations),
        session_reader=lambda: dict(binding["session"]),
        prepare_profile=lambda _actor: {},
        bind_work=lambda expected: {
            "workRef": dict(expected["workRef"]),
            "receipt": {"receiptRoot": f"sha256:{'8' * 64}"},
        },
        now="2026-08-25T09:01:00Z",
    )

    assert receipt["ok"] is True
    assert receipt["assignmentWrites"] == []
    assert (
        receipt["preservation"]["lifecycleStateRoot"]
        == plan["work"]["lifecycleStateRoot"]
    )


def test_fresh_recovery_fails_closed_on_attempt_plan_or_state_drift():
    status, binding, plan = _fresh_recovery_fixture()
    with __import__("pytest").raises(ValueError, match="new SessionAttempt"):
        assignment_fresh_recovery.build_plan(
            workspace=plan["workspace"],
            status=status,
            binding=binding,
            previous_attempt_id="native:new",
            expected_request_root=status["assignment"]["request_root"],
            expected_work_definition_root=status["assignment"]["work_definition_root"],
            expected_profile_root=plan["workRef"]["profileRoot"],
            recovery_profile=plan["recoveryProfile"],
            profile_active=True,
        )
    drifted = json.loads(json.dumps(status))
    drifted["completion_claim_count"] = 2
    with __import__("pytest").raises(ValueError, match="lifecycle state changed"):
        assignment_fresh_recovery.apply_plan(
            plan,
            expected_plan_root=plan["planRoot"],
            authorized_by="maintainer:test",
            status_reader=lambda: drifted,
            session_reader=lambda: dict(binding["session"]),
            prepare_profile=lambda _actor: {},
            bind_work=lambda expected: {"workRef": dict(expected["workRef"])},
            now="2026-08-25T09:01:00Z",
        )
    forged = {**plan, "continuationMode": "first-attempt"}
    with __import__("pytest").raises(ValueError, match="root does not verify"):
        assignment_fresh_recovery.apply_plan(
            forged,
            expected_plan_root=plan["planRoot"],
            authorized_by="maintainer:test",
            status_reader=lambda: status,
            session_reader=lambda: dict(binding["session"]),
            prepare_profile=lambda _actor: {},
            bind_work=lambda expected: {"workRef": dict(expected["workRef"])},
            now="2026-08-25T09:01:00Z",
        )


def test_fresh_recovery_rejects_expiry_attempt_drift_and_unknown_effects():
    status, binding, plan = _fresh_recovery_fixture()
    mutations = []
    common = {
        "authorized_by": "maintainer:test",
        "status_reader": lambda: status,
        "prepare_profile": lambda _actor: mutations.append("profile") or {},
        "bind_work": lambda expected: (
            mutations.append("bind") or {"workRef": dict(expected["workRef"])}
        ),
    }
    with __import__("pytest").raises(ValueError, match="expired"):
        assignment_fresh_recovery.apply_plan(
            plan,
            expected_plan_root=plan["planRoot"],
            session_reader=lambda: dict(binding["session"]),
            now="2026-08-25T09:11:00Z",
            **common,
        )
    with __import__("pytest").raises(ValueError, match="another current"):
        assignment_fresh_recovery.apply_plan(
            plan,
            expected_plan_root=plan["planRoot"],
            session_reader=lambda: {
                **binding["session"],
                "sessionAttemptId": "native:other",
            },
            now="2026-08-25T09:01:00Z",
            **common,
        )
    forged_body = {
        **{key: value for key, value in plan.items() if key != "planRoot"},
        "effects": [*plan["effects"], {"stage": "admit"}],
    }
    forged = {
        **forged_body,
        "planRoot": assignment_fresh_recovery._root(forged_body),
    }
    with __import__("pytest").raises(ValueError, match="effect sequence"):
        assignment_fresh_recovery.apply_plan(
            forged,
            expected_plan_root=forged["planRoot"],
            session_reader=lambda: dict(binding["session"]),
            now="2026-08-25T09:01:00Z",
            **common,
        )
    assert mutations == []
