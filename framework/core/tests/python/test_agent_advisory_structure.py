# SPDX-License-Identifier: Apache-2.0

from kungfu.agent import advisory
from kungfu.agent import resources


def test_agent_resource_facade_reexports_owned_advisory_policy() -> None:
    assert resources.assess_skill_decision.__module__ == "kungfu.agent.resources"
    assert resources.assess_work_advisory.__module__ == "kungfu.agent.resources"
    assert resources.validate_skill_decision_signals.__module__ == (
        "kungfu.agent.resources"
    )
    assert resources.validate_work_advisory_signals.__module__ == (
        "kungfu.agent.resources"
    )


def test_agent_advisory_policy_keeps_resource_authority_on_the_facade() -> None:
    assert advisory._resources() is resources
    assert advisory.evaluate_skill_decision.__module__ == "kungfu.agent.advisory"
    assert advisory.evaluate_work_advisory.__module__ == "kungfu.agent.advisory"
