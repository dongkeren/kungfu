// SPDX-License-Identifier: Apache-2.0

#ifndef KUNGFU_RUNTIME_STORAGE_SERVICE_MAINTENANCE_H
#define KUNGFU_RUNTIME_STORAGE_SERVICE_MAINTENANCE_H

#include <kungfu/runtime/storage/service_qualification.h>

namespace kungfu::runtime::storage_service_api {

enum class storage_fsck_scope { All, Source, Episode };

struct storage_fsck_request {
  std::string runtime_dir = {};
  std::string provider = {};
  std::string provider_config_source = {};
  storage_fsck_scope scope = storage_fsck_scope::All;
  std::string source_id = {};
  uint64_t episode_id = 0;
  bool verify_frames = false;
};

struct storage_fsck_cross_issue {
  std::optional<std::string> source_id = {};
  std::optional<std::string> path = {};
  std::optional<std::string> payload_hash = {};
  std::optional<std::string> expected = {};
  std::optional<std::string> actual = {};
  std::optional<std::string> reason = {};
};

using storage_fsck_issue_detail =
    std::variant<storage_fsck_cross_issue, yijinjing::storage::source_registry_fsck_issue,
                 yijinjing::storage::manifest_catalog_fsck_issue, yijinjing::storage::episode_fsck_issue,
                 episode_frame_verification_issue, storage_projection_status_view>;

struct storage_fsck_issue {
  std::string severity = "error";
  std::string code = {};
  std::string projection = {};
  storage_fsck_issue_detail detail = storage_fsck_cross_issue{};
};

struct storage_fsck_counts {
  uint64_t sources = 0;
  uint64_t manifests = 0;
  uint64_t manifest_entries = 0;
  uint64_t payloads = 0;
  uint64_t entries_documents = 0;
  uint64_t accepted_ranges = 0;
  uint64_t source_records = 0;
  uint64_t projection_indexes = 0;
  uint64_t orphan_payloads = 0;
  uint64_t episode_manifest_records = 0;
  uint64_t episodes = 0;
  uint64_t episode_frames_verified = 0;
};

struct storage_fsck_result {
  bool ok = true;
  bool degraded = false;
  std::string status = "ok";
  storage_fsck_scope scope = storage_fsck_scope::All;
  std::optional<std::string> source_id = {};
  std::optional<uint64_t> episode_id = {};
  std::string authority = "yijinjing-journal";
  storage_fsck_counts checked = {};
  yijinjing::storage::source_registry_fsck_result source_registry = {};
  std::optional<yijinjing::storage::manifest_catalog_fsck_result> manifest_catalog = {};
  yijinjing::storage::episode_fsck_result episode_manifest = {};
  std::vector<storage_projection_status_view> projections = {};
  std::optional<episode_frame_verification> frame_verification = {};
  std::optional<episode_qualification_result> qualification = {};
  std::vector<storage_fsck_issue> issues = {};
};

struct storage_repair_plan_request {
  std::string runtime_dir = {};
  std::string provider = {};
  std::string provider_config_source = {};
  storage_fsck_scope scope = storage_fsck_scope::All;
  std::string source_id = {};
  uint64_t episode_id = 0;
  bool verify_frames = false;
  bool dry_run = true;
};

struct storage_repair_subject {
  std::optional<uint64_t> episode_id = {};
  std::optional<uint64_t> dependency_episode_id = {};
  std::optional<uint64_t> frame_uid = {};
  std::optional<uint64_t> dependent_frame_uid = {};
  std::optional<std::string> ref_id = {};
  std::optional<std::string> ref_hash = {};
  std::optional<std::string> source_id = {};
  std::optional<std::string> subject = {};
  std::optional<std::string> state = {};
  std::optional<std::string> path = {};
  std::optional<std::string> payload_hash = {};
};

struct storage_repair_candidate_view {
  std::string code = {};
  std::string issue_code = {};
  std::string kind = {};
  std::string role = {};
  std::string action = {};
  bool safe_to_apply = false;
  std::vector<std::string> required_inputs = {};
  storage_repair_subject subject = {};
  storage_fsck_issue issue = {};
};

struct storage_repair_plan_result {
  bool ok = true;
  storage_fsck_scope scope = storage_fsck_scope::All;
  std::optional<std::string> source_id = {};
  std::optional<uint64_t> episode_id = {};
  bool dry_run = true;
  bool plan_only = true;
  std::string status = "ok";
  bool degraded = false;
  std::vector<storage_repair_candidate_view> candidates = {};
  std::vector<storage_fsck_issue> unsupported = {};
  storage_fsck_result fsck = {};
  std::vector<std::string> notes = {};
};

struct storage_status_request {
  std::string runtime_dir = {};
  std::string provider = {};
  std::string provider_config_source = {};
  std::string source_id = {};
};

struct storage_status_result {
  bool ok = true;
  std::string backend = {};
  std::string provider = {};
  std::string provider_config_source = {};
  storage_provider_runtime_view provider_runtime = {};
  storage_provider_cache_view provider_cache = {};
  std::string scope = "all";
  std::optional<std::string> source_id = {};
  std::string authority = "yijinjing-journal";
  std::vector<storage_source_registry_view> sources = {};
  std::vector<storage_projection_status_view> projections = {};
  std::vector<storage_source_status_view> source_status = {};
};

struct storage_gc_plan_request {
  std::string runtime_dir = {};
  std::string provider = {};
  std::string source_id = {};
  bool dry_run = true;
};

struct storage_gc_candidate_view {
  std::string payload_hash = {};
  std::string uri = {};
  uint64_t bytes = 0;
  bool safe_to_delete = false;
};

struct storage_gc_plan_result {
  bool ok = true;
  std::string scope = "all";
  std::optional<std::string> source_id = {};
  bool dry_run = true;
  uint64_t payloads_scanned = 0;
  uint64_t referenced_payloads = 0;
  uint64_t candidate_bytes = 0;
  std::vector<storage_gc_candidate_view> candidates = {};
  std::vector<std::string> notes = {};
};

using storage_projection_action_detail =
    std::variant<storage_projection_verify_result, storage_projection_rebuild_result>;

struct storage_projection_action_view {
  std::string name = {};
  bool dry_run = true;
  bool written = false;
  bool would_write = false;
  storage_projection_action_detail detail = storage_projection_verify_result{};
};

struct storage_rebuild_index_request {
  std::string runtime_dir = {};
  std::string source_id = {};
  bool dry_run = true;
};

struct storage_projection_error {
  std::string code = {};
  std::optional<std::string> projection = {};
  std::optional<std::string> source_id = {};
};

struct storage_rebuild_index_result {
  bool ok = true;
  std::string scope = "all";
  std::optional<std::string> source_id = {};
  std::string authority = "yijinjing-journal";
  std::string rebuilt_from = "storage kernel journals";
  std::vector<storage_projection_action_view> projections = {};
  bool dry_run = true;
  bool would_write = false;
  bool written = false;
  uint64_t sources_rebuilt = 0;
  std::vector<storage_projection_error> errors = {};
};

struct storage_compact_plan_request {
  std::string runtime_dir = {};
  std::string provider = {};
  std::string source_id = {};
  bool dry_run = true;
};

struct storage_retained_manifest_view {
  std::string source_id = {};
  std::string manifest_id = {};
  uint64_t entries = 0;
  storage_sync_root_view sync_root = {};
};

struct storage_projection_compact_view {
  std::string name = {};
  std::string path = {};
  std::string action = "rebuild-and-vacuum";
  bool dry_run = true;
  bool rebuildable = true;
};

struct storage_unsupported_action_view {
  std::string name = {};
  std::string reason = {};
};

struct storage_compact_plan_result {
  bool ok = true;
  std::string scope = "all";
  std::optional<std::string> source_id = {};
  bool dry_run = true;
  std::vector<storage_retained_manifest_view> retained_manifests = {};
  storage_rebuild_index_result rebuild_index = {};
  storage_gc_plan_result gc = {};
  storage_projection_compact_view projection_compact = {};
  std::vector<storage_unsupported_action_view> unsupported = {};
  std::vector<std::string> notes = {};
};

} // namespace kungfu::runtime::storage_service_api

#endif // KUNGFU_RUNTIME_STORAGE_SERVICE_MAINTENANCE_H
