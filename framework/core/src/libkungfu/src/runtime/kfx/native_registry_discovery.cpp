// SPDX-License-Identifier: Apache-2.0

#include <kungfu/runtime/kfx/native_registry.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <kungfu/runtime/kfx/native_contract.h>
#include <kungfu/runtime/profile/profile_lifecycle.h>
#include <kungfu/runtime/profile/profile_source_contract.h>
#include <kungfu/runtime/storage/fact_kernel.h>
#include <kungfu/yijinjing/storage/content_hash.h>

#include "native_authority.h"

#include "native_registry_internal.h"

namespace kungfu::runtime::kfx {
namespace native_registry_internal {

namespace fs = std::filesystem;
using json = nlohmann::json;

inline constexpr size_t MAX_PACKAGE_FILES = 10000;
inline constexpr size_t MAX_PACKAGES = 4096;
inline constexpr const char *KFX_MANIFEST_FILE = "kungfu.kfx.json";
inline constexpr const char *PACKAGE_TRANSPORT_FILE = "package.json";

[[noreturn]] void refuse(const std::string &code, const std::string &message) {
  throw std::invalid_argument(code + ": " + message);
}

std::string sha256(const std::string &value) {
  return yijinjing::storage::compute_content_hash_value(value, yijinjing::storage::CONTENT_HASH_ALGORITHM_SHA256);
}

std::string root_of(const json &value) { return "sha256:" + sha256(value.dump()); }

json embedded_domain_profile() {
  const auto &source = generated::KFX_DOMAIN_PROFILE_CONTRACT;
  return json::parse(source.begin(), source.end());
}

std::string read_file(const fs::path &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input)
    refuse("KF_KFX_SCHEMA_INVALID", "cannot read KFX file: " + path.string());
  return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

bool safe_token(const std::string &value) {
  return !value.empty() && value.size() <= 128 && std::all_of(value.begin(), value.end(), [](unsigned char ch) {
    return std::isalnum(ch) != 0 || ch == '-' || ch == '_' || ch == '.';
  });
}

std::string required_text(const json &value, const char *field, const std::string &path) {
  if (!value.is_object() || !value.contains(field) || !value.at(field).is_string() ||
      value.at(field).get<std::string>().empty()) {
    refuse("KF_KFX_SCHEMA_INVALID", path + "." + field + " must be a non-empty string");
  }
  return value.at(field).get<std::string>();
}

json object_or_empty(const json &value, const char *field) {
  if (!value.is_object() || !value.contains(field) || value.at(field).is_null())
    return json::object();
  if (!value.at(field).is_object())
    refuse("KF_KFX_SCHEMA_INVALID", std::string(field) + " must be an object");
  return value.at(field);
}

std::vector<std::string> string_array_or_empty(const json &value, const char *field) {
  if (!value.is_object() || !value.contains(field) || value.at(field).is_null())
    return {};
  if (!value.at(field).is_array())
    refuse("KF_KFX_SCHEMA_INVALID", std::string(field) + " must be an array");
  std::vector<std::string> result;
  for (const auto &entry : value.at(field)) {
    if (!entry.is_string() || entry.get<std::string>().empty())
      refuse("KF_KFX_SCHEMA_INVALID", std::string(field) + " must contain non-empty strings");
    result.push_back(entry.get<std::string>());
  }
  std::sort(result.begin(), result.end());
  if (std::adjacent_find(result.begin(), result.end()) != result.end())
    refuse("KF_KFX_SCHEMA_INVALID", std::string(field) + " must not contain duplicates");
  return result;
}

bool ignored_part(const fs::path &part) {
  static const std::set<std::string> ignored = {".git", "node_modules", "__pycache__", ".DS_Store"};
  return ignored.contains(part.string());
}

json package_closure(const fs::path &package_path) {
  json files = json::array();
  std::error_code error;
  fs::recursive_directory_iterator iterator(package_path, fs::directory_options::skip_permission_denied, error);
  fs::recursive_directory_iterator end;
  if (error)
    refuse("KF_KFX_SCHEMA_INVALID", "cannot scan KFX package: " + package_path.string());
  for (; iterator != end; iterator.increment(error)) {
    if (error)
      refuse("KF_KFX_SCHEMA_INVALID", "cannot scan KFX package: " + error.message());
    const auto relative = iterator->path().lexically_relative(package_path);
    const auto ignored = std::any_of(relative.begin(), relative.end(), ignored_part);
    if (ignored) {
      if (iterator->is_directory(error))
        iterator.disable_recursion_pending();
      continue;
    }
    if (iterator->is_symlink(error))
      refuse("KF_KFX_PATH_TRAVERSAL", "KFX package closure cannot contain symlinks: " + relative.generic_string());
    if (!iterator->is_regular_file(error))
      continue;
    const auto bytes = read_file(iterator->path());
    files.push_back({{"path", relative.generic_string()}, {"sha256", sha256(bytes)}, {"size", bytes.size()}});
    if (files.size() > MAX_PACKAGE_FILES)
      refuse("KF_KFX_SCHEMA_INVALID", "KFX package exceeds the bounded file count");
  }
  std::sort(files.begin(), files.end(), [](const auto &left, const auto &right) {
    return left.at("path").template get<std::string>() < right.at("path").template get<std::string>();
  });
  if (files.empty())
    refuse("KF_KFX_CLOSURE_MISSING", "KFX package closure is empty: " + package_path.string());
  return {{"schema", "kungfu.kfx-package-closure/v1"}, {"files", files}};
}

void validate_relative_path(const fs::path &root, const std::string &relative, const std::string &label) {
  const fs::path value(relative);
  if (relative.empty() || value.is_absolute())
    refuse("KF_KFX_PATH_TRAVERSAL", label + " must be a confined relative path");
  for (const auto &part : value) {
    if (part == "..")
      refuse("KF_KFX_PATH_TRAVERSAL", label + " escapes the package root");
  }
  const auto candidate = fs::weakly_canonical(root / value);
  auto root_it = root.begin();
  auto candidate_it = candidate.begin();
  for (; root_it != root.end(); ++root_it, ++candidate_it) {
    if (candidate_it == candidate.end() || *root_it != *candidate_it)
      refuse("KF_KFX_PATH_TRAVERSAL", label + " escapes the package root");
  }
  if (!fs::is_regular_file(candidate))
    refuse("KF_KFX_CLOSURE_MISSING", label + " does not resolve to a regular file");
}

std::vector<std::string> declared_facets(const json &manifest, const fs::path &package_path) {
  const auto config = object_or_empty(manifest.at("kungfuConfig"), "config");
  std::vector<std::string> facets;
  for (const auto *facet : {"view", "adapter", "service", "wasm"}) {
    if (!config.contains(facet))
      continue;
    if (!config.at(facet).is_object())
      refuse("KF_KFX_SCHEMA_INVALID", std::string("kungfuConfig.config.") + facet + " must be an object");
    facets.emplace_back(facet);
    const auto &declaration = config.at(facet);
    if (declaration.contains("entry")) {
      if (declaration.at("entry").is_string()) {
        validate_relative_path(package_path, declaration.at("entry").get<std::string>(),
                               std::string("kungfuConfig.config.") + facet + ".entry");
      } else if (declaration.at("entry").is_object()) {
        for (const auto &[runtime, entry] : declaration.at("entry").items()) {
          if (entry.is_string())
            validate_relative_path(package_path, entry.get<std::string>(),
                                   std::string("kungfuConfig.config.") + facet + ".entry." + runtime);
        }
      }
    }
  }
  if (manifest.at("kungfuConfig").contains("suite"))
    facets.emplace_back("profile-suite");
  std::sort(facets.begin(), facets.end());
  return facets;
}

std::vector<std::string> declared_capabilities(const json &manifest) {
  std::set<std::string> capabilities;
  const auto config = object_or_empty(manifest.at("kungfuConfig"), "config");
  for (const auto *facet : {"view", "adapter", "service", "wasm"}) {
    if (!config.contains(facet) || !config.at(facet).is_object() || !config.at(facet).contains("capabilities"))
      continue;
    if (!config.at(facet).at("capabilities").is_array())
      refuse("KF_KFX_SCHEMA_INVALID", std::string("kungfuConfig.config.") + facet + ".capabilities must be an array");
    for (const auto &capability : config.at(facet).at("capabilities")) {
      if (!capability.is_string() || capability.get<std::string>().empty())
        refuse("KF_KFX_SCHEMA_INVALID", "declared capability must be a non-empty string");
      capabilities.insert(capability.get<std::string>());
    }
  }
  return {capabilities.begin(), capabilities.end()};
}

std::vector<std::string> declared_product_roles(const json &manifest) {
  const auto product = object_or_empty(manifest.at("kungfuConfig"), "product");
  const auto roles = string_array_or_empty(product, "roles");
  return roles;
}

std::vector<fs::path> package_directories(const fs::path &root) {
  std::set<fs::path> result;
  if (fs::is_regular_file(root / KFX_MANIFEST_FILE) || fs::is_regular_file(root / PACKAGE_TRANSPORT_FILE))
    result.insert(root);
  std::error_code error;
  fs::recursive_directory_iterator iterator(root, fs::directory_options::skip_permission_denied, error);
  fs::recursive_directory_iterator end;
  for (; !error && iterator != end; iterator.increment(error)) {
    if (iterator.depth() >= 3 && iterator->is_directory(error))
      iterator.disable_recursion_pending();
    if (!iterator->is_directory(error))
      continue;
    if (iterator->is_symlink(error) || iterator->path().filename() == "node_modules") {
      iterator.disable_recursion_pending();
      continue;
    }
    if (fs::is_regular_file(iterator->path() / KFX_MANIFEST_FILE, error) ||
        fs::is_regular_file(iterator->path() / PACKAGE_TRANSPORT_FILE, error))
      result.insert(fs::weakly_canonical(iterator->path()));
  }
  if (error)
    refuse("KF_KFX_SCHEMA_INVALID", "cannot scan KFX root: " + root.string() + ": " + error.message());
  return {result.begin(), result.end()};
}

void validate_enum(const std::string &value, const std::set<std::string> &allowed, const std::string &label) {
  if (!allowed.contains(value))
    refuse("KF_KFX_SCHEMA_INVALID", label + " is not supported: " + value);
}

json host_placements(const json &manifest) {
  std::set<std::string> hosts;
  const auto config = object_or_empty(manifest.at("kungfuConfig"), "config");
  if (config.contains("view"))
    hosts.insert("gui");
  for (const auto *facet : {"adapter", "service"}) {
    if (!config.contains(facet) || !config.at(facet).is_object())
      continue;
    for (const auto &runtime : string_array_or_empty(config.at(facet), "runtimes"))
      hosts.insert(std::string(facet) + "-" + runtime);
  }
  if (config.contains("wasm"))
    hosts.insert("wasm");
  if (manifest.at("kungfuConfig").contains("suite"))
    hosts.insert("profile");
  return json(hosts);
}

json find_package(const json &packages, const std::string &key) {
  for (const auto &package : packages) {
    if (package.at("key") == key)
      return package;
  }
  return nullptr;
}

void detect_suite_cycles(const json &packages) {
  std::map<std::string, std::vector<std::string>> graph;
  for (const auto &package : packages) {
    if (!package.contains("suiteMembers"))
      continue;
    auto &edges = graph[package.at("key").get<std::string>()];
    for (const auto &member : package.at("suiteMembers")) {
      const auto nested = find_package(packages, member.get<std::string>());
      if (!nested.is_null() && nested.contains("suiteMembers"))
        edges.push_back(member.get<std::string>());
    }
  }
  std::set<std::string> active;
  std::set<std::string> complete;
  std::function<void(const std::string &)> visit = [&](const std::string &key) {
    if (active.contains(key))
      refuse("KF_KFX_SUITE_CYCLE", "KFX Suite membership cycle includes " + key);
    if (complete.contains(key))
      return;
    active.insert(key);
    for (const auto &next : graph[key])
      visit(next);
    active.erase(key);
    complete.insert(key);
  };
  for (const auto &[key, ignored] : graph) {
    (void)ignored;
    visit(key);
  }
}

bool version_matches(const std::string &version, const std::string &constraint) {
  if (constraint == "*")
    return true;
  if (constraint.starts_with("^")) {
    const auto expected = constraint.substr(1, constraint.find('.') - 1);
    return version.substr(0, version.find('.')) == expected;
  }
  if (constraint.ends_with(".*"))
    return version.starts_with(constraint.substr(0, constraint.size() - 1));
  return version == constraint;
}

class semantic_graph_builder {
public:
  semantic_graph_builder(const json &packages, json &diagnostics) : packages_(packages), diagnostics_(diagnostics) {}

