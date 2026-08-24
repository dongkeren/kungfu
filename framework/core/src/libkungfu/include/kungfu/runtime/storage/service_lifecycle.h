// SPDX-License-Identifier: Apache-2.0

#ifndef KUNGFU_RUNTIME_STORAGE_SERVICE_LIFECYCLE_H
#define KUNGFU_RUNTIME_STORAGE_SERVICE_LIFECYCLE_H

#include <kungfu/runtime/storage/service_transfer.h>

namespace kungfu::runtime::storage_service_api {

struct storage_episode_begin_request {
  std::string runtime_dir = {};
  yijinjing::storage::episode_begin_options options = {};
};

struct storage_episode_heartbeat_request {
  std::string runtime_dir = {};
  yijinjing::storage::episode_heartbeat_options options = {};
};

struct storage_episode_frame_attach_request {
  std::string runtime_dir = {};
  yijinjing::storage::episode_frame_attach_options options = {};
};

struct storage_episode_ref_attach_request {
  std::string runtime_dir = {};
  yijinjing::storage::episode_ref_attach_options options = {};
};

struct storage_episode_close_request {
  std::string runtime_dir = {};
  yijinjing::storage::episode_close_options options = {};
};

struct storage_episode_recover_request {
  std::string runtime_dir = {};
  yijinjing::storage::episode_recover_options options = {};
};

struct storage_episode_projection_rebuild_request {
  std::string runtime_dir = {};
};

struct storage_episode_list_request {
  std::string runtime_dir = {};
  uint32_t location_uid = 0;
  uint64_t limit = 100;
};

struct storage_episode_list_result {
  bool ok = true;
  std::string runtime_dir = {};
  std::string authority = "yijinjing-journal";
  std::vector<yijinjing::storage::episode_current_view> episodes = {};
  uint64_t unknown_record_count = 0;
};

struct storage_episode_inspect_request {
  std::string runtime_dir = {};
  uint64_t episode_id = 0;
};

struct storage_episode_inspect_result {
  bool ok = true;
  std::string runtime_dir = {};
  std::string authority = "yijinjing-journal";
  yijinjing::storage::episode_current_view episode = {};
  yijinjing::storage::episode_content_root_verification content_root = {};
  yijinjing::storage::episode_causal_graph causal_graph = {};
  uint64_t unknown_record_count = 0;
  std::optional<episode_qualification_result> qualification = {};
};

struct storage_source_register_request {
  std::string runtime_dir = {};
  yijinjing::storage::source_register_options options = {};
};

struct storage_source_head_update_request {
  std::string runtime_dir = {};
  yijinjing::storage::source_head_update_options options = {};
};

struct storage_source_accepted_range_request {
  std::string runtime_dir = {};
  yijinjing::storage::accepted_range_options options = {};
};

struct storage_source_list_request {
  std::string runtime_dir = {};
};

struct storage_source_list_result {
  bool ok = true;
  std::string runtime_dir = {};
  std::string authority = "yijinjing-journal";
  std::vector<yijinjing::storage::source_registry_current_view> sources = {};
  uint64_t unknown_record_count = 0;
};

struct storage_source_inspect_request {
  std::string runtime_dir = {};
  std::string source_id = {};
};

struct storage_source_inspect_result {
  bool ok = true;
  std::string runtime_dir = {};
  std::string authority = "yijinjing-journal";
  yijinjing::storage::source_registry_current_view source = {};
  uint64_t unknown_record_count = 0;
};

struct storage_source_registry_fsck_request {
  std::string runtime_dir = {};
  std::string source_id = {};
};

struct storage_source_registry_fsck_result {
  bool ok = true;
  std::string status = "ok";
  yijinjing::storage::source_registry_fsck_result journal = {};
  storage_projection_verify_result projection = {};
};

struct storage_source_registry_rebuild_request {
  std::string runtime_dir = {};
};

} // namespace kungfu::runtime::storage_service_api

#endif // KUNGFU_RUNTIME_STORAGE_SERVICE_LIFECYCLE_H
