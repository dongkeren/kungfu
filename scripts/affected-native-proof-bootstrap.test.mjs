// SPDX-License-Identifier: Apache-2.0

import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import test from 'node:test';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

function assertSourceOnlyClosure(entryModules) {
  const pending = [...entryModules];
  const visited = new Set();
  while (pending.length > 0) {
    const relativePath = pending.pop();
    if (visited.has(relativePath)) continue;
    visited.add(relativePath);
    const absolutePath = path.join(ROOT, relativePath);
    const source = fs.readFileSync(absolutePath, 'utf8');
    const specifiers = [
      ...source.matchAll(/(?:from|import)\s+['"]([^'"]+)['"]/gu),
    ].map((match) => match[1]);
    assert.deepEqual(
      specifiers.filter(
        (specifier) =>
          !specifier.startsWith('node:') && !specifier.startsWith('.'),
      ),
      [],
      `${relativePath} must run before workspace dependencies are installed`,
    );
    for (const specifier of specifiers.filter((entry) =>
      entry.startsWith('.'),
    )) {
      pending.push(
        path.relative(
          ROOT,
          path.resolve(path.dirname(absolutePath), specifier),
        ),
      );
    }
  }
  return visited;
}

test('affected-native proof bootstrap has no installed package imports', () => {
  const closure = assertSourceOnlyClosure([
    'scripts/affected-native-proof.mjs',
    'scripts/dev-delivery-warrant-input.mjs',
  ]);
  assert.ok(closure.has('scripts/project-cut-family-queue-lease.mjs'));
  assert.equal(
    closure.has('scripts/project-cut-merge-queue-admission.mjs'),
    false,
  );
});
