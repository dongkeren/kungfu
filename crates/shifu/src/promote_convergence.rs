// SPDX-License-Identifier: Apache-2.0

//! Exact receipt-only convergence from an installed preview to promotion.

use std::fs;
use std::path::{Path, PathBuf};

use shifu_core::{json, style};

use crate::artifact_catalog::{write_promotion_receipt, GitRelation};
use crate::native_update::{self, artifact_sha256};

use super::{
    exact_receipt_updater, launch_product, receipt_value, rollback_entry_valid, BuildEntry,
    PromotionLock,
};

pub(super) struct PreviewPromotionConvergence {
    pub(super) installed: PathBuf,
    pub(super) from_sha: String,
    pub(super) relation: GitRelation,
}

fn relation_from_receipt(value: &str) -> Option<GitRelation> {
    match value {
        "same" => Some(GitRelation::Same),
        "descendant" => Some(GitRelation::Descendant),
        "ancestor" => Some(GitRelation::Ancestor),
        "diverged" => Some(GitRelation::Diverged),
        "unknown" => Some(GitRelation::Unknown),
        _ => None,
    }
}

pub(super) fn inspect_at(
    entry: &BuildEntry,
    registry: &Path,
) -> Result<Option<PreviewPromotionConvergence>, String> {
    let installed_receipt = match fs::read_to_string(registry.join("installed.meta.env")) {
        Ok(value) => value,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => return Ok(None),
        Err(error) => return Err(format!("cannot inspect installed Product receipt: {error}")),
    };
    if receipt_value(&installed_receipt, "KUNGFU_INSTALLED_SHA") != entry.sha
        || receipt_value(&installed_receipt, "KUNGFU_INSTALLED_BUILD_ID") != entry.name
    {
        return Ok(None);
    }
    for (key, expected) in [
        ("KUNGFU_INSTALLED_DIGEST", entry.digest.as_str()),
        (
            "KUNGFU_INSTALLED_PRODUCT_VERSION",
            entry.product_version.as_str(),
        ),
        (
            "KUNGFU_INSTALLED_RELEASE_CUT_ROOT",
            entry.release_cut_root.as_str(),
        ),
        (
            "KUNGFU_INSTALLED_PLATFORM_SLICE_ROOT",
            entry.platform_slice_root.as_str(),
        ),
        ("KUNGFU_INSTALLED_MAINLINE_SHA", entry.mainline_sha.as_str()),
    ] {
        if receipt_value(&installed_receipt, key) != expected {
            return Err(format!(
                "installed Product {key} differs from the exact preview target"
            ));
        }
    }
    if receipt_value(&installed_receipt, "KUNGFU_INSTALLED_MODE") != "qualified"
        || receipt_value(&installed_receipt, "KUNGFU_INSTALLED_INTEGRATED") != "true"
        || receipt_value(&installed_receipt, "KUNGFU_INSTALLED_QUALIFIED") != "true"
    {
        return Err("installed preview is not a qualified integrated Product".to_string());
    }
    let installed = PathBuf::from(receipt_value(
        &installed_receipt,
        "KUNGFU_INSTALLED_ARTIFACT",
    ));
    if !installed.exists() || artifact_sha256(&installed)? != entry.digest {
        return Err("installed preview desktop differs from its exact receipt".to_string());
    }
    let from_build_id = receipt_value(&installed_receipt, "KUNGFU_ROLLBACK_BUILD_ID");
    let from_sha = receipt_value(&installed_receipt, "KUNGFU_ROLLBACK_SHA");
    if !rollback_entry_valid(registry, &from_build_id, &from_sha) {
        return Err("installed preview has no exact retained rollback coordinate".to_string());
    }
    let promotion_text = match fs::read_to_string(registry.join("last-promotion.json")) {
        Ok(value) => value,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => return Ok(None),
        Err(error) => return Err(format!("cannot inspect preview promotion receipt: {error}")),
    };
    let promotion = json::parse(&promotion_text)
        .map_err(|error| format!("preview promotion receipt is invalid: {error}"))?;
    if promotion.str_of("action") != "preview" {
        return Ok(None);
    }
    if promotion.str_of("schema") != "shifu.local-promotion-receipt/v1"
        || promotion.str_of("product") != "kungfu"
        || promotion.str_of("artifactId") != entry.name
        || promotion.str_of("fromCommit") != from_sha
        || promotion.str_of("toCommit") != entry.sha
    {
        return Err("preview promotion receipt differs from the installed Product".to_string());
    }
    let relation = relation_from_receipt(promotion.str_of("relation"))
        .ok_or_else(|| "preview promotion receipt has an unknown Git relation".to_string())?;
    let updater = exact_receipt_updater(&installed_receipt)?;
    let selected = native_update::selected_release_cut(&updater)?;
    if selected.release_cut_root != entry.release_cut_root
        || selected.receipt_root
            != receipt_value(&installed_receipt, "KUNGFU_INSTALLED_NATIVE_RECEIPT_ROOT")
    {
        return Err("native selection differs from the exact installed preview".to_string());
    }
    Ok(Some(PreviewPromotionConvergence {
        installed,
        from_sha,
        relation,
    }))
}

pub(super) fn release_lock(lock: &mut Option<PromotionLock>) -> Result<(), String> {
    match lock.take() {
        Some(value) => value.release(),
        None => Ok(()),
    }
}

pub(super) fn try_finish(
    entry: &BuildEntry,
    registry: &Path,
    current_sha: &str,
    check: bool,
    launch: bool,
    lock: &mut Option<PromotionLock>,
) -> Result<bool, String> {
    let convergence = match inspect_at(entry, registry) {
        Ok(Some(value)) => value,
        Ok(None) => return Ok(false),
        Err(error) => {
            release_lock(lock)?;
            return Err(format!(
                "exact preview cannot converge to promotion: {error}"
            ));
        }
    };
    if check {
        println!(
            "{{\"schema\":\"shifu.local-promotion-plan/v1\",\"ok\":true,\
             \"action\":\"promote\",\"artifactId\":\"{}\",\
             \"sourceCommit\":\"{}\",\"currentCommit\":\"{}\",\
             \"targetReleaseCutRoot\":\"{}\",\"receiptOnly\":true,\
             \"wouldWrite\":false}}",
            super::json_escape(&entry.name),
            super::json_escape(&entry.sha),
            super::json_escape(current_sha),
            super::json_escape(&entry.release_cut_root),
        );
        return Ok(true);
    }
    if let Err(error) = write_promotion_receipt(
        registry,
        "kungfu",
        "promote",
        &entry.name,
        &convergence.from_sha,
        &entry.sha,
        convergence.relation,
    ) {
        release_lock(lock)?;
        return Err(format!(
            "cannot converge exact preview promotion receipt: {error}"
        ));
    }
    release_lock(lock)?;
    eprintln!(
        "\u{2705} {} {}",
        style::green("promoted"),
        style::bold(&convergence.installed.display().to_string())
    );
    if launch {
        launch_product(&convergence.installed);
    }
    Ok(true)
}