  json build() {
    collect_providers();
    collect_extension_points();
    collect_dependencies();
    degrade_dependency_cycles();
    collect_contributions();
    return finalize();
  }

private:
  void collect_providers() {
    for (const auto &package : packages_) {
      const auto provider_id = package.at("key").get<std::string>();
      const auto capabilities = package.at("declaredCapabilities");
      const json trust_identity = {{"runtimeTier", package.at("runtimeTier")},
                                   {"admissionGrade", package.at("admissionGrade")}};
      const auto trust_root = root_of(trust_identity);
      const auto capability_root = root_of(capabilities);
      const json identity = {{"providerId", provider_id},
                             {"version", package.at("version")},
                             {"packageRoot", package.at("packageRoot")},
                             {"trustRoot", trust_root},
                             {"capabilityRoot", capability_root}};
      json provider = identity;
      provider["providerRoot"] = root_of(identity);
      provider["trust"] = trust_identity;
      provider["capabilities"] = capabilities;
      provider["state"] = "active";
      provider["causes"] = json::array();
      provider["recoveryGuidance"] = json::array();
      providers_.push_back(provider);
      provider_by_id_[provider_id] = provider;
      provider_state_[provider_id] = "active";
    }
  }

  void add_diagnostic(const std::string &code, const std::string &provider_id, const std::string &cause,
                      const std::string &recovery, const std::string &severity) {
    diagnostics_.push_back({{"code", code},
                            {"providerId", provider_id},
                            {"cause", cause},
                            {"recoveryGuidance", json::array({recovery})},
                            {"severity", severity}});
    if (severity == "degraded")
      provider_state_[provider_id] = "degraded";
  }

