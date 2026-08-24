// SPDX-License-Identifier: Apache-2.0

#ifndef KUNGFU_RUNTIME_STORAGE_SERVICE_QUALIFICATION_H
#define KUNGFU_RUNTIME_STORAGE_SERVICE_QUALIFICATION_H

#include <kungfu/runtime/storage/service_layout.h>

namespace kungfu::runtime::storage_service_api {

struct storage_frame_range_view {
  uint64_t first_frame_uid = 0;
  uint64_t last_frame_uid = 0;
  int64_t since = 0;
  int64_t until = 0;
};

struct storage_source_registry_view {
  uint64_t source_uid = 0;
  std::string source_id = {};
  bool registered = false;
  uint64_t record_count = 0;
  uint64_t accepted_range_count = 0;
  std::optional<std::string> kind = {};
  std::optional<std::string> coordinate = {};
  std::optional<std::string> head = {};
  std::optional<uint32_t> location_uid = {};
  std::optional<int64_t> register_time = {};
  std::optional<storage_frame_range_view> current_range = {};
  std::optional<storage_sync_root_view> inventory_hash = {};
  std::optional<int64_t> update_time = {};
};

struct storage_accepted_range_view {
  std::string source_id = {};
  std::string manifest_id = {};
  storage_time_range range = {};
  std::string source_head = {};
  storage_sync_root_view sync_root = {};
  uint64_t entry_count = 0;
  std::string status = {};
};

struct storage_cursor_view {
  std::string source_id = {};
  std::string manifest_id = {};
  std::string source_head = {};
  storage_time_range range = {};
  storage_sync_root_view sync_root = {};
  uint64_t entry_count = 0;
};

struct storage_manifest_source_view {
  std::string source_id = {};
  std::string source_type = {};
  std::string kind = {};
  std::string coordinate = {};
  std::string source_head = {};
  storage_time_range range = {};
  std::string inventory_hash = {};
  storage_accepted_range_view accepted_range = {};
  std::string manifest_id = {};
};

struct storage_source_status_view {
  std::string source_id = {};
  bool ok = false;
  std::optional<std::string> reason = {};
  storage_source_registry_view source = {};
  std::optional<std::string> manifest_id = {};
  std::optional<std::string> source_type = {};
  std::optional<std::string> source_head = {};
  std::optional<storage_accepted_range_view> accepted_range = {};
  std::optional<storage_cursor_view> accepted_cursor = {};
  std::optional<storage_sync_root_view> sync_root = {};
  uint64_t entries = 0;
  uint64_t payload_inventory = 0;
  uint64_t schema_inventory = 0;
  std::optional<storage_manifest_source_view> source_record = {};
};

struct storage_projection_status_view {
  std::string name = {};
  std::string path = {};
  bool rebuildable = true;
  storage_projection_verify_result verification = {};
};

using episode_frame_field_value = std::variant<int64_t, uint64_t>;

struct episode_frame_field_mismatch {
  std::string field = {};
  episode_frame_field_value claimed = uint64_t{0};
  episode_frame_field_value actual = uint64_t{0};

  friend bool operator==(const episode_frame_field_mismatch &, const episode_frame_field_mismatch &) = default;
};

struct episode_frame_verification_issue {
  std::string code = {};
  uint64_t episode_id = 0;
  uint64_t frame_uid = 0;
  std::optional<uint32_t> location_uid = {};
  std::optional<uint32_t> dest = {};
  std::optional<uint32_t> integrity_version = {};
  std::vector<episode_frame_field_mismatch> fields = {};
  std::optional<uint64_t> claimed_payload_checksum = {};
  std::optional<uint64_t> actual_payload_checksum = {};
  std::optional<uint64_t> claimed_frame_checksum = {};
  std::optional<uint64_t> actual_frame_checksum = {};

  friend bool operator==(const episode_frame_verification_issue &, const episode_frame_verification_issue &) = default;
};

struct episode_frame_verification {
  std::vector<episode_frame_verification_issue> errors = {};
  std::vector<episode_frame_verification_issue> warnings = {};
  uint64_t verified = 0;
  bool degraded = false;
};

struct episode_qualification_evidence {
  std::string name = {};
  std::string state = {};
  std::vector<std::string> issue_codes = {};

  friend bool operator==(const episode_qualification_evidence &, const episode_qualification_evidence &) = default;
};

using episode_qualification_issue_detail =
    std::variant<yijinjing::storage::episode_fsck_issue, episode_frame_verification_issue,
                 storage_projection_verify_result>;

struct episode_qualification_issue {
  std::string severity = {};
  std::string code = {};
  std::string evidence = {};
  episode_qualification_issue_detail detail = yijinjing::storage::episode_fsck_issue{};

  friend bool operator==(const episode_qualification_issue &, const episode_qualification_issue &) = default;
};

struct episode_qualification_capability {
  std::string name = {};
  bool safe = false;
  std::vector<std::string> required_evidence = {};
  std::vector<std::string> blocked_by = {};

  friend bool operator==(const episode_qualification_capability &, const episode_qualification_capability &) = default;
};

struct episode_repair_subject {
  std::optional<uint64_t> episode_id = {};
  std::optional<uint64_t> dependency_episode_id = {};
  std::optional<uint64_t> frame_uid = {};
  std::optional<uint64_t> dependent_frame_uid = {};
  std::optional<std::string> ref_id = {};
  std::optional<std::string> ref_hash = {};
  std::optional<std::string> role = {};

  friend bool operator==(const episode_repair_subject &, const episode_repair_subject &) = default;
};

struct episode_repair_prerequisite {
  std::string issue_code = {};
  std::string action = {};
  std::vector<std::string> required_inputs = {};
  episode_repair_subject subject = {};

  friend bool operator==(const episode_repair_prerequisite &, const episode_repair_prerequisite &) = default;
};

struct episode_qualification_result {
  std::string schema = "kungfu.episode.qualification/v1";
  std::string policy_source = "cpp-typed-fold-fsck";
  uint64_t episode_id = 0;
  std::string lifecycle = "missing";
  std::string status = "failed";
  std::vector<episode_qualification_evidence> evidence = {};
  std::vector<episode_qualification_issue> issues = {};
  std::vector<episode_qualification_capability> capabilities = {};
  std::vector<episode_repair_prerequisite> repair_prerequisites = {};

  friend bool operator==(const episode_qualification_result &, const episode_qualification_result &) = default;
};

} // namespace kungfu::runtime::storage_service_api

#endif // KUNGFU_RUNTIME_STORAGE_SERVICE_QUALIFICATION_H
