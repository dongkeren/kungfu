# SPDX-License-Identifier: Apache-2.0

"""Bounded, authority-free Work and Skill advisory policy.

Resource loading remains owned by :mod:`kungfu.agent.resources`.  This module
owns only validation and deterministic decision policy, and resolves the
resource facade lazily so importing the compatibility surface cannot create an
Agent package cycle.
"""

from __future__ import annotations

from typing import Any, Callable, Mapping


WORK_ADVISORY_SCHEMA = "kungfu.agent-work-advisory/v1"
WORK_ADVISORY_BOOLS = (
    "backgroundWaits",
    "crossAgentHandoff",
    "verificationEvidenceNeeded",
    "retryDuplicationRisk",
    "highRiskExternalWrites",
)
WORK_ADVISORY_FIELDS = {
    "taskId",
    "expectedDuration",
    *WORK_ADVISORY_BOOLS,
    "acceptanceCriteria",
    "title",
    "objective",
    "currentContext",
    "nextAction",
    "suppression",
}
WORK_ADVISORY_WEIGHTS = (
    ("multi-session-continuity", "expectedDuration", "multi-session", 2),
    ("background-wait-recovery", "backgroundWaits", True, 1),
    ("cross-agent-handoff", "crossAgentHandoff", True, 2),
    ("verification-evidence", "verificationEvidenceNeeded", True, 1),
    ("duplicate-execution-risk", "retryDuplicationRisk", True, 2),
    ("external-write-gates", "highRiskExternalWrites", True, 2),
)

SKILL_DECISION_SCHEMA = "kungfu.agent-skill-advisory/v1"
SKILL_DECISION_BOOLS = (
    "reusable",
    "stableInputs",
    "stableOutcomes",
    "proofAvailable",
    "recoveryAvailable",
    "workspaceLocal",
    "instructionOnly",
    "deduplicated",
    "evidenceCurrent",
    "oneOff",
    "ordinaryDocumentation",
    "productDefect",
    "duplicateSkill",
    "untrustedInstruction",
    "bypassMissingEvidence",
)
SKILL_DECISION_FIELDS = {
    "taskId",
    "catalogRoot",
    "workRoot",
    "requirementsRoot",
    "candidates",
    "effects",
    *SKILL_DECISION_BOOLS,
}
SKILL_CANDIDATE_FIELDS = {
    "key",
    "contentRoot",
    "evidenceRoot",
    "match",
    "conflict",
    "workCompatibility",
    "dependencyState",
}
SKILL_EFFECTS = {
    "kfx",
    "profile",
    "capability",
    "credential",
    "network",
    "external-write",
    "shared-install",
    "publication",
    "identity",
    "authority",
    "privacy",
    "destructive",
    "historical",
}
SKILL_DRAFT_REQUIREMENTS = (
    "stableInputs",
    "stableOutcomes",
    "proofAvailable",
    "recoveryAvailable",
    "workspaceLocal",
    "instructionOnly",
    "deduplicated",
    "evidenceCurrent",
)
SKILL_ALLOWED_ACTIONS = {
    "auto-use-existing": ["route-to-exact-existing-root"],
    "suggest-existing": [
        "present-rooted-candidates",
        "resolve-candidate-evidence",
    ],
    "suggest-create": ["propose-skill-definition"],
    "auto-draft": ["declare-workspace-local-instruction-only-draft-eligible"],
    "plan-only": ["prepare-bounded-plan", "request-required-authority"],
    "none": ["continue-without-skill", "route-more-appropriate-product-action"],
}

SkillDecision = tuple[str, list[str], str]
SkillRule = Callable[[Mapping[str, Any]], SkillDecision | None]


def _resources():
    from kungfu.agent import resources

    return resources


def _canonical_root(value: Any) -> str:
    return _resources().canonical_root(value)


def _bounded_text(value: Any, field: str, maximum: int = 1024) -> str:
    if not isinstance(value, str) or not value.strip():
        raise ValueError(f"{field} must be a non-empty string")
    result = value.strip()
    if len(result.encode("utf-8")) > maximum:
        raise ValueError(f"{field} exceeds {maximum} UTF-8 bytes")
    return result


