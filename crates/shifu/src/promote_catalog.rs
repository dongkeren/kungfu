// SPDX-License-Identifier: Apache-2.0

use super::*;

fn build_identity_recorded(entry: &BuildEntry) -> bool {
    !entry.sha.is_empty()
        && entry.sha != "unknown"
        && !entry.sha.ends_with("-dirty")
        && matches!(entry.kind.as_str(), "app" | "installer" | "appimage")
        && entry.digest.len() == 64
        && entry.digest.bytes().all(|byte| byte.is_ascii_hexdigit())
}

fn release_sidecars_recorded(entry: &BuildEntry) -> bool {
    !entry.cli_archive.is_empty()
        && content_root_valid(&entry.cli_archive_digest)
        && entry.slot.join(&entry.cli_archive).is_file()
        && !entry.upgrade_manifest.is_empty()
        && content_root_valid(&entry.upgrade_manifest_digest)
        && entry.slot.join(&entry.upgrade_manifest).is_file()
        && !entry.product_version.is_empty()
        && content_root_valid(&entry.release_cut_root)
        && content_root_valid(&entry.platform_slice_root)
}

fn mainline_registration_valid(entry: &BuildEntry) -> bool {
    entry.mainline_ref == product_mainline_ref()
        && entry.integrated
        && entry.qualified
        && entry.sha == entry.mainline_sha
}

pub(super) fn recorded_build_valid(entry: &BuildEntry) -> bool {
    build_identity_recorded(entry)
        && release_sidecars_recorded(entry)
        && mainline_registration_valid(entry)
}

pub(super) fn previewable_build(entry: &BuildEntry) -> bool {
    build_identity_recorded(entry)
        && local_release_evidence_valid(entry)
        && release_sidecars_recorded(entry)
        && entry.mainline_ref == product_mainline_ref()
        && product_manifests_valid(entry)
}

fn local_release_evidence_valid(entry: &BuildEntry) -> bool {
    let artifact = entry.slot.join(&entry.artifact);
    let archive = entry.slot.join(&entry.cli_archive);
    let manifest = entry.slot.join(&entry.upgrade_manifest);
    if entry.digest.len() != 64
        || !entry.digest.bytes().all(|byte| byte.is_ascii_hexdigit())
        || artifact_sha256(&artifact).ok().as_deref() != Some(entry.digest.as_str())
        || bootstrap::sha256_file(&archive)
            .ok()
            .map(|digest| format!("sha256:{digest}"))
            .as_deref()
            != Some(entry.cli_archive_digest.as_str())
        || bootstrap::sha256_file(&manifest)
            .ok()
            .map(|digest| format!("sha256:{digest}"))
            .as_deref()
            != Some(entry.upgrade_manifest_digest.as_str())
    {
        return false;
    }
    let Ok(text) = fs::read_to_string(manifest) else {
        return false;
    };
    let Ok(document) = json::parse(&text) else {
        return false;
    };
    let local = document.get("localArtifact");
    document.str_of("schema") == "kungfu.product-upgrade.manifest/v1"
        && document.str_of("releaseCutRoot") == entry.release_cut_root
        && document.str_of("platformSliceRoot") == entry.platform_slice_root
        && local
            .map(|value| local_artifact_identity_valid(&artifact, &entry.digest, value))
            .unwrap_or(false)
}

pub(super) fn content_root_valid(value: &str) -> bool {
    value.len() == 71
        && value.starts_with("sha256:")
        && value[7..]
            .bytes()
            .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
}

fn product_manifests_valid(entry: &BuildEntry) -> bool {
    if entry.kind != "app" {
        return true;
    }
    product_app_manifests_valid(&entry.slot.join(&entry.artifact), &entry.sha).unwrap_or(false)
}
