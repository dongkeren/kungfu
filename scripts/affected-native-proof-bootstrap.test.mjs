// SPDX-License-Identifier: Apache-2.0

import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';
import { fileURLToPath, pathToFileURL } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const SOURCE_ONLY_CLOSURE = [
  'scripts/affected-native-proof.mjs',
  'scripts/dev-delivery-warrant-input.mjs',
  'scripts/project-cut-family-queue-lease.mjs',
  'product/release/affected-native-artifact-lookup.mjs',
  'product/release/affected-native-proof-cli.mjs',
  'framework/spec/format/project-cut-canonical-json.mjs',
];

function copySourceOnlyClosure(destination, omitted = '') {
  for (const relativePath of SOURCE_ONLY_CLOSURE) {
    if (relativePath === omitted) continue;
    const output = path.join(destination, relativePath);
    fs.mkdirSync(path.dirname(output), { recursive: true });
    fs.copyFileSync(path.join(ROOT, relativePath), output);
  }
}

test('affected-native Warrant bootstrap loads from its source-only closure', async (t) => {
  const destination = fs.mkdtempSync(
    path.join(os.tmpdir(), 'kungfu-warrant-bootstrap-'),
  );
  t.after(() => fs.rmSync(destination, { recursive: true, force: true }));
  copySourceOnlyClosure(destination);
  assert.equal(fs.existsSync(path.join(destination, 'node_modules')), false);
  await import(
    `${pathToFileURL(path.join(destination, 'scripts/dev-delivery-warrant-input.mjs')).href}?complete`
  );
});

test('affected-native Warrant bootstrap fails when its source closure is incomplete', async (t) => {
  const destination = fs.mkdtempSync(
    path.join(os.tmpdir(), 'kungfu-warrant-bootstrap-missing-'),
  );
  t.after(() => fs.rmSync(destination, { recursive: true, force: true }));
  copySourceOnlyClosure(
    destination,
    'scripts/project-cut-family-queue-lease.mjs',
  );
  await assert.rejects(
    import(
      `${pathToFileURL(path.join(destination, 'scripts/dev-delivery-warrant-input.mjs')).href}?missing`
    ),
    (error) =>
      error?.code === 'ERR_MODULE_NOT_FOUND' &&
      String(error.message).includes('project-cut-family-queue-lease.mjs'),
  );
});
