// SPDX-License-Identifier: Apache-2.0

#include <kungfu/runtime/storage/json_edge.h>
#include <kungfu/runtime/storage/service.h>

#include "service_internal.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <kungfu/runtime/action_recorder.h>
#include <kungfu/runtime/facts/fact_admission.h>
#include <kungfu/runtime/kfx/native_contract.h>
#include <kungfu/runtime/kfx/native_registry.h>
#include <kungfu/runtime/profile/profile_lifecycle.h>
#include <kungfu/runtime/query/fact_query.h>
#include <kungfu/runtime/query/saved_query_catalog.h>
#include <kungfu/runtime/storage/episode_manifest_projection.h>
#include <kungfu/runtime/storage/manifest_catalog_projection.h>
#include <kungfu/runtime/storage/source_registry_projection.h>
#include <kungfu/runtime/trust/assessment_runtime.h>
#include <kungfu/yijinjing/storage/content_hash.h>
#include <kungfu/yijinjing/storage/content_store.h>
#include <kungfu/yijinjing/storage/episode_manifest.h>
#include <kungfu/yijinjing/storage/manifest_catalog.h>
#include <kungfu/yijinjing/storage/source_registry.h>
#include <kungfu/yijinjing/storage/sync_root.h>
#include <kungfu/yijinjing/time.h>
#include <sqlite3.h>

namespace kungfu::runtime::storage_service_api {

namespace yy_storage = kungfu::yijinjing::storage;
namespace yy_enums = kungfu::yijinjing::enums;

namespace detail {

class file_storage_service : public storage_service {
public:
  [[nodiscard]] storage_status_result status(const storage_status_request &request) const override {
    return status_typed_impl(request);
  }

  [[nodiscard]] storage_layout_result layout(const storage_layout_request &request) const override {
    const auto provider = provider_cache::instance().acquire(request.runtime_dir, request.provider);
    return workspace_episode_layout_typed(request, *provider);
  }

  [[nodiscard]] storage_fsck_result fsck(const storage_fsck_request &request) const override {
    return fsck_typed_impl(request);
  }

  [[nodiscard]] storage_repair_plan_result repair_plan(const storage_repair_plan_request &request) const override {
    return repair_plan_typed_impl(request);
  }

  [[nodiscard]] storage_query_result query(const storage_query_request &request) const override {
    return query_journal_projection(request);
  }

  [[nodiscard]] storage_gc_plan_result gc_plan(const storage_gc_plan_request &request) const override {
    return gc_plan_typed_impl(request);
  }

  [[nodiscard]] storage_rebuild_index_result
  rebuild_index(const storage_rebuild_index_request &request) const override {
    return rebuild_index_typed_impl(request);
  }

  [[nodiscard]] storage_compact_plan_result compact_plan(const storage_compact_plan_request &request) const override {
    return compact_plan_typed_impl(request);
  }

  [[nodiscard]] storage_export_bundle_result
  export_bundle(const storage_export_bundle_request &request) const override {
    return export_bundle_typed_impl(request);
  }

  [[nodiscard]] storage_import_bundle_result
  import_bundle(const storage_import_bundle_request &request) const override {
    return import_bundle_typed_impl(request);
  }

  [[nodiscard]] storage_verify_sync_result verify_sync(const storage_verify_sync_request &request) const override {
    return verify_sync_typed_impl(request);
  }

  [[nodiscard]] yijinjing::types::EpisodeOpen
  episode_begin(const storage_episode_begin_request &request) const override {
    return yy_storage::episode_manifest_store(request.runtime_dir).begin(request.options);
  }

  [[nodiscard]] yijinjing::types::EpisodeHeartbeat
  episode_heartbeat(const storage_episode_heartbeat_request &request) const override {
    return yy_storage::episode_manifest_store(request.runtime_dir).heartbeat(request.options);
  }

  [[nodiscard]] yijinjing::types::EpisodeFrameAttached
  episode_attach_frame(const storage_episode_frame_attach_request &request) const override {
    return yy_storage::episode_manifest_store(request.runtime_dir).attach_frame(request.options);
  }

  [[nodiscard]] yijinjing::types::EpisodeRefAttached
  episode_attach_ref(const storage_episode_ref_attach_request &request) const override {
    return yy_storage::episode_manifest_store(request.runtime_dir).attach_ref(request.options);
  }

  [[nodiscard]] yy_storage::episode_close_write_result
  episode_end(const storage_episode_close_request &request) const override {
    return yy_storage::episode_manifest_store(request.runtime_dir).end(request.options);
  }

  [[nodiscard]] yy_storage::episode_close_write_result
  episode_abort(const storage_episode_close_request &request) const override {
    return yy_storage::episode_manifest_store(request.runtime_dir).abort(request.options);
  }

