// SPDX-License-Identifier: Apache-2.0

use std::fs;
use std::io::{self, Read};
use std::path::{Path, PathBuf};

use shifu_core::json;

pub(crate) struct DesktopCommit {
    pub(crate) installed: PathBuf,
    pub(crate) backup: Option<PathBuf>,
}

fn remove_path(path: &Path) -> std::io::Result<()> {
    let metadata = match fs::symlink_metadata(path) {
        Ok(metadata) => metadata,
        Err(error) if error.kind() == io::ErrorKind::NotFound => return Ok(()),
        Err(error) => return Err(error),
    };
    if metadata.file_type().is_dir() {
        fs::remove_dir_all(path)
    } else {
        fs::remove_file(path)
    }
}

pub(super) fn swap_retained<Identity>(
    installed: &Path,
    rollback: &Path,
    current_identity: &str,
    rollback_identity: &str,
    identity: Identity,
) -> Result<(), String>
where
    Identity: Fn(&Path) -> Result<String, String>,
{
    let staged = rollback.with_extension("shifu-swap-pending");
    let installed_exists = path_exists(installed).map_err(|error| error.to_string())?;
    let rollback_exists = path_exists(rollback).map_err(|error| error.to_string())?;
    let staged_exists = path_exists(&staged).map_err(|error| error.to_string())?;

    if staged_exists && !installed_exists && rollback_exists {
        fs::rename(rollback, installed)
            .map_err(|error| format!("cannot restore retained desktop payload: {error}"))?;
        fs::rename(&staged, rollback)
            .map_err(|error| format!("cannot retain replaced desktop payload: {error}"))?;
    } else if staged_exists && installed_exists && !rollback_exists {
        fs::rename(&staged, rollback)
            .map_err(|error| format!("cannot retain replaced desktop payload: {error}"))?;
    } else if staged_exists {
        return Err(format!(
            "desktop rollback staging is ambiguous: {}",
            staged.display()
        ));
    }

    if !path_exists(installed).map_err(|error| error.to_string())?
        || !path_exists(rollback).map_err(|error| error.to_string())?
    {
        return Err("installed Product has no retained desktop rollback payload".to_string());
    }
    let installed_identity = identity(installed)?;
    let retained_identity = identity(rollback)?;
    if installed_identity == rollback_identity && retained_identity == current_identity {
        return Ok(());
    }
    if installed_identity != current_identity || retained_identity != rollback_identity {
        return Err(
            "retained desktop payloads do not match the exact rollback receipts".to_string(),
        );
    }

    fs::rename(installed, &staged)
        .map_err(|error| format!("cannot stage current desktop for rollback: {error}"))?;
    if let Err(error) = fs::rename(rollback, installed) {
        let _ = fs::rename(&staged, installed);
        return Err(format!("cannot restore retained desktop payload: {error}"));
    }
    fs::rename(&staged, rollback)
        .map_err(|error| format!("cannot retain replaced desktop payload: {error}"))?;
    if identity(installed)? != rollback_identity || identity(rollback)? != current_identity {
        return Err("desktop rollback completed without exact artifact identities".to_string());
    }
    Ok(())
}

fn path_exists(path: &Path) -> io::Result<bool> {
    match fs::symlink_metadata(path) {
        Ok(_) => Ok(true),
        Err(error) if error.kind() == io::ErrorKind::NotFound => Ok(false),
        Err(error) => Err(error),
    }
}

pub(super) fn is_regular_file(path: &Path) -> Result<bool, String> {
    match fs::symlink_metadata(path) {
        Ok(metadata) => Ok(metadata.file_type().is_file()),
        Err(error) if error.kind() == io::ErrorKind::NotFound => Ok(false),
        Err(error) => Err(format!("cannot inspect {}: {error}", path.display())),
    }
}

pub(super) fn parse_document(path: &Path) -> Result<Option<json::Json>, String> {
    let text = match fs::read_to_string(path) {
        Ok(text) => text,
        Err(error) if error.kind() == io::ErrorKind::NotFound => return Ok(None),
        Err(error) => return Err(format!("cannot read {}: {error}", path.display())),
    };
    Ok(json::parse(&text).ok())
}