  void collect_extension_points() {
    for (const auto &package : packages_) {
      const auto provider_id = package.at("key").get<std::string>();
      const auto semantic = package.at("semantic");
      const auto points = semantic.value("extensionPoints", json::array());
      for (const auto &declaration : points) {
        const auto point_id = required_text(declaration, "id", "kungfuConfig.registry.extensionPoints[]");
        if (!safe_token(point_id))
          refuse("KF_KFX_SCHEMA_INVALID", "extension point id is not a safe token");
        if (extension_point_by_id_.contains(point_id))
          refuse("KF_KFX_OWNER_DUPLICATE", "one extension point may have only one owner: " + point_id);
        const auto capabilities = declaration.value("capabilities", json::array());
        const json identity = {{"ownerProviderId", provider_id},
                               {"extensionPointId", point_id},
                               {"version", declaration.at("version")},
                               {"surface", declaration.at("surface")},
                               {"capabilityRoot", root_of(capabilities)}};
        json point = identity;
        point["extensionPointRoot"] = root_of(identity);
        point["capabilities"] = capabilities;
        point["state"] = "active";
        extension_points_.push_back(point);
        extension_point_by_id_[point_id] = point;
      }
    }
  }

  void collect_dependencies() {
    for (const auto &package : packages_) {
      const auto consumer_id = package.at("key").get<std::string>();
      const auto declarations = package.at("semantic").value("dependencies", json::array());
      for (const auto &declaration : declarations) {
        const auto provider_id = required_text(declaration, "provider", "kungfuConfig.registry.dependencies[]");
        const auto mode = required_text(declaration, "mode", "kungfuConfig.registry.dependencies[]");
        const auto constraint = required_text(declaration, "version", "kungfuConfig.registry.dependencies[]");
        const auto capabilities = declaration.value("capabilities", json::array());
        const auto grades = declaration.value("admissionGrades", json::array());
        const json identity = {
            {"consumerProviderId", consumer_id},      {"providerId", provider_id},
            {"versionConstraint", constraint},        {"mode", mode},
            {"trustConstraintRoot", root_of(grades)}, {"capabilityConstraintRoot", root_of(capabilities)}};
        json edge = identity;
        edge["dependencyRoot"] = root_of(identity);
        edge["state"] = "active";
        edge["causes"] = json::array();
        edge["recoveryGuidance"] = json::array();
        if (!provider_by_id_.contains(provider_id)) {
          const auto code =
              mode == "required" ? "KF_KFX_REQUIRED_PROVIDER_MISSING" : "KF_KFX_OPTIONAL_PROVIDER_MISSING";
          edge["state"] = mode == "required" ? "degraded" : "dormant";
          edge["causes"].push_back(code);
          edge["recoveryGuidance"].push_back("install-provider:" + provider_id);
          add_diagnostic(code, consumer_id, "provider is absent: " + provider_id, "install-provider:" + provider_id,
                         mode == "required" ? "degraded" : "dormant");
        } else {
          const auto &provider = provider_by_id_.at(provider_id);
          if (!version_matches(provider.at("version").get<std::string>(), constraint)) {
            edge["state"] = mode == "required" ? "degraded" : "dormant";
            edge["causes"].push_back("KF_KFX_PROVIDER_VERSION_MISMATCH");
            edge["recoveryGuidance"].push_back("install-compatible-provider:" + provider_id + "@" + constraint);
            add_diagnostic("KF_KFX_PROVIDER_VERSION_MISMATCH", consumer_id,
                           "provider version does not satisfy " + constraint + ": " + provider_id,
                           "install-compatible-provider:" + provider_id + "@" + constraint,
                           mode == "required" ? "degraded" : "dormant");
          }
          for (const auto &capability : capabilities) {
            if (std::find(provider.at("capabilities").begin(), provider.at("capabilities").end(), capability) ==
                provider.at("capabilities").end()) {
              edge["state"] = mode == "required" ? "degraded" : "dormant";
              edge["causes"].push_back("KF_KFX_CAPABILITY_BROADENING");
              edge["recoveryGuidance"].push_back("use-provider-with-declared-capability:" +
                                                 capability.get<std::string>());
              add_diagnostic("KF_KFX_CAPABILITY_BROADENING", consumer_id,
                             "dependency capability is not declared by provider: " + capability.get<std::string>(),
                             "use-provider-with-declared-capability:" + capability.get<std::string>(),
                             mode == "required" ? "degraded" : "dormant");
            }
          }
          if (!grades.empty() &&
              std::find(grades.begin(), grades.end(), provider.at("trust").at("admissionGrade")) == grades.end()) {
            edge["state"] = mode == "required" ? "degraded" : "dormant";
            edge["causes"].push_back("KF_KFX_TRUST_CONSTRAINT_REJECTED");
            edge["recoveryGuidance"].push_back("admit-exact-provider-root:" + provider_id);
            add_diagnostic("KF_KFX_TRUST_CONSTRAINT_REJECTED", consumer_id,
                           "provider admission grade is outside the dependency constraint: " + provider_id,
                           "admit-exact-provider-root:" + provider_id, mode == "required" ? "degraded" : "dormant");
          }
          dependency_graph_[consumer_id].push_back(provider_id);
        }
        dependencies_.push_back(edge);
      }
    }
  }

