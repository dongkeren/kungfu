// SPDX-License-Identifier: Apache-2.0

import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';
import { fileURLToPath } from 'node:url';

import {
  prepareSettlement,
  verifySettlement,
} from '../framework/project-cut/src/settlement.mjs';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const FIXTURE = path.join(ROOT, 'xinfa', 'fixtures', 'repository-small');
const BINARY = path.join(
  ROOT,
  'xinfa',
  'target',
  'debug',
  process.platform === 'win32' ? 'xinfa.exe' : 'xinfa',
);

function run(root, command, args) {
  return execFileSync(command, args, { cwd: root, encoding: 'utf8' }).trim();
}

test('settlement compiles and promotes a real Xinfa successor Atlas', (t) => {
  assert.equal(
    fs.existsSync(BINARY),
    true,
    'Xinfa binary must be built by the Shifu task',
  );
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'project-cut-xinfa-'));
  t.after(() => fs.rmSync(root, { recursive: true, force: true }));
  fs.cpSync(FIXTURE, root, { recursive: true });
  run(root, 'git', ['init', '-q']);
  run(root, 'git', ['config', 'user.name', 'Settlement Integration']);
  run(root, 'git', ['config', 'user.email', 'settlement@example.invalid']);
  run(root, 'git', ['add', '--all']);
  run(root, 'git', ['commit', '-qm', 'test: Xinfa source fixture']);

  const before = path.join(root, '.xinfa', 'generated', 'before');
  fs.mkdirSync(path.dirname(before), { recursive: true });
  const baseline = JSON.parse(
    run(root, BINARY, [
      'atlas',
      'compile',
      '--project',
      'project.json',
      '--output',
      before,
      '--root',
      '.',
      '--visibility',
      'public',
      '--json',
    ]),
  );
  assert.equal(baseline.verdict, 'pass');
  fs.appendFileSync(
    path.join(root, 'src', 'runtime.rs'),
    '// unstaged workspace drift must not enter the successor Atlas\n',
  );

  const request = {
    schema: 'project.cut.settlement-request/v1',
    project: {
      id: 'small',
      identityRoot: `sha256:${'5'.repeat(64)}`,
    },
    parentCutRoots: [],
    visibility: 'public',
    authorityMode: 'bridge',
    source: { visibility: [] },
    atlas: {
      mode: 'episode-successor',
      before: '.xinfa/generated/before',
      project: 'project.json',
      submission: 'evidence/episode-submission.json',
      root: '.',
    },
    episodes: [{ semanticRoot: `sha256:${'a'.repeat(64)}` }],
    omissions: [],
    conflicts: [],
    unknowns: [],
  };
  const result = prepareSettlement(root, request, {
    xinfaBin: BINARY,
    execute: true,
    stage: true,
  });
  assert.notEqual(result.promotion.atlasRoot, baseline.atlas_root);
  assert.deepEqual(result.plan.unstagedPaths, ['src/runtime.rs']);
  assert.equal(verifySettlement(root, result.statePath).ok, true);
  assert.deepEqual(
    run(root, 'git', ['diff', '--cached', '--name-only']).split('\n'),
    [...result.plan.outputs].sort(),
  );
});