fn contains_regular_file(path: &Path) -> Result<bool, String> {
    let entries = match fs::read_dir(path) {
        Ok(entries) => entries,
        Err(error) if error.kind() == io::ErrorKind::NotFound => return Ok(false),
        Err(error) => return Err(format!("cannot read {}: {error}", path.display())),
    };
    for entry in entries {
        let entry = entry.map_err(|error| format!("cannot read {}: {error}", path.display()))?;
        if is_regular_file(&entry.path())? {
            return Ok(true);
        }
    }
    Ok(false)
}

pub(super) fn app_manifests_valid(app: &Path, expected_sha: &str) -> Result<bool, String> {
    let resources = app.join("Contents/Resources");
    let runtime = resources.join("kungfu");
    let executable_present = contains_regular_file(&app.join("Contents/MacOS"))?;
    let build = parse_document(&runtime.join("kungfubuildinfo.json"))?;
    let release = parse_document(&resources.join("upgrade/kungfu-release-manifest.json"))?;
    let profiles = parse_document(&runtime.join("profile-kfd3.json"))?;
    let build_revision = build
        .as_ref()
        .and_then(|doc| doc.get("git"))
        .map(|git| git.str_of("revision"))
        .unwrap_or("");
    let release_revision = release
        .as_ref()
        .map(|doc| doc.str_of("sourceCommit"))
        .unwrap_or("");
    let profile_count = profiles
        .as_ref()
        .and_then(|doc| doc.get("entries"))
        .and_then(json::Json::as_array)
        .map(<[_]>::len)
        .unwrap_or(0);
    Ok(executable_present
        && build_revision == expected_sha
        && release_revision == expected_sha
        && profiles
            .as_ref()
            .map(|doc| doc.str_of("schema") == "kungfu.system-profile-kfd3-manifest/v1")
            .unwrap_or(false)
        && profile_count > 0)
}

fn files_equal(left: &Path, right: &Path) -> Result<bool, String> {
    let mut left = fs::File::open(left).map_err(|error| error.to_string())?;
    let mut right = fs::File::open(right).map_err(|error| error.to_string())?;
    let mut left_buffer = [0_u8; 64 * 1024];
    let mut right_buffer = [0_u8; 64 * 1024];
    loop {
        let left_read = left
            .read(&mut left_buffer)
            .map_err(|error| error.to_string())?;
        let right_read = right
            .read(&mut right_buffer)
            .map_err(|error| error.to_string())?;
        if left_read != right_read || left_buffer[..left_read] != right_buffer[..right_read] {
            return Ok(false);
        }
        if left_read == 0 {
            return Ok(true);
        }
    }
}

fn sorted_entries(path: &Path) -> Result<Vec<fs::DirEntry>, String> {
    let mut entries = fs::read_dir(path)
        .map_err(|error| error.to_string())?
        .collect::<Result<Vec<_>, _>>()
        .map_err(|error| error.to_string())?;
    entries.sort_by_key(fs::DirEntry::file_name);
    Ok(entries)
}

fn file_types_exact(source: &fs::Metadata, candidate: &fs::Metadata) -> bool {
    let source_type = source.file_type();
    let candidate_type = candidate.file_type();
    source_type.is_symlink() == candidate_type.is_symlink()
        && source_type.is_dir() == candidate_type.is_dir()
        && source_type.is_file() == candidate_type.is_file()
}

fn permissions_exact(source: &fs::Metadata, candidate: &fs::Metadata) -> bool {
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        source.permissions().mode() & 0o7777 == candidate.permissions().mode() & 0o7777
    }
    #[cfg(not(unix))]
    {
        let _ = (source, candidate);
        true
    }
}

fn symlink_exact(source: &Path, candidate: &Path) -> Result<bool, String> {
    let source_link = fs::read_link(source).map_err(|error| error.to_string())?;
    let candidate_link = fs::read_link(candidate).map_err(|error| error.to_string())?;
    Ok(source_link == candidate_link)
}

