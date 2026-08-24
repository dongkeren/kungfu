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

const char *episode_status_text(yy_enums::EpisodeStatus status) {
  switch (status) {
  case yy_enums::EpisodeStatus::Open:
    return "open";
  case yy_enums::EpisodeStatus::Ended:
    return "ended";
  case yy_enums::EpisodeStatus::Aborted:
    return "aborted";
  case yy_enums::EpisodeStatus::Tombstoned:
    return "tombstoned";
  }
  return "unknown";
}

const char *episode_ref_kind_text(yy_enums::EpisodeRefKind kind) {
  switch (kind) {
  case yy_enums::EpisodeRefKind::InputFrame:
    return "input_frame";
  case yy_enums::EpisodeRefKind::Payload:
    return "payload";
  case yy_enums::EpisodeRefKind::Schema:
    return "schema";
  case yy_enums::EpisodeRefKind::Episode:
    return "episode";
  }
  return "unknown";
}

template <typename T> nlohmann::json episode_base_record_json(const char *kind, const T &record) {
  return {{"schema", yy_storage::EPISODE_MANIFEST_SCHEMA_V1},
          {"record_kind", kind},
          {"schema_version", record.schema_version},
          {"episode_id", record.episode_id},
          {"location_uid", record.location_uid}};
}

nlohmann::json episode_record_body_json(const yijinjing::types::EpisodeOpen &record) {
  auto row = episode_base_record_json("episode_open", record);
  row["status"] = episode_status_text(yy_enums::EpisodeStatus::Open);
  row["parent_episode_id"] = record.parent_episode_id;
  row["root_trigger_frame_uid"] = record.root_trigger_frame_uid;
  row["begin_time"] = record.begin_time;
  row["title"] = fixed_string(record.title);
  row["actor"] = fixed_string(record.actor);
  row["source"] = fixed_string(record.source);
  return row;
}

nlohmann::json episode_record_body_json(const yijinjing::types::EpisodeHeartbeat &record) {
  auto row = episode_base_record_json("episode_heartbeat", record);
  row["update_time"] = record.update_time;
  row["last_frame_uid"] = record.last_frame_uid;
  row["frame_count"] = record.frame_count;
  row["note"] = fixed_string(record.note);
  return row;
}

nlohmann::json episode_record_body_json(const yijinjing::types::EpisodeFrameAttached &record) {
  auto row = episode_base_record_json("episode_frame_attached", record);
  row["frame_uid"] = record.frame_uid;
  row["trigger_frame_uid"] = record.trigger_frame_uid;
  row["stream_id"] = record.stream_id;
  row["gen_time"] = record.gen_time;
  row["trigger_time"] = record.trigger_time;
  row["carrier_type"] = record.carrier_type;
  row["source"] = record.source;
  row["dest"] = record.dest;
  row["data_length"] = record.data_length;
  row["integrity_version"] = record.integrity_version;
  row["payload_checksum"] = record.payload_checksum;
  row["frame_checksum"] = record.frame_checksum;
  return row;
}

nlohmann::json episode_record_body_json(const yijinjing::types::EpisodeRefAttached &record) {
  auto row = episode_base_record_json("episode_ref_attached", record);
  row["ref_kind"] = episode_ref_kind_text(record.ref_kind);
  row["ref_uid"] = record.ref_uid;
  row["update_time"] = record.update_time;
  row["ref_id"] = fixed_string(record.ref_id);
  row["ref_hash"] = fixed_string(record.ref_hash);
  return row;
}

nlohmann::json episode_record_body_json(const yijinjing::types::EpisodeClosed &record) {
  auto row = episode_base_record_json("episode_closed", record);
  row["status"] = episode_status_text(record.status);
  row["end_time"] = record.end_time;
  row["last_frame_uid"] = record.last_frame_uid;
  row["frame_count"] = record.frame_count;
  row["reason"] = fixed_string(record.reason);
  return row;
}

