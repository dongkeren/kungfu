// SPDX-License-Identifier: Apache-2.0

#ifndef KUNGFU_RUNTIME_KFX_NATIVE_REGISTRY_INTERNAL_H
#define KUNGFU_RUNTIME_KFX_NATIVE_REGISTRY_INTERNAL_H

#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace kungfu::runtime::kfx::native_registry_internal {

using json = nlohmann::json;

inline constexpr const char *KFX_REGISTRY_REF = "profiles/kfx/registry";
inline constexpr const char *KFX_PROFILE_ID = "kungfu-kfx-domain-profile";
inline constexpr const char *KFX_CONTROL_SUITE_ID = "kungfu-kfx-control-suite";
inline constexpr const char *KFX_CONTROL_PACKAGE_KEY = "kfx-manager";

struct snapshot {
  json packages = json::array();
  json suites = json::array();
  json diagnostics = json::array();
  json graph = json::object();
  std::string registry_root;
  std::string graph_root;
};

struct lifecycle_view {
  bool present = false;
  uint64_t revision = 0;
  std::string cut_root;
  json cut = json::object();
  snapshot authoritative;
  std::map<std::string, std::string> desired_states;
  std::map<std::string, std::string> observed_states;
  json work_history = json::array();
  std::map<std::string, std::string> current_versions;
  std::set<std::string> relation_roots;
};

class lifecycle_writer_lock {
public:
  explicit lifecycle_writer_lock(const std::string &runtime_dir);
  lifecycle_writer_lock(const lifecycle_writer_lock &) = delete;
  lifecycle_writer_lock &operator=(const lifecycle_writer_lock &) = delete;
  ~lifecycle_writer_lock();

private:
  std::filesystem::path path_;
  bool held_ = false;
};

[[noreturn]] void refuse(const std::string &code, const std::string &message);
std::string sha256(const std::string &value);
std::string root_of(const json &value);
json embedded_domain_profile();
std::string required_text(const json &value, const char *field, const std::string &path);

json find_package(const json &packages, const std::string &key);
json package_closure(const std::filesystem::path &package_path);
void validate_enum(const std::string &value, const std::set<std::string> &allowed, const std::string &label);
json semantic_graph(const json &packages, json &diagnostics);
snapshot build_snapshot(const json &request);

json public_package(json package);
json assess_package(const json &package, const std::string &registry_root, const json &request);
json snapshot_projection(const snapshot &value);
snapshot snapshot_from_projection(const json &value);
snapshot merge_candidate_observation(const snapshot &authority, const snapshot &candidate);
snapshot empty_observation();

std::filesystem::path lifecycle_root(const std::string &runtime_dir);
json fact_call(const std::string &runtime_dir, const std::string &action, json request);
std::string fact_id(const std::string &kind, const std::string &identity);
std::string relation_id(const std::string &kind, const std::string &source, const std::string &target);
lifecycle_view load_lifecycle(const std::string &runtime_dir);
const json &provider_for(const snapshot &value, const std::string &provider_id);
std::string derived_verdict(const json &provider, const std::string &desired, const std::string &observed);
std::string runtime_placement(const std::string &host);
json package_host_authorization(const json &package, const json &provider, const json &required_capabilities,
                                const std::string &placement, const lifecycle_view &lifecycle,
                                const std::string &generation_root);
json host_generation(const snapshot &value, const lifecycle_view &lifecycle);
json experience_flow_descriptor(const snapshot &value, const lifecycle_view &lifecycle, const std::string &plan_root);
json authorize_host_launch(const json &descriptor, const lifecycle_view &lifecycle, const json &request);
json lifecycle_plan(const snapshot &value, const lifecycle_view &lifecycle, const json &request);
json lifecycle_history(const std::string &runtime_dir, const json &request);

json mutation_authorization_plan(const snapshot &value, const lifecycle_view &lifecycle, const json &request,
                                 const json &load_plan);
json control_bootstrap_policy();
json control_status(const lifecycle_view &lifecycle);
json control_plan(const snapshot &value, const lifecycle_view &lifecycle, const json &request, const json &load_plan);
json apply_lifecycle_mutation(const snapshot &value, const lifecycle_view &lifecycle, const json &plan,
                              const json &request, const std::string &runtime_dir);

} // namespace kungfu::runtime::kfx::native_registry_internal

#endif // KUNGFU_RUNTIME_KFX_NATIVE_REGISTRY_INTERNAL_H
