// SPDX-License-Identifier: Apache-2.0

#ifndef KUNGFU_RUNTIME_STORAGE_SERVICE_TRANSFER_H
#define KUNGFU_RUNTIME_STORAGE_SERVICE_TRANSFER_H

#include <kungfu/runtime/storage/service_maintenance.h>

namespace kungfu::runtime::storage_service_api {

struct storage_export_bundle_request {
  std::string runtime_dir = {};
  std::string provider = {};
  std::string source_id = {};
  storage_time_range range = {};
  bool record_receipt = true;
};

struct storage_export_record_view {
  yijinjing::storage::manifest_entry_view entry = {};
  std::optional<std::string> payload_json = {};
};

struct storage_export_bundle_result {
  std::string bundle_id = {};
  std::string source_id = {};
  yijinjing::storage::manifest_document_view manifest = {};
  std::vector<storage_export_record_view> records = {};
};

// KF-ADR-019f86da-4f90-726e-b31f-ed180aa2e7a8: the bytes an Episode owns travel with its bundle. A frame is
// carried whole (header + payload) because the header holds writer-local
// fields (frame_uid, trigger provenance) no claim covers and the claimed
// frame checksum recomputes over exactly these bytes.
struct episode_frame_material {
  uint64_t frame_uid = 0;
  int64_t gen_time = 0;
  int32_t carrier_type = 0;
  uint32_t frame_length = 0;
  uint32_t data_length = 0;
  std::string bytes = {};
};

struct episode_journal_material {
  std::string role = {};
  std::string namespace_ = {};
  std::string name = {};
  std::string mode = {};
  uint32_t seed = 0;
  uint32_t location_uid = 0;
  uint32_t dest = 0;
  std::vector<episode_frame_material> frames = {};
};

struct episode_ref_payload_material {
  std::string content_namespace = "payloads";
  std::string ref_hash = {};
  uint64_t byte_len = 0;
  std::string bytes = {};
};

struct storage_episode_bundle_result {
  std::string bundle_id = {};
  uint64_t episode_id = 0;
  yijinjing::storage::episode_current_view manifest = {};
  yijinjing::storage::episode_causal_graph causal_graph = {};
  bool self_contained = false;
  std::vector<episode_journal_material> journals = {};
  std::vector<episode_ref_payload_material> ref_payloads = {};
  uint64_t material_missing_frame_count = 0;
  uint64_t material_missing_ref_payload_count = 0;
};

struct storage_import_bundle_request {
  std::string runtime_dir = {};
  std::string provider = {};
  storage_export_bundle_result bundle = {};
  bool verify = true;
};

struct storage_import_bundle_result {
  bool ok = true;
  std::string scope = "source";
  std::string source_id = {};
  std::string manifest_id = {};
  uint64_t records = 0;
};

struct storage_verify_sync_request {
  std::string runtime_dir = {};
  std::string provider = {};
  std::string provider_config_source = {};
  std::string source_id = {};
};

struct storage_verify_sync_result {
  bool ok = true;
  std::string scope = "source";
  std::string source_id = {};
  uint64_t exported_records = 0;
  storage_import_bundle_result import = {};
  storage_sync_root_view local_sync_root = {};
  storage_sync_root_view imported_sync_root = {};
  bool sync_roots_match = false;
  storage_fsck_result source_fsck = {};
  storage_fsck_result imported_fsck = {};
  std::string imported_runtime_dir = {};
};

struct storage_backend_inventory_view {
  uint64_t object_count = 0;
  uint64_t byte_count = 0;
  std::string semantic_root = {};
};

struct storage_backend_binding_view {
  bool present = false;
  std::string provider = {};
  std::string previous_provider = {};
  uint64_t generation = 0;
  std::string operation_id = {};
  int64_t committed_at = 0;
  storage_backend_inventory_view inventory = {};
};

struct storage_backend_migration_view {
  bool present = false;
  std::string operation_id = {};
  std::string action = {};
  std::string phase = {};
  std::string source_provider = {};
  std::string target_provider = {};
  uint64_t source_generation = 0;
  uint64_t target_generation = 0;
  uint64_t copied_objects = 0;
  uint64_t copied_bytes = 0;
  int64_t started_at = 0;
  int64_t updated_at = 0;
};

struct storage_backend_status_request {
  std::string runtime_dir = {};
  std::string requested_provider = {};
};

struct storage_backend_status_result {
  bool ok = true;
  std::string schema = "kungfu.storage.backend-status/v1";
  std::string runtime_dir = {};
  std::string provider = {};
  std::string provider_config_source = {};
  storage_backend_binding_view binding = {};
  storage_backend_migration_view migration = {};
  storage_backend_inventory_view inventory = {};
  std::vector<std::string> warnings = {};
};

struct storage_backend_change_request {
  std::string runtime_dir = {};
  std::string target_provider = {};
  std::optional<uint64_t> expected_generation = {};
  // Qualification-only deterministic fault injection. Production callers
  // leave this empty; fixtures use it only under temporary runtime roots.
  std::optional<uint64_t> fail_after_copied_objects = {};
};

struct storage_backend_change_result {
  bool ok = true;
  std::string schema = "kungfu.storage.backend-switch-receipt/v1";
  std::string operation_id = {};
  std::string action = {};
  std::string phase = {};
  std::string source_provider = {};
  std::string target_provider = {};
  std::string source_profile = {};
  std::string target_profile = {};
  uint64_t source_generation = 0;
  uint64_t target_generation = 0;
  storage_backend_inventory_view pre_cut = {};
  storage_backend_inventory_view post_cut = {};
  uint64_t copied_objects = 0;
  uint64_t copied_bytes = 0;
  uint64_t target_extra_objects = 0;
  bool target_fsck_ok = false;
  bool binding_committed = false;
  bool old_backend_retained_readonly = false;
  std::vector<std::string> residual_risks = {};
};

[[nodiscard]] storage_backend_status_result storage_backend_status(const storage_backend_status_request &request);

[[nodiscard]] storage_backend_change_result storage_backend_switch(const storage_backend_change_request &request);

[[nodiscard]] storage_backend_change_result storage_backend_rollback(const storage_backend_change_request &request);

} // namespace kungfu::runtime::storage_service_api

#endif // KUNGFU_RUNTIME_STORAGE_SERVICE_TRANSFER_H
