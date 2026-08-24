#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0

import assert from 'node:assert/strict';
import test from 'node:test';

import { SANITIZER_TARGETS, qualificationPlan } from './check-cpp-safety.mjs';

test('qualification uses one scoped CMake/CTest authority and the real watcher addon', () => {
  const plan = qualificationPlan('/repo');
  assert.equal(plan.sanitizer, 'address-undefined');
  assert.deepEqual(plan.targets, SANITIZER_TARGETS);
  assert.match(
    plan.configure.join(' '),
    /KUNGFU_CORE_SANITIZER=address-undefined/u,
  );
  assert.match(plan.ctest.join(' '), /kungfu_node_boundary_contract_tests/u);
  assert.match(plan.ctest.join(' '), /kungfu_fact_authority_contract_tests/u);
  assert.match(plan.ctest.join(' '), /kungfu_native_kfx_contract_tests/u);
  assert.match(plan.watcher.at(-1), /watcher-runtime-boundary\.test\.js$/u);
});