  [[nodiscard]] yy_storage::episode_recover_result
  episode_recover(const storage_episode_recover_request &request) const override {
    return yy_storage::episode_manifest_store(request.runtime_dir).recover(request.options);
  }

  [[nodiscard]] storage_projection_rebuild_result
  episode_projection_rebuild(const storage_episode_projection_rebuild_request &request) const override {
    return episode_manifest_projection(request.runtime_dir).rebuild_typed();
  }

  [[nodiscard]] storage_episode_list_result episode_list(const storage_episode_list_request &request) const override {
    const auto fold = yy_storage::episode_manifest_store(request.runtime_dir).fold_typed_records();
    storage_episode_list_result result{};
    result.runtime_dir = request.runtime_dir;
    result.unknown_record_count = static_cast<uint64_t>(fold.unknown_record_count);
    for (auto iter = fold.episodes.rbegin(); iter != fold.episodes.rend(); ++iter) {
      const auto &view = iter->second;
      const auto location_uid = view.opened ? view.open.location_uid : uint32_t{0};
      if (request.location_uid != 0 && request.location_uid != location_uid)
        continue;
      result.episodes.push_back(view);
      if (request.limit != 0 && result.episodes.size() >= request.limit)
        break;
    }
    return result;
  }

  [[nodiscard]] storage_episode_inspect_result
  episode_inspect(const storage_episode_inspect_request &request) const override {
    const auto inspected = yy_storage::episode_manifest_store(request.runtime_dir).inspect_typed(request.episode_id);
    storage_fsck_request fsck_request{};
    fsck_request.runtime_dir = request.runtime_dir;
    fsck_request.scope = storage_fsck_scope::Episode;
    fsck_request.episode_id = request.episode_id;
    const auto fsck_result = fsck(fsck_request);
    return {true,
            request.runtime_dir,
            "yijinjing-journal",
            inspected.episode,
            inspected.content_root,
            inspected.causal_graph,
            inspected.unknown_record_count,
            fsck_result.qualification};
  }

  [[nodiscard]] yijinjing::types::SourceRegistered
  source_register(const storage_source_register_request &request) const override {
    return yy_storage::source_registry_store(request.runtime_dir).register_source(request.options);
  }

  [[nodiscard]] yijinjing::types::SourceHeadUpdated
  source_update_head(const storage_source_head_update_request &request) const override {
    return yy_storage::source_registry_store(request.runtime_dir).update_head(request.options);
  }

  [[nodiscard]] yijinjing::types::AcceptedRangeRecorded
  source_record_accepted_range(const storage_source_accepted_range_request &request) const override {
    return yy_storage::source_registry_store(request.runtime_dir).record_accepted_range(request.options);
  }

  [[nodiscard]] storage_source_list_result source_list(const storage_source_list_request &request) const override {
    const auto fold = yy_storage::source_registry_store(request.runtime_dir).fold_typed_records();
    storage_source_list_result result{};
    result.runtime_dir = request.runtime_dir;
    result.unknown_record_count = static_cast<uint64_t>(fold.unknown_record_count);
    result.sources.reserve(fold.sources.size());
    for (const auto &[source_uid, source] : fold.sources) {
      (void)source_uid;
      result.sources.push_back(source);
    }
    return result;
  }

  [[nodiscard]] storage_source_inspect_result
  source_inspect(const storage_source_inspect_request &request) const override {
    const auto store = yy_storage::source_registry_store(request.runtime_dir);
    const auto source = store.inspect_typed(request.source_id);
    if (!source.has_value())
      throw std::invalid_argument("source not found: " + request.source_id);
    const auto fold = store.fold_typed_records();
    return {true, request.runtime_dir, "yijinjing-journal", *source, static_cast<uint64_t>(fold.unknown_record_count)};
  }

  [[nodiscard]] storage_source_registry_fsck_result
  source_registry_fsck(const storage_source_registry_fsck_request &request) const override {
    const auto journal = yy_storage::source_registry_store(request.runtime_dir).fsck_typed(request.source_id);
    const auto projection = source_registry_projection(request.runtime_dir).verify_typed();
    const bool projection_degraded = projection.status == "degraded";
    return {journal.ok && !projection_degraded, !journal.ok ? "failed" : (projection_degraded ? "degraded" : "ok"),
            journal, projection};
  }

  [[nodiscard]] storage_projection_rebuild_result
  source_registry_rebuild(const storage_source_registry_rebuild_request &request) const override {
    return source_registry_projection(request.runtime_dir).rebuild_typed();
  }
};

const storage_service &typed_storage_service_instance() {
  static const file_storage_service service;
  return service;
}

} // namespace detail

} // namespace kungfu::runtime::storage_service_api