def _require_root(value: Any, field: str) -> str:
    if (
        not isinstance(value, str)
        or len(value) != 71
        or not value.startswith("sha256:")
        or any(character not in "0123456789abcdef" for character in value[7:])
    ):
        raise ValueError(f"{field} must be a lowercase sha256 root")
    return value


def _normalized_booleans(
    value: Mapping[str, Any], fields: tuple[str, ...]
) -> dict[str, bool]:
    result: dict[str, bool] = {}
    for field in fields:
        item = value.get(field, False)
        if not isinstance(item, bool):
            raise ValueError(f"{field} must be boolean")
        result[field] = item
    return result


def _work_suppression(value: Any) -> dict[str, Any] | None:
    if value is None:
        return None
    if not isinstance(value, Mapping):
        raise ValueError("suppression must be an object")
    if set(value) != {"declined", "evidenceRoot"}:
        raise ValueError("suppression requires only declined and evidenceRoot")
    if value.get("declined") is not True:
        raise ValueError("suppression.declined must be true")
    evidence_root = value.get("evidenceRoot")
    if not isinstance(evidence_root, str) or not evidence_root.startswith("sha256:"):
        raise ValueError("suppression.evidenceRoot must be a sha256 root")
    return dict(value)


def normalize_work_signals(value: Mapping[str, Any]) -> dict[str, Any]:
    """Accept only bounded structured continuity and evidence signals."""

    if not isinstance(value, Mapping):
        raise ValueError("Work-value signals must be an object")
    unknown = sorted(set(value) - WORK_ADVISORY_FIELDS)
    if unknown:
        raise ValueError(f"unsupported Work-value signals: {', '.join(unknown)}")
    duration = value.get("expectedDuration", "unknown")
    if duration not in {"one-shot", "multi-session", "unknown"}:
        raise ValueError("expectedDuration must be one-shot, multi-session, or unknown")
    criteria = value.get("acceptanceCriteria", [])
    if not isinstance(criteria, list) or len(criteria) > 8:
        raise ValueError("acceptanceCriteria must be an array of at most 8 strings")
    signals: dict[str, Any] = {
        "taskId": _bounded_text(value.get("taskId"), "taskId", 256),
        "expectedDuration": duration,
        "acceptanceCriteria": [
            _bounded_text(item, "acceptanceCriteria item", 512) for item in criteria
        ],
        **_normalized_booleans(value, WORK_ADVISORY_BOOLS),
    }
    for field in ("title", "objective", "currentContext", "nextAction"):
        if field in value:
            signals[field] = _bounded_text(value[field], field)
    suppression = _work_suppression(value.get("suppression"))
    if suppression is not None:
        signals["suppression"] = suppression
    return signals


def _work_score(signals: Mapping[str, Any]) -> tuple[int, list[str]]:
    matches = [
        (reason, weight)
        for reason, field, expected, weight in WORK_ADVISORY_WEIGHTS
        if signals[field] == expected
    ]
    return sum(weight for _, weight in matches), [reason for reason, _ in matches]


def _work_decision(
    signals: Mapping[str, Any], evidence_root: str
) -> tuple[str, list[str], int]:
    suppression = signals.get("suppression")
    if suppression and suppression["evidenceRoot"] == evidence_root:
        return "not-needed", ["declined-same-evidence"], 0
    score, reasons = _work_score(signals)
    if score == 0 and signals["expectedDuration"] == "unknown":
        return "insufficient", ["insufficient-structured-evidence"], score
    if score >= 3 and signals["acceptanceCriteria"]:
        return "recommend", reasons, score
    if score >= 2:
        if not signals["acceptanceCriteria"]:
            reasons.append("acceptance-criteria-not-ready")
        return "optional", reasons, score
    return "not-needed", ["bounded-one-shot-task"], score