nlohmann::json episode_record_body_json(const yijinjing::types::EpisodeRootCommitted &record) {
  auto row = episode_base_record_json("episode_root_committed", record);
  row["commit_time"] = record.commit_time;
  row["covered_record_count"] = record.covered_record_count;
  row["algorithm"] = fixed_string(record.algorithm);
  row["root_value"] = fixed_string(record.root_value);
  return row;
}

nlohmann::json render_episode_close_write_result(const yy_storage::episode_close_write_result &result) {
  auto rendered = episode_record_body_json(result.close);
  if (result.content_root.has_value()) {
    rendered["content_root"] = episode_record_body_json(*result.content_root);
  }
  return rendered;
}

nlohmann::json render_episode_recover_result(const yy_storage::episode_recover_result &result) {
  nlohmann::json recovered = nlohmann::json::array();
  for (const auto &item : result.recovered)
    recovered.push_back(render_episode_close_write_result(item));
  nlohmann::json skipped = nlohmann::json::array();
  for (const auto &item : result.skipped_open)
    skipped.push_back({{"episode_id", item.episode_id}, {"location_uid", item.location_uid}});
  return {{"ok", true},
          {"schema", yy_storage::EPISODE_MANIFEST_SCHEMA_V1},
          {"runtime_dir", result.runtime_dir},
          {"authority", "yijinjing-journal"},
          {"recovered", std::move(recovered)},
          {"recovered_count", result.recovered.size()},
          {"skipped_open", std::move(skipped)},
          {"skipped_count", result.skipped_open.size()}};
}

nlohmann::json episode_record_row_json(const yy_storage::episode_manifest_record &record) {
  auto row = std::visit(
      [&record](const auto &body) -> nlohmann::json {
        using body_t = std::decay_t<decltype(body)>;
        if constexpr (std::is_same_v<body_t, yy_storage::episode_manifest_unknown_record>) {
          return {{"schema", yy_storage::EPISODE_MANIFEST_SCHEMA_V1},
                  {"record_kind", "unknown"},
                  {"carrier_type", body.carrier_type},
                  {"frame_uid", record.manifest_frame_uid},
                  {"gen_time", record.manifest_gen_time}};
        } else {
          return episode_record_body_json(body);
        }
      },
      record.body);
  row["manifest_frame_uid"] = record.manifest_frame_uid;
  row["manifest_gen_time"] = record.manifest_gen_time;
  return row;
}

nlohmann::json render_storage_episode_inspect_records(const storage_episode_inspect_result &result) {
  nlohmann::json records = nlohmann::json::array();
  for (const auto &record : result.episode.records)
    records.push_back(episode_record_row_json(record));
  return records;
}

nlohmann::json episode_dependency_json(const yy_storage::episode_dependency &dependency) {
  nlohmann::json rendered = {{"kind", dependency.kind}, {"role", dependency.role}, {"status", dependency.status}};
  if (dependency.episode_id.has_value())
    rendered["episode_id"] = *dependency.episode_id;
  if (dependency.frame_uid.has_value())
    rendered["frame_uid"] = *dependency.frame_uid;
  if (dependency.dependent_frame_uid.has_value())
    rendered["dependent_frame_uid"] = *dependency.dependent_frame_uid;
  if (dependency.ref_uid.has_value())
    rendered["ref_uid"] = *dependency.ref_uid;
  if (dependency.ref_id.has_value())
    rendered["ref_id"] = *dependency.ref_id;
  if (dependency.ref_hash.has_value())
    rendered["ref_hash"] = *dependency.ref_hash;
  return rendered;
}

nlohmann::json episode_dependencies_json(const yy_storage::episode_causal_graph &graph) {
  nlohmann::json rendered = nlohmann::json::array();
  for (const auto &dependency : graph.dependencies)
    rendered.push_back(episode_dependency_json(dependency));
  return rendered;
}

