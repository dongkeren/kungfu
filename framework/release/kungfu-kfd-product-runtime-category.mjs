// SPDX-License-Identifier: Apache-2.0
// @ts-check

import {
  ADOPTER_DELIVERY_GATE_REQUEST_CONTRACT,
  createAdopterDeliveryGate,
  createGitCommitArtifactProfile,
} from '@kungfu-tech/buildchain/adopter-delivery-gate';
import {
  KFD_ADOPTER_CATEGORY_PROTOCOL_ID,
  KFD_ADOPTER_CATEGORY_PROTOCOL_VERSION,
  createKfdAdopterCategoryProtocolDriver,
} from '@kungfu-tech/buildchain/kfd-adopter-category-driver';

export const KUNGFU_KFD_PRODUCT_RUNTIME_CATEGORY_GATE =
  'kungfu-kfd-product-runtime-category-gate/v1';

const GIT_COMMIT_PROFILE = Object.freeze({
  id: 'buildchain.artifact/git-commit',
  version: '1.0.0',
});

const gate = createAdopterDeliveryGate({
  drivers: [createKfdAdopterCategoryProtocolDriver()],
  artifactProfiles: [createGitCommitArtifactProfile(GIT_COMMIT_PROFILE)],
});

function requireObject(value, label) {
  if (!value || typeof value !== 'object' || Array.isArray(value)) {
    throw new TypeError(`${label} must be a JSON object`);
  }
  return value;
}

export function createKungfuKfdProductRuntimeCategoryRequest(instanceManifest) {
  const instance = requireObject(instanceManifest, 'category instance');
  const project = requireObject(instance.project, 'category instance project');
  return {
    schemaVersion: 1,
    contract: ADOPTER_DELIVERY_GATE_REQUEST_CONTRACT,
    protocol: {
      id: KFD_ADOPTER_CATEGORY_PROTOCOL_ID,
      version: KFD_ADOPTER_CATEGORY_PROTOCOL_VERSION,
    },
    artifactProfile: GIT_COMMIT_PROFILE,
    project: {
      instanceId: instance.instanceId,
      adopterId: project.adopterId,
    },
    artifact: structuredClone(project.artifact),
    declaration: structuredClone(instance),
  };
}

export function evaluateKungfuKfdProductRuntimeCategory({
  instanceManifest,
  adopterManifest,
  verifiedAt,
  maxAgeSeconds,
} = {}) {
  return gate.evaluate(
    createKungfuKfdProductRuntimeCategoryRequest(instanceManifest),
    {
      adopterManifest: structuredClone(
        requireObject(adopterManifest, 'full-cut adopter manifest'),
      ),
      verifiedAt,
      maxAgeSeconds,
    },
  );
}