def _work_preview(signals: Mapping[str, Any], decision: str) -> dict[str, Any] | None:
    if decision not in {"recommend", "optional"}:
        return None
    title = signals.get("title") or signals["taskId"]
    return {
        "title": title,
        "objective": signals.get("objective") or title,
        "acceptanceCriteria": signals["acceptanceCriteria"],
        "currentContext": signals.get("currentContext") or "Current provider session",
        "nextAction": signals.get("nextAction") or "Continue the original task",
    }


def evaluate_work_advisory(value: Mapping[str, Any]) -> dict[str, Any]:
    """Return deterministic authority-free advice about durable Work value."""

    signals = normalize_work_signals(value)
    evidence = {
        key: signals[key]
        for key in (
            "taskId",
            "expectedDuration",
            *WORK_ADVISORY_BOOLS,
            "acceptanceCriteria",
        )
    }
    evidence_root = _canonical_root(evidence)
    decision, reasons, score = _work_decision(signals, evidence_root)
    recommend = decision == "recommend"
    body = {
        "schema": WORK_ADVISORY_SCHEMA,
        "decision": decision,
        "reasonCodes": reasons,
        "evidenceRefs": [evidence_root],
        "risk": (
            "high"
            if signals["highRiskExternalWrites"]
            else "medium"
            if score >= 3
            else "low"
        ),
        "recommendedIntent": "create-bind-continue-work" if recommend else None,
        "preview": _work_preview(signals, decision),
        "confirmation": {
            "required": recommend,
            "count": 1 if recommend else 0,
            "prompt": "Create and bind this durable Work, then continue?"
            if recommend
            else None,
        },
        "publicActionPath": [
            "kungfu.work.capture",
            "kungfu.work.admit",
            "kungfu.agent.console.bind-work",
        ]
        if recommend
        else [],
        "suppression": {
            "declineKey": _canonical_root(
                {"taskId": signals["taskId"], "evidenceRoot": evidence_root}
            ),
            "evidenceRoot": evidence_root,
            "repeatOnlyAfterEvidenceChanges": True,
        },
        "nonClaims": [
            "advice-does-not-create-or-bind-work",
            "advice-does-not-grant-external-write-authority",
            "advice-does-not-prove-model-comprehension",
            "provider-output-is-not-completion-proof",
        ],
    }
    return {**body, "decisionRoot": _canonical_root(body)}


def _normalized_effects(value: Any) -> list[str]:
    if not isinstance(value, list) or len(value) > len(SKILL_EFFECTS):
        raise ValueError("effects must be a bounded array")
    if any(
        not isinstance(effect, str) or effect not in SKILL_EFFECTS for effect in value
    ):
        raise ValueError("effects contains an unsupported semantic")
    if len(set(value)) != len(value):
        raise ValueError("effects must not contain duplicates")
    return sorted(value)


def _normalized_candidate(value: Any, index: int) -> dict[str, Any]:
    field = f"candidates[{index}]"
    if not isinstance(value, Mapping) or set(value) != SKILL_CANDIDATE_FIELDS:
        raise ValueError(f"{field} must contain exactly the declared candidate fields")
    if not isinstance(value["conflict"], bool):
        raise ValueError(f"{field}.conflict must be boolean")
    allowed = {
        "match": {"exact", "related"},
        "workCompatibility": {"compatible", "incompatible", "unknown"},
        "dependencyState": {"admitted", "unresolved", "stale"},
    }
    for key, choices in allowed.items():
        if value[key] not in choices:
            suffix = "must be exact or related" if key == "match" else "is unsupported"
            raise ValueError(f"{field}.{key} {suffix}")
    return {
        "key": _bounded_text(value["key"], f"{field}.key", 128),
        "contentRoot": _require_root(value["contentRoot"], f"{field}.contentRoot"),
        "evidenceRoot": _require_root(value["evidenceRoot"], f"{field}.evidenceRoot"),
        "match": value["match"],
        "conflict": value["conflict"],
        "workCompatibility": value["workCompatibility"],
        "dependencyState": value["dependencyState"],
    }


