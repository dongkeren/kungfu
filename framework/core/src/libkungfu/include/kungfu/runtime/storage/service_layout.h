// SPDX-License-Identifier: Apache-2.0

#ifndef KUNGFU_RUNTIME_STORAGE_SERVICE_LAYOUT_H
#define KUNGFU_RUNTIME_STORAGE_SERVICE_LAYOUT_H

#include <kungfu/runtime/storage/service_query.h>

namespace kungfu::runtime::storage_service_api {

struct storage_provider_runtime_view {
  std::string lifecycle = {};
  std::string instance_lifecycle = {};
  std::string handle = {};
  bool readonly_open_creates_backend = false;
  bool write_open_creates_backend = true;
  std::optional<bool> read_fill_cache = {};
  std::optional<bool> write_sync = {};
};

struct storage_provider_layout_view {
  std::optional<std::string> database = {};
  std::string manifest_catalog_journal = {};
  std::string manifest_entries = {};
  std::string payloads = {};
};

struct storage_provider_cache_view {
  std::string lifecycle = "process";
  uint64_t entries = 0;
  uint64_t hits = 0;
  uint64_t misses = 0;
};

struct storage_layout_request {
  std::string runtime_dir = {};
  std::string runtime_home = {};
  std::string config_home = {};
  std::string provider = {};
};

struct storage_layout_paths_view {
  std::string data_home = {};
  std::string workspace_ignore = {};
  std::string workspace_config = {};
  std::string extensions_dir = {};
  std::string runtime_dir = {};
  std::string dataset_dir = {};
  std::string inbox_dir = {};
  std::string backtest_dir = {};
  std::string sealed_episodes_dir = {};
  std::string project_cuts_dir = {};
  std::string journal_dir = {};
  std::string db_dir = {};
  std::string nn_dir = {};
  std::string map_dir = {};
  std::string log_dir = {};
  std::string ownership_dir = {};
  std::string coordinator_dir = {};
  std::string skill_manager_dir = {};
  std::string agent_session_dir = {};
  std::string skill_context_dir = {};
  std::string project_cut_runtime_dir = {};
  std::string sources_dir = {};
  std::string peers_dir = {};
  std::string coordination_dir = {};
  std::string admission_dir = {};
  std::string fact_durable_admission_dir = {};
  std::string receipts_dir = {};
  std::string legacy_master_dir = {};
  std::string storage_dir = {};
  std::string source_registry_journal = {};
  std::string manifest_catalog_journal = {};
  std::string manifest_entries = {};
  std::string payloads = {};
  std::string schemas = {};
  std::string rocksdb = {};
  std::string backend_binding = {};
  std::string backend_switch_state = {};
  std::string backend_switch_receipts = {};
  std::string backend_switch_operation_lock = {};
  std::string backend_authority_lock = {};
  std::string source_registry_projection = {};
  std::string manifest_catalog_projection = {};
  std::string episode_manifest_journal_dir = {};
  std::string episode_manifest_journal = {};
  std::string coordinator_state = {};
  std::string remote_mirrors = {};
  std::string atlas_store = {};
};

struct storage_layout_entry_view {
  std::string id = {};
  std::string path = {};
  std::string persistence = {};
  std::string authority = {};
};

struct storage_layout_coverage_view {
  bool complete = true;
  std::vector<std::string> checked_roots = {};
  std::vector<std::string> unclassified_durable_candidates = {};
};

struct storage_layout_episode_view {
  std::string authority = "yijinjing-journal";
  std::string schema = {};
  std::string manifest_namespace = {};
  std::string manifest_name = {};
  std::string manifest_journal = {};
  std::vector<std::string> query_tables = {};
  std::string export_schema = {};
};

struct storage_layout_ownership_view {
  std::string journal_dir = {};
  std::string episode_manifest_journal = {};
  std::string storage_dir = {};
  std::string source_registry_journal = {};
  std::string manifest_catalog_journal = {};
  std::string manifest_entries = {};
  std::string payloads = {};
  std::string source_registry_projection = {};
  std::string manifest_catalog_projection = {};
  std::string rocksdb = {};
  std::string config_home = {};
};

struct storage_layout_result {
  std::string schema = "kungfu.workspace.episode-layout/v1";
  std::string owner = RUNTIME_STORAGE_SERVICE_OWNER;
  uint32_t layout_version = 1;
  std::string runtime_home = {};
  std::string workspace_data_home = {};
  std::string runtime_home_source = {};
  std::string runtime_dir = {};
  bool runtime_dir_is_standard_child = false;
  std::string config_home = {};
  std::string provider = {};
  storage_provider_layout_view provider_layout = {};
  storage_provider_runtime_view provider_runtime = {};
  storage_provider_cache_view provider_cache = {};
  storage_layout_paths_view paths = {};
  std::vector<storage_layout_entry_view> entries = {};
  storage_layout_coverage_view coverage = {};
  storage_layout_episode_view episodes = {};
  storage_layout_ownership_view ownership = {};
  std::vector<std::string> notes = {};
};

} // namespace kungfu::runtime::storage_service_api

#endif // KUNGFU_RUNTIME_STORAGE_SERVICE_LAYOUT_H