  void visit_dependency(const std::string &provider_id, std::set<std::string> &visiting, std::set<std::string> &visited,
                        std::set<std::string> &cycle_members) {
    if (visiting.contains(provider_id)) {
      cycle_members.insert(provider_id);
      return;
    }
    if (visited.contains(provider_id))
      return;
    visiting.insert(provider_id);
    for (const auto &dependency : dependency_graph_[provider_id]) {
      if (visiting.contains(dependency)) {
        cycle_members.insert(provider_id);
        cycle_members.insert(dependency);
      } else {
        visit_dependency(dependency, visiting, visited, cycle_members);
      }
    }
    visiting.erase(provider_id);
    visited.insert(provider_id);
  }

  void degrade_dependency_cycles() {
    std::set<std::string> visiting;
    std::set<std::string> visited;
    std::set<std::string> cycle_members;
    for (const auto &[provider_id, ignored] : dependency_graph_) {
      (void)ignored;
      visit_dependency(provider_id, visiting, visited, cycle_members);
    }
    for (const auto &provider_id : cycle_members)
      add_diagnostic("KF_KFX_DEPENDENCY_CYCLE", provider_id, "semantic provider dependency cycle",
                     "remove-or-relax-cyclic-dependency", "degraded");
    for (auto &edge : dependencies_) {
      if (cycle_members.contains(edge.at("consumerProviderId").get<std::string>()) &&
          cycle_members.contains(edge.at("providerId").get<std::string>())) {
        edge["state"] = "degraded";
        edge["causes"].push_back("KF_KFX_DEPENDENCY_CYCLE");
        edge["recoveryGuidance"].push_back("remove-or-relax-cyclic-dependency");
      }
    }
  }