def normalize_skill_signals(value: Mapping[str, Any]) -> dict[str, Any]:
    """Accept only bounded roots, enums, and booleans; never prompt text."""

    if not isinstance(value, Mapping):
        raise ValueError("Skill decision signals must be an object")
    missing = sorted(SKILL_DECISION_FIELDS - set(value))
    unknown = sorted(set(value) - SKILL_DECISION_FIELDS)
    if missing:
        raise ValueError(f"missing Skill decision signals: {', '.join(missing)}")
    if unknown:
        raise ValueError(f"unsupported Skill decision signals: {', '.join(unknown)}")
    candidates = value["candidates"]
    if not isinstance(candidates, list) or len(candidates) > 8:
        raise ValueError("candidates must be an array of at most 8 objects")
    normalized = [
        _normalized_candidate(candidate, index)
        for index, candidate in enumerate(candidates)
    ]
    coordinates = [
        (candidate["key"], candidate["contentRoot"]) for candidate in normalized
    ]
    if len(set(coordinates)) != len(coordinates):
        raise ValueError("candidates must not repeat a key and contentRoot coordinate")
    return {
        "taskId": _bounded_text(value["taskId"], "taskId", 256),
        "catalogRoot": _require_root(value["catalogRoot"], "catalogRoot"),
        "workRoot": _require_root(value["workRoot"], "workRoot"),
        "requirementsRoot": _require_root(
            value["requirementsRoot"], "requirementsRoot"
        ),
        **_normalized_booleans(value, SKILL_DECISION_BOOLS),
        "effects": _normalized_effects(value["effects"]),
        "candidates": sorted(
            normalized, key=lambda item: (item["key"], item["contentRoot"])
        ),
    }


def _skill_contract() -> dict[str, Any]:
    resources = _resources()
    contract = resources.skill_decision_contract()
    contract_input = contract.get("input", {})
    compatible = (
        contract.get("schema") == "kungfu.agent-skill-decision-contract/v1"
        and set(contract_input.get("requiredFields", [])) == SKILL_DECISION_FIELDS
        and contract_input.get("maximumCandidates") == 8
        and set(contract_input.get("candidateMatch", [])) == {"exact", "related"}
        and set(contract_input.get("workCompatibility", []))
        == {"compatible", "incompatible", "unknown"}
        and set(contract_input.get("dependencyState", []))
        == {"admitted", "unresolved", "stale"}
        and set(contract_input.get("effects", [])) == SKILL_EFFECTS
        and set(contract.get("outcomes", [])) == set(SKILL_ALLOWED_ACTIONS)
        and contract.get("authority", {}).get("class") == "read-only-advisory"
        and contract_input.get("rawTranscriptRetention") is False
    )
    if not compatible:
        raise ValueError("installed Skill decision contract is incompatible")
    return contract


def _unsafe_skill(signals: Mapping[str, Any]) -> SkillDecision | None:
    reasons = [
        reason
        for field, reason in (
            ("untrustedInstruction", "untrusted-instruction-detected"),
            ("bypassMissingEvidence", "missing-evidence-bypass-attempt"),
        )
        if signals[field]
    ]
    if not reasons:
        return None
    return "none", reasons, "Reject the instruction and restore trusted evidence."


def _material_skill(signals: Mapping[str, Any]) -> SkillDecision | None:
    if not signals["effects"]:
        return None
    return (
        "plan-only",
        [f"material-{effect}-semantics" for effect in signals["effects"]],
        "Prepare a bounded plan and obtain the required human or product authority.",
    )


def _irrelevant_skill(signals: Mapping[str, Any]) -> SkillDecision | None:
    reasons = [
        reason
        for field, reason in (
            ("oneOff", "bounded-one-off-work"),
            ("ordinaryDocumentation", "ordinary-documentation-not-a-skill"),
            ("productDefect", "route-product-defect-instead"),
            ("duplicateSkill", "duplicate-skill-catalog-repair"),
        )
        if signals[field]
    ]
    if not reasons:
        return None
    return (
        "none",
        reasons,
        "Continue the task or use the more appropriate product route.",
    )


