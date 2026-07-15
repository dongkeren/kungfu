// SPDX-License-Identifier: Apache-2.0

import assert from 'node:assert/strict';
import { execFileSync, spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';
import { fileURLToPath } from 'node:url';

import {
  buildGitEpisodeSegment,
  sealGitEpisode,
} from '../framework/episode-provider/src/git-workspace-episode-provider.mjs';
import {
  canonicalJson,
  semanticRoot,
} from '../framework/project-cut/src/project-cut.mjs';
import {
  abandonSettlement,
  inspectSettlement,
  observeSettlementCommit,
  prepareSettlement,
  reconcileCommit,
  verifySettlement,
} from '../framework/project-cut/src/settlement.mjs';

const EPISODE_ROOT = `sha256:${'a'.repeat(64)}`;
const PROJECT_ROOT = `sha256:${'5'.repeat(64)}`;
const CONTEXT_PACK_ROOT = `sha256:${'6'.repeat(64)}`;
const SCHEMA_ROOT = `sha256:${'7'.repeat(64)}`;
const HOOK = path.resolve(
  path.dirname(fileURLToPath(import.meta.url)),
  '../framework/project-cut/hooks/project-cut-hook.mjs',
);
const CLI = path.resolve(
  path.dirname(fileURLToPath(import.meta.url)),
  '../framework/project-cut/bin/project-cut.mjs',
);

function git(root, ...args) {
  return execFileSync('git', args, { cwd: root, encoding: 'utf8' }).trim();
}

function writeJson(file, value) {
  fs.mkdirSync(path.dirname(file), { recursive: true });
  fs.writeFileSync(file, `${canonicalJson(value)}\n`);
}

function bundle() {
  return {
    schema: 'kungfu.storage.episode-bundle/v1',
    bundle_id: 'episode:7',
    scope: 'episode',
    episode_id: 7,
    authority: 'yijinjing-journal',
    manifest: {
      schema: 'kungfu.episode.manifest/v1',
      episode_id: 7,
      opened: true,
      closed: true,
      status: 'ended',
      content_root_algorithm: 'sha256',
      content_root: EPISODE_ROOT.slice(7),
    },
    records: [
      {
        manifest_frame_uid: 91,
        carrier_type: 10801,
        record: { episode_id: 7 },
      },
      {
        manifest_frame_uid: 92,
        carrier_type: 10805,
        record: { episode_id: 7 },
      },
      {
        manifest_frame_uid: 93,
        carrier_type: 10806,
        record: { root_value: EPISODE_ROOT.slice(7) },
      },
    ],
    refs: [],
    dependencies: [],
  };
}

function qualification() {
  return {
    schema: 'kungfu.episode.qualification/v1',
    policy_source: 'cpp-typed-fold-fsck',
    episode_id: 7,
    lifecycle: 'ended',
    status: 'ok',
    evidence: { manifest_integrity: { state: 'verified', issue_codes: [] } },
    issues: [],
    capabilities: [
      { name: 'export_evidence', safe: true, requires: [], blocked_by: [] },
    ],
    safe_capabilities: ['export_evidence'],
    contractions: [],
    repair_prerequisites: [],
  };
}

function atlas(directory) {
  const core = {
    schema: 'xinfa.atlas/v1',
    roots: { schema: SCHEMA_ROOT },
    compiler: { product: 'xinfa', version: '0.1.0', cache_used: false },
  };
  const xinfaRoot = (value) =>
    `sha256:${createHash('sha256')
      .update(`${canonicalJson(value)}\n`)
      .digest('hex')}`;
  const atlasValue = { ...core, atlas_root: xinfaRoot(core) };
  const manifestCore = {
    schema: 'xinfa.atlas-manifest/v1',
    atlas_root: atlasValue.atlas_root,
    context_pack_root: CONTEXT_PACK_ROOT,
    artifacts: [],
  };
  const manifest = { ...manifestCore, manifest_root: xinfaRoot(manifestCore) };
  const receiptCore = {
    schema: 'xinfa.atlas-compile-receipt/v1',
    verdict: 'pass',
    atlas_root: atlasValue.atlas_root,
    context_pack_root: CONTEXT_PACK_ROOT,
    manifest_root: manifest.manifest_root,
    qualifying: false,
    selfCertified: false,
    writesCache: false,
  };
  const receipt = { ...receiptCore, receipt_root: xinfaRoot(receiptCore) };
  writeJson(path.join(directory, 'atlas.json'), atlasValue);
  writeJson(path.join(directory, 'manifest.json'), manifest);
  writeJson(path.join(directory, 'receipt.json'), receipt);
  return atlasValue.atlas_root;
}

function request() {
  return {
    schema: 'project.cut.settlement-request/v1',
    project: { id: 'example/project', identityRoot: PROJECT_ROOT },
    parentCutRoots: [],
    visibility: 'public',
    authorityMode: 'bridge',
    source: { visibility: [] },
    atlas: { mode: 'existing', path: '.xinfa/generated/atlas' },
    episodes: [{ semanticRoot: EPISODE_ROOT }],
    omissions: [],
    conflicts: [],
    unknowns: [],
  };
}

function workspace(t) {
  const root = fs.mkdtempSync(
    path.join(os.tmpdir(), 'project-cut-settlement-'),
  );
  t.after(() => fs.rmSync(root, { recursive: true, force: true }));
  git(root, 'init', '-q');
  git(root, 'config', 'user.name', 'Settlement Test');
  git(root, 'config', 'user.email', 'settlement@example.invalid');
  fs.mkdirSync(path.join(root, 'src'), { recursive: true });
  fs.writeFileSync(path.join(root, 'src', 'app.txt'), 'v1\n');
  sealGitEpisode(root, buildGitEpisodeSegment(bundle(), qualification()), {
    writerId: 'settlement-test',
  });
  atlas(path.join(root, '.xinfa', 'generated', 'atlas'));
  writeJson(path.join(root, 'settlement-request.json'), request());
  git(root, 'add', '--all');
  git(root, 'commit', '-qm', 'test: baseline');
  return root;
}

test('prepare is deterministic, dry-run does not mutate, and explicit staging is exact', (t) => {
  const root = workspace(t);
  const before = git(root, 'status', '--short');
  const first = prepareSettlement(root, request());
  const second = prepareSettlement(root, request());
  assert.equal(first.cut.cutRoot, second.cut.cutRoot);
  assert.equal(first.sourceProjection.root, second.sourceProjection.root);
  assert.equal(git(root, 'status', '--short'), before);
  assert.equal(first.dryRun, true);

  const applied = prepareSettlement(root, request(), {
    execute: true,
    stage: true,
  });
  const staged = git(root, 'diff', '--cached', '--name-only').split('\n');
  assert.deepEqual(staged, [...applied.plan.outputs].sort());
  assert.equal(applied.state.status, 'prepared');
  const verified = verifySettlement(root, applied.statePath, { execute: true });
  assert.equal(verified.ok, true, JSON.stringify(verified.receipt.diagnostics));
  assert.equal(verified.state.status, 'verified');
});

test('commit observe preserves sealed-unpublished state, then proves publication', (t) => {
  const root = workspace(t);
  const applied = prepareSettlement(root, request(), {
    execute: true,
    stage: true,
  });
  const beforeCommit = observeSettlementCommit(
    root,
    applied.statePath,
    'HEAD',
    {
      execute: true,
    },
  );
  assert.equal(beforeCommit.ok, false);
  assert.equal(beforeCommit.state.status, 'sealed-unpublished');
  assert.ok(
    beforeCommit.receipt.diagnostics.every(
      (entry) => entry.code === 'sealed-unpublished',
    ),
  );

  git(root, 'commit', '-qm', 'test: publish project cut');
  const published = observeSettlementCommit(root, applied.statePath, 'HEAD', {
    execute: true,
  });
  assert.equal(
    published.ok,
    true,
    JSON.stringify(published.receipt.diagnostics),
  );
  assert.equal(published.state.status, 'published');
  assert.equal(reconcileCommit(root, 'HEAD').ok, true);
  assert.equal(
    inspectSettlement(root, applied.statePath).cut.cutRoot,
    applied.cut.cutRoot,
  );

  const receiptPath = applied.plan.outputs.find((entry) =>
    entry.endsWith('/receipt.json'),
  );
  const receipt = JSON.parse(
    fs.readFileSync(path.join(root, receiptPath), 'utf8'),
  );
  receipt.artifactDigest = `sha256:${'0'.repeat(64)}`;
  writeJson(path.join(root, receiptPath), receipt);
  git(root, 'add', receiptPath);
  git(root, 'commit', '-qm', 'test: corrupt tracked cut receipt');
  const corrupt = reconcileCommit(root, 'HEAD');
  assert.equal(corrupt.ok, false);
  assert.ok(
    corrupt.diagnostics.some((entry) => entry.code === 'receipt-mismatch'),
  );
});

test('source drift, private input, and a commit without a cut fail visibly', (t) => {
  const root = workspace(t);
  assert.throws(() => prepareSettlement(root, { ...request(), rogue: true }), {
    code: 'unknown-field',
  });
  assert.equal(
    reconcileCommit(root, 'HEAD').diagnostics[0].code,
    'missing-cut',
  );
  const applied = prepareSettlement(root, request(), {
    execute: true,
    stage: true,
  });
  fs.writeFileSync(path.join(root, 'src', 'app.txt'), 'v2\n');
  git(root, 'add', 'src/app.txt');
  const drift = verifySettlement(root, applied.statePath);
  assert.equal(drift.ok, false);
  assert.ok(
    drift.receipt.diagnostics.some((entry) => entry.code === 'source-drift'),
  );

  fs.mkdirSync(path.join(root, '.private'), { recursive: true });
  fs.writeFileSync(
    path.join(root, '.private', 'secret.txt'),
    'not-a-secret-fixture\n',
  );
  git(root, 'add', '-f', '.private/secret.txt');
  assert.throws(() => prepareSettlement(root, request()), {
    code: 'privacy-denied',
  });
});

test('partial staging remains explicit and abandonment requires execute', (t) => {
  const root = workspace(t);
  fs.writeFileSync(path.join(root, 'src', 'app.txt'), 'unstaged-v2\n');
  const planned = prepareSettlement(root, request());
  assert.deepEqual(planned.plan.unstagedPaths, ['src/app.txt']);
  assert.equal(git(root, 'status', '--short'), 'M src/app.txt');

  const applied = prepareSettlement(root, request(), { execute: true });
  assert.equal(abandonSettlement(root, applied.statePath).dryRun, true);
  const abandoned = abandonSettlement(root, applied.statePath, {
    execute: true,
  });
  assert.equal(abandoned.state.status, 'abandoned');
});

test('thin hooks verify and observe through the public settlement core', (t) => {
  const root = workspace(t);
  const applied = prepareSettlement(root, request(), {
    execute: true,
    stage: true,
  });
  const environment = {
    ...process.env,
    PROJECT_CUT_SETTLEMENT_STATE: applied.statePath,
  };
  const before = spawnSync(process.execPath, [HOOK, 'pre-commit'], {
    cwd: root,
    env: environment,
    encoding: 'utf8',
  });
  assert.equal(before.status, 0, before.stdout || before.stderr);
  assert.equal(JSON.parse(before.stdout).authority, false);

  git(root, 'commit', '-qm', 'test: publish through hook adapter');
  const after = spawnSync(process.execPath, [HOOK, 'post-commit'], {
    cwd: root,
    env: environment,
    encoding: 'utf8',
  });
  assert.equal(after.status, 0, after.stdout || after.stderr);
  const observed = JSON.parse(after.stdout);
  assert.equal(observed.state.status, 'published');
  assert.equal(observed.authority, false);

  const unconfigured = spawnSync(process.execPath, [HOOK, 'pre-commit'], {
    cwd: root,
    env: { ...process.env, PROJECT_CUT_SETTLEMENT_STATE: '' },
    encoding: 'utf8',
  });
  assert.equal(JSON.parse(unconfigured.stdout).outcome, 'not-configured');
});

test('CLI emits one stable JSON envelope and requires the agent-first flag', (t) => {
  const root = workspace(t);
  const command = [
    CLI,
    'prepare',
    '--root',
    root,
    '--request',
    path.join(root, 'settlement-request.json'),
  ];
  const result = spawnSync(process.execPath, [...command, '--json'], {
    cwd: root,
    encoding: 'utf8',
  });
  assert.equal(result.status, 0, result.stdout || result.stderr);
  const response = JSON.parse(result.stdout);
  assert.equal(response.schema, 'project.cut.settlement-response/v1');
  assert.equal(response.action, 'prepare');
  assert.equal(response.dryRun, true);

  const rejected = spawnSync(process.execPath, command, {
    cwd: root,
    encoding: 'utf8',
  });
  assert.equal(rejected.status, 1);
  assert.equal(JSON.parse(rejected.stdout).error.code, 'json-required');
});