  void collect_contributions() {
    std::set<std::string> contribution_ids;
    for (const auto &package : packages_) {
      const auto provider_id = package.at("key").get<std::string>();
      const auto declarations = package.at("semantic").value("contributions", json::array());
      for (const auto &declaration : declarations) {
        const auto contribution_id = required_text(declaration, "id", "kungfuConfig.registry.contributions[]");
        const auto point_id = required_text(declaration, "extensionPoint", "kungfuConfig.registry.contributions[]");
        const auto canonical_id = provider_id + ":" + contribution_id;
        if (!contribution_ids.insert(canonical_id).second)
          refuse("KF_KFX_OWNER_DUPLICATE", "duplicate contribution identity: " + canonical_id);
        const auto capabilities = declaration.value("capabilities", json::array());
        const json identity = {{"ownerProviderId", provider_id},
                               {"contributionId", contribution_id},
                               {"extensionPointId", point_id},
                               {"version", declaration.at("version")},
                               {"capabilityRoot", root_of(capabilities)}};
        json contribution = identity;
        contribution["contributionRoot"] = root_of(identity);
        contribution["capabilities"] = capabilities;
        contribution["presentation"] = declaration.value("presentation", json::object());
        contribution["state"] = provider_state_.at(provider_id);
        contribution["causes"] = json::array();
        contribution["recoveryGuidance"] = json::array();
        if (!extension_point_by_id_.contains(point_id)) {
          contribution["state"] = "degraded";
          contribution["causes"].push_back("KF_KFX_EXTENSION_POINT_MISSING");
          contribution["recoveryGuidance"].push_back("install-extension-point-owner:" + point_id);
          add_diagnostic("KF_KFX_EXTENSION_POINT_MISSING", provider_id, "contribution target is absent: " + point_id,
                         "install-extension-point-owner:" + point_id, "degraded");
        } else {
          const auto &point = extension_point_by_id_.at(point_id);
          contribution["extensionPointRoot"] = point.at("extensionPointRoot");
          contribution["targetOwnerProviderId"] = point.at("ownerProviderId");
          contribution["surface"] = point.at("surface");
          if (!version_matches(point.at("version").get<std::string>(), declaration.at("version").get<std::string>())) {
            contribution["state"] = "degraded";
            contribution["causes"].push_back("KF_KFX_PROVIDER_VERSION_MISMATCH");
            contribution["recoveryGuidance"].push_back("target-compatible-extension-point:" + point_id);
            add_diagnostic("KF_KFX_PROVIDER_VERSION_MISMATCH", provider_id,
                           "extension point version does not satisfy contribution constraint: " + point_id,
                           "target-compatible-extension-point:" + point_id, "degraded");
          }
          for (const auto &capability : capabilities) {
            if (std::find(package.at("declaredCapabilities").begin(), package.at("declaredCapabilities").end(),
                          capability) == package.at("declaredCapabilities").end()) {
              contribution["state"] = "degraded";
              contribution["causes"].push_back("KF_KFX_CAPABILITY_BROADENING");
              contribution["recoveryGuidance"].push_back("declare-contribution-capability:" +
                                                         capability.get<std::string>());
              add_diagnostic("KF_KFX_CAPABILITY_BROADENING", provider_id,
                             "contribution capability is not declared by its provider: " +
                                 capability.get<std::string>(),
                             "declare-contribution-capability:" + capability.get<std::string>(), "degraded");
            }
          }
        }
        contributions_.push_back(contribution);
      }
    }
  }

  void finalize_provider_states() {
    for (auto &provider : providers_) {
      const auto provider_id = provider.at("providerId").get<std::string>();
      provider["state"] = provider_state_.at(provider_id);
      for (const auto &diagnostic : diagnostics_) {
        if (diagnostic.value("providerId", "") != provider_id)
          continue;
        provider["causes"].push_back(diagnostic.at("code"));
        for (const auto &guidance : diagnostic.at("recoveryGuidance"))
          provider["recoveryGuidance"].push_back(guidance);
      }
      std::sort(provider["causes"].begin(), provider["causes"].end());
      std::sort(provider["recoveryGuidance"].begin(), provider["recoveryGuidance"].end());
    }
  }

  static void sort_by(json &values, const char *field) {
    std::sort(values.begin(), values.end(), [field](const auto &left, const auto &right) {
      return left.at(field).template get<std::string>() < right.at(field).template get<std::string>();
    });
  }

  json finalize() {
    finalize_provider_states();
    sort_by(providers_, "providerRoot");
    sort_by(extension_points_, "extensionPointRoot");
    sort_by(contributions_, "contributionRoot");
    sort_by(dependencies_, "dependencyRoot");
    const json identity = {{"schema", "kungfu.kfx.semantic-graph/v1"},
                           {"providers", providers_},
                           {"extensionPoints", extension_points_},
                           {"contributions", contributions_},
                           {"dependencies", dependencies_}};
    auto graph = identity;
    graph["graphRoot"] = root_of(identity);
    return graph;
  }