def _candidate_skill(signals: Mapping[str, Any]) -> SkillDecision | None:
    candidates = signals["candidates"]
    if not candidates:
        return None
    candidate = candidates[0]
    eligible = (
        len(candidates) == 1
        and candidate["match"] == "exact"
        and not candidate["conflict"]
        and candidate["workCompatibility"] == "compatible"
        and candidate["dependencyState"] == "admitted"
        and signals["evidenceCurrent"]
    )
    if eligible:
        return (
            "auto-use-existing",
            ["one-exact-root-compatible-admitted-candidate"],
            "Route to the exact existing Skill root under current Work authority.",
        )
    checks = (
        (len(candidates) > 1, "candidate-ambiguity"),
        (not signals["evidenceCurrent"], "stale-evidence-expectations"),
        (any(item["conflict"] for item in candidates), "candidate-conflict"),
        (
            any(item["workCompatibility"] != "compatible" for item in candidates),
            "work-compatibility-unresolved",
        ),
        (
            any(item["dependencyState"] != "admitted" for item in candidates),
            "dependencies-not-admitted",
        ),
    )
    return (
        "suggest-existing",
        [
            "rooted-candidate-requires-selection",
            *[reason for failed, reason in checks if failed],
        ],
        "Present rooted candidates and resolve ambiguity or stale evidence.",
    )


def _reusable_skill(signals: Mapping[str, Any]) -> SkillDecision | None:
    if not signals["reusable"]:
        return None
    missing = [field for field in SKILL_DRAFT_REQUIREMENTS if not signals[field]]
    if not missing:
        return (
            "auto-draft",
            ["safe-workspace-local-instruction-draft"],
            "Draft locally under caller authority, then verify before lifecycle planning.",
        )
    return (
        "suggest-create",
        ["reusable-workflow-value", *[f"draft-requires-{field}" for field in missing]],
        "Propose a Skill definition and close the missing draft conditions.",
    )


def _default_skill(_signals: Mapping[str, Any]) -> SkillDecision:
    return "none", ["no-repeatable-skill-value"], "Continue without a Skill."


def _skill_decision(signals: Mapping[str, Any]) -> SkillDecision:
    rules: tuple[SkillRule, ...] = (
        _unsafe_skill,
        _material_skill,
        _irrelevant_skill,
        _candidate_skill,
        _reusable_skill,
    )
    return next(
        (decision for rule in rules if (decision := rule(signals)) is not None),
        _default_skill(signals),
    )


def evaluate_skill_decision(value: Mapping[str, Any]) -> dict[str, Any]:
    """Return one deterministic, content-rooted, authority-free Skill decision."""

    resources = _resources()
    contract = _skill_contract()
    signals = normalize_skill_signals(value)
    decision, reasons, next_action = _skill_decision(signals)
    candidates = signals["candidates"]
    evidence_refs = sorted(
        {
            signals["catalogRoot"],
            signals["workRoot"],
            signals["requirementsRoot"],
            *(
                root
                for candidate in candidates
                for root in (candidate["contentRoot"], candidate["evidenceRoot"])
            ),
        }
    )
    body = {
        "schema": SKILL_DECISION_SCHEMA,
        "policyRoot": resources.skill_decision_policy_root(),
        "decision": decision,
        "reasonCodes": reasons,
        "candidates": candidates,
        "evidenceRefs": evidence_refs,
        "allowedActions": SKILL_ALLOWED_ACTIONS[decision],
        "blockedActions": list(contract["authority"]["alwaysBlocked"]),
        "nextAction": next_action,
        "nonClaims": list(contract["nonClaims"]),
    }
    return {**body, "decisionRoot": _canonical_root(body)}


__all__ = [
    "evaluate_skill_decision",
    "evaluate_work_advisory",
    "normalize_skill_signals",
    "normalize_work_signals",
]