fn regular_files_exact(
    source: &Path,
    candidate: &Path,
    source_meta: &fs::Metadata,
    candidate_meta: &fs::Metadata,
) -> Result<bool, String> {
    if source_meta.len() != candidate_meta.len() {
        return Ok(false);
    }
    files_equal(source, candidate)
}

fn directories_exact(source: &Path, candidate: &Path) -> Result<bool, String> {
    let source_entries = sorted_entries(source)?;
    let candidate_entries = sorted_entries(candidate)?;
    if source_entries.len() != candidate_entries.len() {
        return Ok(false);
    }
    for (source_entry, candidate_entry) in source_entries.iter().zip(candidate_entries.iter()) {
        if source_entry.file_name() != candidate_entry.file_name()
            || !exact_tree(&source_entry.path(), &candidate_entry.path())?
        {
            return Ok(false);
        }
    }
    Ok(true)
}

pub(super) fn exact_tree(source: &Path, candidate: &Path) -> Result<bool, String> {
    let source_meta = fs::symlink_metadata(source).map_err(|error| error.to_string())?;
    let candidate_meta = match fs::symlink_metadata(candidate) {
        Ok(metadata) => metadata,
        Err(error) if error.kind() == io::ErrorKind::NotFound => return Ok(false),
        Err(error) => return Err(error.to_string()),
    };
    if !file_types_exact(&source_meta, &candidate_meta)
        || !permissions_exact(&source_meta, &candidate_meta)
    {
        return Ok(false);
    }
    let source_type = source_meta.file_type();
    if source_type.is_symlink() {
        return symlink_exact(source, candidate);
    }
    if source_type.is_file() {
        return regular_files_exact(source, candidate, &source_meta, &candidate_meta);
    }
    if source_type.is_dir() {
        return directories_exact(source, candidate);
    }
    Ok(false)
}

pub(super) fn complete_target<Stage, Verify>(
    source: &Path,
    target: &Path,
    backup: &Path,
    staged: &Path,
    mut stage_source: Stage,
    verify: Verify,
) -> Result<DesktopCommit, String>
where
    Stage: FnMut(&Path, &Path) -> Result<(), String>,
    Verify: Fn(&Path) -> Result<bool, String>,
{
    if verify(target)? {
        remove_path(staged).map_err(|error| {
            format!(
                "cannot discard obsolete staged desktop {}: {error}",
                staged.display()
            )
        })?;
        return Ok(DesktopCommit {
            installed: target.to_path_buf(),
            backup: path_exists(backup)
                .map_err(|error| error.to_string())?
                .then(|| backup.to_path_buf()),
        });
    }
    if path_exists(staged).map_err(|error| error.to_string())? && !verify(staged)? {
        remove_path(staged).map_err(|error| {
            format!(
                "cannot discard incomplete staged desktop {}: {error}",
                staged.display()
            )
        })?;
    }
    if !path_exists(staged).map_err(|error| error.to_string())? {
        stage_source(source, staged)?;
    }
    if !verify(staged)? {
        return Err(format!(
            "staged desktop target failed exact verification: {}",
            staged.display()
        ));
    }
    let backup_exists = path_exists(backup).map_err(|error| error.to_string())?;
    let target_exists = path_exists(target).map_err(|error| error.to_string())?;
    if !backup_exists && target_exists {
        fs::rename(target, backup)
            .map_err(|error| format!("cannot stage previous {}: {error}", target.display()))?;
    } else if backup_exists && target_exists {
        remove_path(target).map_err(|error| {
            format!(
                "cannot discard interrupted desktop target {}: {error}",
                target.display()
            )
        })?;
    }
    fs::rename(staged, target)
        .map_err(|error| format!("cannot place {}: {error}", target.display()))?;
    if !verify(target)? {
        return Err(format!(
            "installed desktop target failed exact verification: {}",
            target.display()
        ));
    }
    Ok(DesktopCommit {
        installed: target.to_path_buf(),
        backup: path_exists(backup)
            .map_err(|error| error.to_string())?
            .then(|| backup.to_path_buf()),
    })
}