  const json &packages_;
  json &diagnostics_;
  json providers_ = json::array();
  json extension_points_ = json::array();
  json contributions_ = json::array();
  json dependencies_ = json::array();
  std::map<std::string, json> provider_by_id_;
  std::map<std::string, json> extension_point_by_id_;
  std::map<std::string, std::string> provider_state_;
  std::map<std::string, std::vector<std::string>> dependency_graph_;
};

json semantic_graph(const json &packages, json &diagnostics) {
  return semantic_graph_builder(packages, diagnostics).build();
}

void validate_snapshot_request(const json &request) {
  if (!request.is_object() || !request.contains("roots") || !request.at("roots").is_array() ||
      request.at("roots").empty())
    refuse("KF_KFX_SCHEMA_INVALID", "registry request requires explicit non-empty roots");
  for (const auto *field : {"runtimeTier", "runtimeTiers", "hostPlacements", "admissionGrade", "admissionGrades",
                            "productSystem", "firstParty", "system", "trusted", "supportsKFD", "installed", "admitted",
                            "systemAuthority", "grantedCapabilities"}) {
    if (request.contains(field))
      refuse("KF_KFX_AUTHORITY_CLAIM_FORBIDDEN",
             std::string("registry request may not claim Core-derived authority field ") + field);
  }
}

bool package_transport_claims_kfx(const fs::path &package_transport_path) {
  if (!fs::is_regular_file(package_transport_path))
    return false;
  try {
    const auto package_transport = json::parse(read_file(package_transport_path));
    return package_transport.is_object() && package_transport.contains("kungfuConfig");
  } catch (const json::exception &) {
    // Transport validation belongs to the package manager. It becomes a KFX
    // concern only when it claims the removed semantic authority.
    return false;
  }
}

std::optional<json> candidate_package(const fs::path &package_path, std::map<std::string, fs::path> &keys) {
  const auto manifest_path = package_path / KFX_MANIFEST_FILE;
  if (package_transport_claims_kfx(package_path / PACKAGE_TRANSPORT_FILE)) {
    refuse(fs::is_regular_file(manifest_path) ? "KF_KFX_MANIFEST_CONFLICT" : "KF_KFX_MANIFEST_MISSING",
           "package.json must not author kungfuConfig; kungfu.kfx.json is the only KFX manifest authority");
  }
  if (!fs::is_regular_file(manifest_path))
    return std::nullopt;

  json manifest;
  try {
    manifest = json::parse(read_file(manifest_path));
  } catch (const json::exception &error) {
    refuse("KF_KFX_SCHEMA_INVALID", "invalid KFX package manifest: " + std::string(error.what()));
  }
  if (!manifest.is_object() || !manifest.contains("kungfuConfig") || !manifest.at("kungfuConfig").is_object())
    return std::nullopt;
  manifest = normalize_native_kfx_manifest(manifest);
  const auto key = required_text(manifest.at("kungfuConfig"), "key", "kungfuConfig");
  if (!safe_token(key))
    refuse("KF_KFX_SCHEMA_INVALID", "kungfuConfig.key is not a safe KFX token");
  if (keys.contains(key))
    refuse("KF_KFX_PACKAGE_DUPLICATE", "KFX package key resolves more than once: " + key);
  keys[key] = package_path;

  const auto closure = package_closure(package_path);
  const auto native_contract = native_kfx_contract();
  json package = {{"key", key},
                  {"name", manifest.value("name", "")},
                  {"version", manifest.value("version", "")},
                  {"path", package_path.string()},
                  {"manifestRoot", root_of(manifest)},
                  {"apiCompatibility",
                   {{"sourceContractSchema", native_contract.at("sourceContractSchema")},
                    {"sourceContractVersion", native_contract.at("sourceContractVersion")},
                    {"nativeContractVersion", native_contract.at("contractVersion")},
                    {"compatible", true}}},
                  {"packageRoot", root_of(closure)},
                  {"closure", closure},
                  {"facets", declared_facets(manifest, package_path)},
                  {"declaredCapabilities", declared_capabilities(manifest)},
                  {"productRoles", declared_product_roles(manifest)},
                  {"runtimeTier", "isolated"},
                  {"admissionGrade", "unverified"},
                  {"supplyChainGrade", "unverified"},
                  {"grantedCapabilities", json::array()},
                  {"hosts", host_placements(manifest)},
                  {"semantic", object_or_empty(manifest.at("kungfuConfig"), "registry")},
                  {"candidate", true},
                  {"installed", false},
                  {"admitted", false}};
  if (!manifest.at("kungfuConfig").contains("suite"))
    return package;
  const auto &suite = manifest.at("kungfuConfig").at("suite");
  if (!suite.is_object() || !suite.contains("members") || !suite.at("members").is_array())
    refuse("KF_KFX_SCHEMA_INVALID", "KFX Suite must declare members");
  package["suiteMembers"] = suite.at("members");
  if (suite.contains("profile")) {
    const auto relative = required_text(suite, "profile", "kungfuConfig.suite");
    validate_relative_path(package_path, relative, "kungfuConfig.suite.profile");
    package["profilePath"] = fs::weakly_canonical(package_path / relative).string();
  }
  return package;
}

json discover_packages(const json &roots) {
  const std::set<std::string> root_kinds = {"product", "user", "workspace"};
  json packages = json::array();
  std::set<fs::path> seen_roots;
  std::set<fs::path> seen_packages;
  std::map<std::string, fs::path> keys;
  for (const auto &root_value : roots) {
    const auto kind = required_text(root_value, "kind", "roots[]");
    validate_enum(kind, root_kinds, "root kind");
    const auto root = fs::weakly_canonical(required_text(root_value, "path", "roots[]"));
    if (!fs::is_directory(root))
      refuse("KF_KFX_SCHEMA_INVALID", "KFX root is not a directory: " + root.string());
    if (!seen_roots.insert(root).second)
      refuse("KF_KFX_ROOT_COLLISION", "multiple root declarations resolve to " + root.string());
    for (const auto &package_path : package_directories(root)) {
      if (!seen_packages.insert(package_path).second)
        continue;
      const auto package = candidate_package(package_path, keys);
      if (!package.has_value())
        continue;
      packages.push_back(*package);
      if (packages.size() > MAX_PACKAGES)
        refuse("KF_KFX_SCHEMA_INVALID", "KFX registry exceeds the bounded package count");
    }
  }
  std::sort(packages.begin(), packages.end(), [](const auto &left, const auto &right) {
    return left.at("key").template get<std::string>() < right.at("key").template get<std::string>();
  });
  detect_suite_cycles(packages);
  return packages;
}

std::vector<std::string> declared_suite_members(const json &package) {
  std::vector<std::string> declared;
  for (const auto &member : package.at("suiteMembers")) {
    if (!member.is_string() || !safe_token(member.get<std::string>()))
      refuse("KF_KFX_SCHEMA_INVALID", "KFX Suite members must be safe tokens");
    declared.push_back(member.get<std::string>());
  }
  std::sort(declared.begin(), declared.end());
  if (std::adjacent_find(declared.begin(), declared.end()) != declared.end())
    refuse("KF_KFX_SCHEMA_INVALID", "KFX Suite members must be unique");
  return declared;
}

std::pair<json, json> suite_members(const json &package, const std::vector<std::string> &declared) {
  if (!package.contains("profilePath"))
    return {json(declared), json::array()};
  const auto profile = json::parse(read_file(package.at("profilePath").get<std::string>()));
  if (!profile.is_object() || !profile.contains("members") || !profile.at("members").is_object())
    refuse("KF_KFX_SCHEMA_INVALID", "KFX Profile Suite must declare required and optional members");
  std::vector<std::string> profile_members;
  for (const auto *kind : {"required", "optional"}) {
    if (!profile.at("members").contains(kind) || !profile.at("members").at(kind).is_array())
      refuse("KF_KFX_SCHEMA_INVALID", std::string("Profile members.") + kind + " must be an array");
    for (const auto &member : profile.at("members").at(kind))
      profile_members.push_back(member.get<std::string>());
  }
  std::sort(profile_members.begin(), profile_members.end());
  if (profile_members != declared)
    refuse("KF_KFX_SCHEMA_INVALID", "Profile members must match the Suite manifest");
  return {profile.at("members").at("required"), profile.at("members").at("optional")};
}

json build_suite(const json &package, const json &packages, json &diagnostics) {
  const auto declared = declared_suite_members(package);
  const auto [required, optional] = suite_members(package, declared);
  json member_roots = json::object();
  json missing_optional = json::array();
  for (const auto &member : required) {
    const auto found = find_package(packages, member.get<std::string>());
    if (found.is_null())
      refuse("KF_KFX_MEMBER_MISSING", "required KFX Suite member is missing: " + member.get<std::string>());
    member_roots[member.get<std::string>()] = found.at("packageRoot");
  }
  for (const auto &member : optional) {
    const auto found = find_package(packages, member.get<std::string>());
    if (found.is_null())
      missing_optional.push_back(member);
    else
      member_roots[member.get<std::string>()] = found.at("packageRoot");
  }
  json profile_root = nullptr;
  if (package.contains("profilePath") && missing_optional.empty())
    profile_root =
        profile::inspect_profile(package.at("profilePath").get<std::string>(), member_roots).at("profile_suite_root");
  if (!missing_optional.empty())
    diagnostics.push_back({{"code", "KF_KFX_OPTIONAL_MEMBER_MISSING"},
                           {"suiteKey", package.at("key")},
                           {"members", missing_optional},
                           {"severity", "degraded"}});
  const json identity = {{"schema", "kungfu.kfx-suite-closure/v1"},
                         {"suiteKey", package.at("key")},
                         {"suitePackageRoot", package.at("packageRoot")},
                         {"required", required},
                         {"optional", optional},
                         {"memberRoots", member_roots},
                         {"missingOptional", missing_optional},
                         {"profileRoot", profile_root}};
  return {{"suiteKey", package.at("key")},
          {"suiteRoot", root_of(identity)},
          {"profileRoot", profile_root},
          {"required", required},
          {"optional", optional},
          {"memberRoots", member_roots},
          {"missingOptional", missing_optional}};
}

json registry_identity(const snapshot &value) {
  json packages = json::array();
  for (const auto &package : value.packages) {
    packages.push_back(
        {{"key", package.at("key")},
         {"packageRoot", package.at("packageRoot")},
         {"manifestRoot", package.at("manifestRoot")},
         {"apiCompatibility", package.at("apiCompatibility")},
         {"facets", package.at("facets")},
         {"runtimeTier", package.at("runtimeTier")},
         {"admissionGrade", package.at("admissionGrade")},
         {"grantedCapabilities", package.at("grantedCapabilities")},
         {"capabilityGrantRoot",
          package.contains("authority") ? package.at("authority").at("capabilityGrantRoot") : json(nullptr)},
         {"hosts", package.at("hosts")}});
  }
  return {{"schema", "kungfu.kfx-registry-snapshot/v2"},
          {"packages", packages},
          {"suites", value.suites},
          {"diagnostics", value.diagnostics}};
}

snapshot build_snapshot(const json &request) {
  validate_snapshot_request(request);
  snapshot result;
  result.packages = discover_packages(request.at("roots"));
  for (const auto &package : result.packages) {
    if (package.contains("suiteMembers"))
      result.suites.push_back(build_suite(package, result.packages, result.diagnostics));
  }
  result.graph = semantic_graph(result.packages, result.diagnostics);
  result.graph_root = result.graph.at("graphRoot").get<std::string>();
  result.registry_root = root_of(registry_identity(result));
  if (request.contains("expectedRegistryRoot") &&
      (!request.at("expectedRegistryRoot").is_string() || request.at("expectedRegistryRoot") != result.registry_root))
    refuse("KF_KFX_REGISTRY_STALE", "registry content changed since the caller's expected root");
  return result;
}

json public_package(json package) {
  package.erase("closure");
  package.erase("suiteMembers");
  package.erase("profilePath");
  package.erase("semantic");
  return package;
}

json assess_package(const json &package, const std::string &registry_root, const json &request) {
  return authority::assess(package, registry_root, request);
}

json snapshot_projection(const snapshot &value) {
  return {{"packages", value.packages},          {"suites", value.suites},
          {"diagnostics", value.diagnostics},    {"graph", value.graph},
          {"registryRoot", value.registry_root}, {"graphRoot", value.graph_root}};
}

snapshot snapshot_from_projection(const json &value) {
  if (!value.is_object() || !value.contains("packages") || !value.at("packages").is_array() ||
      !value.contains("suites") || !value.at("suites").is_array() || !value.contains("diagnostics") ||
      !value.at("diagnostics").is_array() || !value.contains("graph") || !value.at("graph").is_object() ||
      !value.contains("registryRoot") || !value.at("registryRoot").is_string() || !value.contains("graphRoot") ||
      !value.at("graphRoot").is_string())
    refuse("KF_KFX_SCHEMA_INVALID", "KFX registry projection Fact body is incomplete");
  snapshot result;
  result.packages = value.at("packages");
  result.suites = value.at("suites");
  result.diagnostics = value.at("diagnostics");
  result.graph = value.at("graph");
  result.registry_root = value.at("registryRoot").get<std::string>();
  result.graph_root = value.at("graphRoot").get<std::string>();
  if (result.graph.value("graphRoot", "") != result.graph_root)
    refuse("KF_KFX_SCHEMA_INVALID", "KFX registry projection graph root is inconsistent");
  return result;
}

snapshot merge_candidate_observation(const snapshot &authority, const snapshot &candidate) {
  snapshot result = authority;
  std::map<std::string, json> packages;
  for (const auto &package : authority.packages)
    packages[package.at("key").get<std::string>()] = package;
  for (const auto &package : candidate.packages)
    packages[package.at("key").get<std::string>()] = package;
  result.packages = json::array();
  for (const auto &[ignored, package] : packages) {
    (void)ignored;
    result.packages.push_back(package);
  }
  std::map<std::string, json> suites;
  for (const auto &suite : authority.suites)
    suites[suite.at("suiteKey").get<std::string>()] = suite;
  for (const auto &suite : candidate.suites)
    suites[suite.at("suiteKey").get<std::string>()] = suite;
  result.suites = json::array();
  for (const auto &[ignored, suite] : suites) {
    (void)ignored;
    result.suites.push_back(suite);
  }
  result.diagnostics = candidate.diagnostics;
  result.graph = semantic_graph(result.packages, result.diagnostics);
  result.graph_root = result.graph.at("graphRoot").get<std::string>();
  json package_identity = json::array();
  for (const auto &package : result.packages) {
    package_identity.push_back(
        {{"key", package.at("key")},
         {"packageRoot", package.at("packageRoot")},
         {"manifestRoot", package.at("manifestRoot")},
         {"apiCompatibility", package.at("apiCompatibility")},
         {"facets", package.at("facets")},
         {"runtimeTier", package.at("runtimeTier")},
         {"admissionGrade", package.at("admissionGrade")},
         {"grantedCapabilities", package.at("grantedCapabilities")},
         {"capabilityGrantRoot",
          package.contains("authority") ? package.at("authority").at("capabilityGrantRoot") : json(nullptr)},
         {"hosts", package.at("hosts")}});
  }
  result.registry_root = root_of({{"schema", "kungfu.kfx-registry-snapshot/v2"},
                                  {"packages", package_identity},
                                  {"suites", result.suites},
                                  {"diagnostics", result.diagnostics}});
  return result;
}

snapshot empty_observation() {
  snapshot result;
  result.graph = semantic_graph(result.packages, result.diagnostics);
  result.graph_root = result.graph.at("graphRoot").get<std::string>();
  result.registry_root = root_of({{"schema", "kungfu.kfx-registry-snapshot/v2"},
                                  {"packages", json::array()},
                                  {"suites", json::array()},
                                  {"diagnostics", json::array()}});
  return result;
}

} // namespace native_registry_internal
} // namespace kungfu::runtime::kfx
