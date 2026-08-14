// SPDX-License-Identifier: Apache-2.0

import assert from 'node:assert/strict';
import test from 'node:test';

import { resolvePublishedKfdAdopterCategoryProfiles } from '@kungfu-tech/buildchain/kfd-adopter-category-driver';
import { initAdopterManifest } from '@kungfu-tech/kfd/adopter-conformance/toolchain';
import { semanticRoot } from '@kungfu-tech/kfd/scripts/self-conformance-contract.mjs';

import {
  createKungfuKfdProductRuntimeCategoryRequest,
  evaluateKungfuKfdProductRuntimeCategory,
} from './kungfu-kfd-product-runtime-category.mjs';

const SOURCE_SHA = '1'.repeat(40);
const OBSERVED_AT = '2026-08-13T12:00:00Z';
const PACKAGE_ROOT = `sha256:${'c'.repeat(64)}`;

function rooted(label) {
  return semanticRoot({ label });
}

function fixture({ adopterId = 'kungfu-systems/kungfu' } = {}) {
  const artifact = {
    kind: 'git-commit',
    coordinate: `${adopterId}@${SOURCE_SHA}`,
    root: rooted(`${adopterId}:source-tree`),
  };
  const release = {
    kind: 'release',
    coordinate: `https://example.invalid/${adopterId}/releases/alpha`,
    root: rooted(`${adopterId}:release-passport`),
  };
  const adopterManifest = initAdopterManifest({
    manifestId: `${adopterId}:full-cut`,
    adopterId,
    artifactKind: artifact.kind,
    artifactCoordinate: artifact.coordinate,
    artifactRoot: artifact.root,
    scope: 'Product runtime, KFX, release, and recovery evidence',
    packageArtifactRoot: PACKAGE_ROOT,
    verifiedAt: OBSERVED_AT,
    maxAgeSeconds: 3600,
  });
  adopterManifest.releaseBindings.push({
    id: `${adopterId}:alpha`,
    artifact: structuredClone(artifact),
    releasePassport: structuredClone(release),
    kfdPackageRoot: PACKAGE_ROOT,
  });

  const selection = {
    schemaVersion: 1,
    contract: 'kfd.adopter-category-profile-selection/v1',
    profiles: [
      {
        id: 'kfd.adopter-category/product-runtime',
        version: '1.0.0',
      },
    ],
  };
  const resolution = resolvePublishedKfdAdopterCategoryProfiles(selection);
  assert.equal(resolution.valid, true);
  const project = {
    adopterId,
    source: structuredClone(artifact),
    artifact: structuredClone(artifact),
    release: structuredClone(release),
  };
  const projectRoot = semanticRoot(project);
  const adopterManifestRoot = semanticRoot(adopterManifest);
  const evidenceBinding = {
    observedAt: OBSERVED_AT,
    projectInstanceId: `${adopterId}@${SOURCE_SHA}`,
    projectRoot,
    adopterManifestRoot,
    kfdPackageRoot: PACKAGE_ROOT,
    categorySelectionRoot: resolution.selectionRoot,
  };
  const instanceManifest = {
    $schema:
      'https://kfd.libkungfu.dev/schemas/kfd-adopter-conformance/category-instance-manifest.schema.json',
    schemaVersion: 1,
    contract: 'kfd.adopter-category-instance-manifest/v1',
    instanceId: evidenceBinding.projectInstanceId,
    rootAlgorithm: 'sha256-kfd-canonical-json-v1',
    project,
    adopterManifest: {
      contract: 'kfd.adopter-conformance-manifest/v1',
      manifestId: adopterManifest.manifestId,
      root: adopterManifestRoot,
    },
    kfdCut: {
      packageVersion: adopterManifest.kfdCut.package.version,
      packageRoot: PACKAGE_ROOT,
      categoryCatalogRoot: resolution.catalogRoot,
    },
    selection,
    selectionRoot: resolution.selectionRoot,
    requirements: resolution.requirements.map((requirement) => ({
      id: requirement.id,
      evidence: requirement.evidenceKinds.map((kind) => ({
        kind,
        coordinate: `${artifact.coordinate}#${requirement.id}/${kind}`,
        root: rooted(`${adopterId}:${requirement.id}:${kind}`),
        ...evidenceBinding,
      })),
    })),
    claimBoundary: {
      categoryConformanceIsDeclarationOnly: true,
      evidenceTransfer: false,
      runtimePermission: false,
      releaseAuthorization: false,
      independentCertification: false,
      semanticAuthorityTransfer: false,
    },
  };
  return { adopterManifest, instanceManifest };
}

test('common gate accepts Kungfu as an ordinary product-runtime instance', () => {
  const value = fixture();
  const result = evaluateKungfuKfdProductRuntimeCategory({
    ...value,
    verifiedAt: OBSERVED_AT,
    maxAgeSeconds: 3600,
  });
  assert.equal(result.status, 'passed', JSON.stringify(result.issues));
  assert.equal(result.qualifying, false);
  assert.equal(result.selfCertified, false);
  assert.equal(result.semanticReport.valid, true);
  assert.deepEqual(result.issues, []);
});

test('the same evaluator accepts another project identity without a project branch', () => {
  const value = fixture({ adopterId: 'example.org/other-product' });
  const request = createKungfuKfdProductRuntimeCategoryRequest(
    value.instanceManifest,
  );
  assert.equal(request.project.adopterId, 'example.org/other-product');
  const result = evaluateKungfuKfdProductRuntimeCategory({
    ...value,
    verifiedAt: OBSERVED_AT,
    maxAgeSeconds: 3600,
  });
  assert.equal(result.status, 'passed', JSON.stringify(result.issues));
});

test('artifact substitution and stale evidence fail closed', () => {
  const substituted = fixture();
  substituted.instanceManifest.project.artifact.root = rooted('substitution');
  const substitutionResult = evaluateKungfuKfdProductRuntimeCategory({
    ...substituted,
    verifiedAt: OBSERVED_AT,
    maxAgeSeconds: 3600,
  });
  assert.equal(substitutionResult.status, 'failed');
  assert.equal(
    substitutionResult.issues.some(
      ({ code }) => code === 'acp-instance-binding-mismatch',
    ),
    true,
  );

  const stale = fixture();
  const staleResult = evaluateKungfuKfdProductRuntimeCategory({
    ...stale,
    verifiedAt: '2026-08-13T14:00:00Z',
    maxAgeSeconds: 60,
  });
  assert.equal(staleResult.status, 'failed');
  assert.equal(
    staleResult.issues.some(({ code }) => code === 'acp-evidence-stale'),
    true,
  );
});