nlohmann::json episode_causal_graph_json(const yy_storage::episode_causal_graph &graph) {
  nlohmann::json edges = nlohmann::json::array();
  for (const auto &edge : graph.edges) {
    edges.push_back({{"kind", "frame_trigger"},
                     {"scope", "internal"},
                     {"from_frame_uid", edge.from_frame_uid},
                     {"to_frame_uid", edge.to_frame_uid}});
  }
  return {{"schema", graph.schema},
          {"episode_id", graph.episode_id},
          {"frame_count", graph.frame_count},
          {"edge_count", graph.edges.size()},
          {"dependency_count", graph.dependencies.size()},
          {"degraded", graph.degraded},
          {"edges", std::move(edges)},
          {"dependencies", episode_dependencies_json(graph)}};
}

nlohmann::json render_storage_episode_bundle_result(const storage_episode_bundle_result &result) {
  nlohmann::json records = nlohmann::json::array();
  for (const auto &record : result.manifest.records)
    records.push_back(episode_record_row_json(record));
  nlohmann::json frames = nlohmann::json::array();
  for (const auto index : result.manifest.frame_indices)
    frames.push_back(episode_record_row_json(result.manifest.records.at(index)));
  nlohmann::json refs = nlohmann::json::array();
  for (const auto index : result.manifest.ref_indices)
    refs.push_back(episode_record_row_json(result.manifest.records.at(index)));
  const auto dependencies = episode_dependencies_json(result.causal_graph);
  nlohmann::json rendered = {{"schema", "kungfu.storage.episode-bundle/v1"},
                             {"bundle_id", result.bundle_id},
                             {"scope", "episode"},
                             {"episode_id", result.episode_id},
                             {"authority", "yijinjing-journal"},
                             {"manifest", yy_storage::episode_summary_json(result.manifest)},
                             {"causal_graph", episode_causal_graph_json(result.causal_graph)},
                             {"records", std::move(records)},
                             {"frames", std::move(frames)},
                             {"refs", std::move(refs)},
                             {"dependencies", dependencies},
                             {"degraded", result.causal_graph.degraded},
                             {"record_count", result.manifest.records.size()},
                             {"frame_count", result.manifest.frame_indices.size()},
                             {"ref_count", result.manifest.ref_indices.size()},
                             {"dependency_count", result.causal_graph.dependencies.size()}};
  if (!result.self_contained) {
    return rendered;
  }
  // KF-ADR-019f86da-4f90-726e-b31f-ed180aa2e7a8: bundle-owned bytes use base64 only at the JSON edge.
  rendered["self_contained"] = true;
  nlohmann::json journals = nlohmann::json::array();
  for (const auto &journal : result.journals) {
    nlohmann::json frame_rows = nlohmann::json::array();
    for (const auto &frame : journal.frames) {
      frame_rows.push_back({{"frame_uid", frame.frame_uid},
                            {"gen_time", frame.gen_time},
                            {"carrier_type", frame.carrier_type},
                            {"frame_length", frame.frame_length},
                            {"data_length", frame.data_length},
                            {"bytes", base64_encode(frame.bytes)}});
    }
    journals.push_back({{"location",
                         {{"role", journal.role},
                          {"namespace", journal.namespace_},
                          {"name", journal.name},
                          {"mode", journal.mode},
                          {"seed", journal.seed},
                          {"uid", journal.location_uid}}},
                        {"dest", journal.dest},
                        {"frames", std::move(frame_rows)}});
  }
  rendered["journals"] = std::move(journals);
  nlohmann::json ref_payloads = nlohmann::json::array();
  for (const auto &payload : result.ref_payloads) {
    ref_payloads.push_back({{"content_namespace", payload.content_namespace},
                            {"ref_hash", payload.ref_hash},
                            {"byte_len", payload.byte_len},
                            {"bytes", base64_encode(payload.bytes)}});
  }
  rendered["ref_payloads"] = std::move(ref_payloads);
  rendered["material"] = {{"missing_frame_count", result.material_missing_frame_count},
                          {"missing_ref_payload_count", result.material_missing_ref_payload_count}};
  return rendered;
}

} // namespace detail

} // namespace kungfu::runtime::storage_service_api
