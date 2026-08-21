// SPDX-License-Identifier: Apache-2.0

#ifndef KUNGFU_RUNTIME_STORAGE_SERVICE_FACADE_H
#define KUNGFU_RUNTIME_STORAGE_SERVICE_FACADE_H

#include <kungfu/runtime/storage/service_lifecycle.h>

namespace kungfu::runtime::storage_service_api {

class storage_service {
public:
  virtual ~storage_service() = default;

  [[nodiscard]] virtual storage_status_result status(const storage_status_request &request) const = 0;

  [[nodiscard]] virtual storage_layout_result layout(const storage_layout_request &request) const = 0;

  [[nodiscard]] virtual storage_fsck_result fsck(const storage_fsck_request &request) const = 0;

  [[nodiscard]] virtual storage_repair_plan_result repair_plan(const storage_repair_plan_request &request) const = 0;

  [[nodiscard]] virtual storage_query_result query(const storage_query_request &request) const = 0;

  [[nodiscard]] virtual storage_gc_plan_result gc_plan(const storage_gc_plan_request &request) const = 0;

  [[nodiscard]] virtual storage_rebuild_index_result
  rebuild_index(const storage_rebuild_index_request &request) const = 0;

  [[nodiscard]] virtual storage_compact_plan_result compact_plan(const storage_compact_plan_request &request) const = 0;

  [[nodiscard]] virtual storage_export_bundle_result
  export_bundle(const storage_export_bundle_request &request) const = 0;

  [[nodiscard]] virtual storage_import_bundle_result
  import_bundle(const storage_import_bundle_request &request) const = 0;

  [[nodiscard]] virtual storage_verify_sync_result verify_sync(const storage_verify_sync_request &request) const = 0;

  [[nodiscard]] virtual yijinjing::types::EpisodeOpen
  episode_begin(const storage_episode_begin_request &request) const = 0;

  [[nodiscard]] virtual yijinjing::types::EpisodeHeartbeat
  episode_heartbeat(const storage_episode_heartbeat_request &request) const = 0;

  [[nodiscard]] virtual yijinjing::types::EpisodeFrameAttached
  episode_attach_frame(const storage_episode_frame_attach_request &request) const = 0;

  [[nodiscard]] virtual yijinjing::types::EpisodeRefAttached
  episode_attach_ref(const storage_episode_ref_attach_request &request) const = 0;

  [[nodiscard]] virtual yijinjing::storage::episode_close_write_result
  episode_end(const storage_episode_close_request &request) const = 0;

  [[nodiscard]] virtual yijinjing::storage::episode_close_write_result
  episode_abort(const storage_episode_close_request &request) const = 0;

  [[nodiscard]] virtual yijinjing::storage::episode_recover_result
  episode_recover(const storage_episode_recover_request &request) const = 0;

  [[nodiscard]] virtual storage_projection_rebuild_result
  episode_projection_rebuild(const storage_episode_projection_rebuild_request &request) const = 0;

  [[nodiscard]] virtual storage_episode_list_result episode_list(const storage_episode_list_request &request) const = 0;

  [[nodiscard]] virtual storage_episode_inspect_result
  episode_inspect(const storage_episode_inspect_request &request) const = 0;

  [[nodiscard]] virtual yijinjing::types::SourceRegistered
  source_register(const storage_source_register_request &request) const = 0;

  [[nodiscard]] virtual yijinjing::types::SourceHeadUpdated
  source_update_head(const storage_source_head_update_request &request) const = 0;

  [[nodiscard]] virtual yijinjing::types::AcceptedRangeRecorded
  source_record_accepted_range(const storage_source_accepted_range_request &request) const = 0;

  [[nodiscard]] virtual storage_source_list_result source_list(const storage_source_list_request &request) const = 0;

  [[nodiscard]] virtual storage_source_inspect_result
  source_inspect(const storage_source_inspect_request &request) const = 0;

  [[nodiscard]] virtual storage_source_registry_fsck_result
  source_registry_fsck(const storage_source_registry_fsck_request &request) const = 0;

  [[nodiscard]] virtual storage_projection_rebuild_result
  source_registry_rebuild(const storage_source_registry_rebuild_request &request) const = 0;
};

[[nodiscard]] std::string storage_query_kind_name(storage_query_kind kind);

[[nodiscard]] std::string storage_fsck_scope_name(storage_fsck_scope scope);

[[nodiscard]] storage_query_kind parse_storage_query_kind(const std::string &kind);

[[nodiscard]] const storage_service &default_storage_service();

} // namespace kungfu::runtime::storage_service_api

#endif // KUNGFU_RUNTIME_STORAGE_SERVICE_FACADE_H
