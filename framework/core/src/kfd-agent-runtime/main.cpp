// SPDX-License-Identifier: Apache-2.0

#include <kungfu/embedding.h>
#include <kungfu/native_storage.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>
#include <string_view>

namespace {

using json = nlohmann::json;

constexpr std::string_view REQUEST_CONTRACT = "kfd.agent-runtime-adapter-request/v1";
constexpr std::string_view RESPONSE_CONTRACT = "kfd.agent-runtime-adapter-response/v1";
constexpr std::string_view PROFILE = "kfd-agent-runtime@0.1.0-alpha.1";
constexpr uint64_t REQUIRED_STORAGE_CAPABILITIES =
    KF_NATIVE_STORAGE_CAP_EPISODE_LIFECYCLE | KF_NATIVE_STORAGE_CAP_HEAD_AND_HISTORICAL_QUERY |
    KF_NATIVE_STORAGE_CAP_FSCK | KF_NATIVE_STORAGE_CAP_EXPORT | KF_NATIVE_STORAGE_CAP_DOMAIN_FACT_ADMISSION |
    KF_NATIVE_STORAGE_CAP_TRUST_ASSESSMENT | KF_NATIVE_STORAGE_CAP_FACT_CUT_KERNEL |
    KF_NATIVE_STORAGE_CAP_EPISODE_RECOVERY | KF_NATIVE_STORAGE_CAP_IMPORT_AND_REBUILD |
    KF_NATIVE_STORAGE_CAP_BACKEND_LIFECYCLE | KF_NATIVE_STORAGE_CAP_FACT_LIBRARY;
constexpr uint64_t REQUIRED_EMBEDDING_CAPABILITIES =
    KF_EMBEDDING_CAP_READ_JOURNAL_BATCH | KF_EMBEDDING_CAP_STORAGE_DIAGNOSTICS | KF_EMBEDDING_CAP_GENERIC_CODEC |
    KF_EMBEDDING_CAP_STORAGE_MAINTENANCE_PLANS | KF_EMBEDDING_CAP_STORAGE_STATUS;

struct decision {
  std::string status;
  std::string code;
};

decision accept(std::string code) { return {"accepted", std::move(code)}; }
decision reject(std::string code) { return {"rejected", std::move(code)}; }

bool is_root(const json &value) {
  if (!value.is_string()) {
    return false;
  }
  const auto &text = value.get_ref<const std::string &>();
  if (text.size() != 71 || !text.starts_with("sha256:")) {
    return false;
  }
  return std::all_of(text.begin() + 7, text.end(),
                     [](unsigned char ch) { return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'); });
}

bool string_array_contains(const json &values, const json &candidate) {
  return values.is_array() &&
         std::any_of(values.begin(), values.end(), [&](const json &value) { return value == candidate; });
}

bool is_subset(const json &subset, const json &superset) {
  return subset.is_array() && superset.is_array() && std::all_of(subset.begin(), subset.end(), [&](const json &value) {
           return string_array_contains(superset, value);
         });
}

bool has_duplicates(const json &values) {
  if (!values.is_array()) {
    return false;
  }
  std::set<std::string> unique;
  for (const auto &value : values) {
    if (!value.is_string() || !unique.insert(value.get<std::string>()).second) {
      return true;
    }
  }
  return false;
}

bool is_contiguous(const json &values) {
  if (!values.is_array()) {
    return false;
  }
  for (size_t index = 0; index < values.size(); ++index) {
    if (!values[index].is_number_integer() || values[index].get<int64_t>() != static_cast<int64_t>(index)) {
      return false;
    }
  }
  return true;
}

uint64_t episode_id_for_request(std::string_view request_id) {
  uint64_t value = UINT64_C(14695981039346656037);
  for (const auto ch : request_id) {
    value ^= static_cast<unsigned char>(ch);
    value *= UINT64_C(1099511628211);
  }
  return UINT64_C(0x4b00000000000000) | (value & UINT64_C(0x00ffffffffffffff));
}

decision pursuit(const std::string &operation, const json &input) {
  const auto state = input.value("state", json{});
  if (operation == "pursuit.create") {
    if (!state.is_null() && !state.empty()) {
      return reject("pursuit-already-exists");
    }
    return input.value("target", json::object()).value("version", 0) == 1 ? accept("pursuit-created")
                                                                          : reject("pursuit-version-gap");
  }
  if (!state.is_object() || state.value("status", "") != "active") {
    return reject("pursuit-not-active");
  }
  if (input.value("baseVersion", -1) != state.value("version", -2)) {
    return reject("pursuit-stale-version");
  }
  if (operation == "pursuit.revise") {
    return input.value("targetVersion", -1) == state.value("version", -2) + 1 ? accept("pursuit-revised")
                                                                              : reject("pursuit-version-gap");
  }
  if (operation == "pursuit.fork") {
    const auto fork = input.value("fork", json::object());
    if (fork.value("id", "") == state.value("id", "")) {
      return reject("pursuit-identity-reuse");
    }
    return fork.value("version", 0) == 1 ? accept("pursuit-forked") : reject("pursuit-version-gap");
  }
  if (operation == "pursuit.settle") {
    if (!input.contains("completionVerdict") || input.value("completionVerdict", "").empty()) {
      return reject("completion-verdict-missing");
    }
    return input.value("completionVerdict", "") == "admitted" ? accept("pursuit-settled")
                                                              : reject("completion-verdict-not-admitted");
  }
  return reject("operation-unsupported");
}

decision atlas(const std::string &operation, const json &input) {
  const auto state = input.value("state", json{});
  if (operation == "atlas.cut") {
    const auto roots = input.value("sourceRoots", json::array());
    if (!roots.is_array() || roots.empty()) {
      return reject("atlas-source-roots-missing");
    }
    return has_duplicates(roots) ? reject("atlas-source-root-duplicate") : accept("atlas-cut-created");
  }
  if (!state.is_object()) {
    return reject("atlas-cut-missing");
  }
  if (operation == "atlas.mark-stale") {
    if (state.value("status", "") == "stale") {
      return reject("atlas-already-stale");
    }
    return input.value("reason", "").empty() ? reject("atlas-stale-reason-missing") : accept("atlas-marked-stale");
  }
  if (state.value("status", "") == "stale") {
    return reject("atlas-cut-stale");
  }
  if (input.value("baseCutRoot", "") != state.value("cutRoot", "")) {
    return reject("atlas-cut-mismatch");
  }
  if (operation == "atlas.derive") {
    return is_root(input.value("derivedRoot", json{})) ? accept("atlas-derived") : reject("atlas-derived-root-missing");
  }
  if (operation == "atlas.refresh") {
    if (input.value("sourceRoot", "") != state.value("sourceRoot", "")) {
      return reject("atlas-source-root-mismatch");
    }
    return is_root(input.value("nextCutRoot", json{})) ? accept("atlas-refreshed") : reject("atlas-next-cut-missing");
  }
  return reject("operation-unsupported");
}

decision warrant(const std::string &operation, const json &input) {
  const auto state = input.value("state", json{});
  if (operation == "warrant.issue") {
    return is_root(input.value("grant", json::object()).value("authorityRoot", json{}))
               ? accept("warrant-issued")
               : reject("warrant-authority-root-missing");
  }
  if (!state.is_object() || !is_root(state.value("authorityRoot", json{}))) {
    return reject("warrant-authority-root-missing");
  }
  if (operation == "warrant.revoke") {
    return input.value("authorityRoot", "") == state.value("authorityRoot", "")
               ? accept("warrant-revoked")
               : reject("warrant-revoker-unauthorized");
  }
  if (operation == "warrant.use") {
    if (state.value("status", "") == "revoked") {
      return reject("warrant-revoked");
    }
    if (!string_array_contains(state.value("allowedActions", json::array()), input.value("action", json{}))) {
      return reject("warrant-action-forbidden");
    }
    if (state.value("scope", "") != input.value("scope", "")) {
      return reject("warrant-scope-mismatch");
    }
    return input.value("now", INT64_C(0)) > state.value("expiresAt", INT64_C(0)) ? reject("warrant-expired")
                                                                                 : accept("warrant-authorized");
  }
  if (state.value("status", "") != "active") {
    return reject("warrant-not-active");
  }
  const auto grant = input.value("grant", json::object());
  const auto actions = grant.value("allowedActions", json::array());
  if (!actions.is_array() || actions.empty()) {
    return reject("warrant-actions-missing");
  }
  if (!is_subset(actions, state.value("allowedActions", json::array()))) {
    return reject("warrant-authority-amplification");
  }
  if (grant.value("scope", "") != state.value("scope", "")) {
    return reject("warrant-scope-amplification");
  }
  if (grant.value("expiresAt", INT64_C(0)) > state.value("expiresAt", INT64_C(0))) {
    return reject("warrant-expiry-amplification");
  }
  if (operation == "warrant.attenuate") {
    return accept("warrant-attenuated");
  }
  if (operation == "warrant.delegate") {
    return accept("warrant-delegated");
  }
  return reject("operation-unsupported");
}

decision action(const std::string &operation, const json &input) {
  if (operation == "action.bind") {
    const auto binding = input.value("binding", json::object());
    for (const auto &[field, code] :
         {std::pair{"pursuitRoot", "action-pursuit-root-missing"}, std::pair{"atlasRoot", "action-atlas-root-missing"},
          std::pair{"warrantRoot", "action-warrant-root-missing"}, std::pair{"actionRoot", "action-root-missing"}}) {
      if (!is_root(binding.value(field, json{}))) {
        return reject(code);
      }
    }
    if (!binding.value("preconditionsSatisfied", false)) {
      return reject("action-precondition-failed");
    }
    return binding.value("warrantActive", false) ? accept("action-bound") : reject("action-warrant-inactive");
  }
  if (operation != "action.assess") {
    return reject("operation-unsupported");
  }
  if (input.value("receiverVerdict", "") == "inferred-from-delivery") {
    return reject("delivery-is-not-admission");
  }
  if (input.value("completionVerdict", "") == "inferred-from-seal") {
    return reject("episode-is-not-completion");
  }
  if (input.value("factVerdict", "") == "inferred-from-call") {
    return reject("call-is-not-admission");
  }
  if (input.value("factVerdict", "") == "admitted" && input.value("verdictAuthority", "") == "producer") {
    return reject("producer-cannot-self-admit");
  }
  if (input.value("factVerdict", "") == "admitted") {
    return accept("receiver-verdict-retained");
  }
  if (input.value("transportDelivered", false)) {
    return accept("delivery-kept-separate");
  }
  if (input.value("episodeSealed", false)) {
    return accept("occurrence-kept-separate");
  }
  return input.value("callSucceeded", false) ? accept("call-kept-separate") : reject("action-assessment-empty");
}

decision episode_fact(const std::string &operation, const json &input) {
  const auto state = input.value("state", json{});
  if (operation == "episode.open") {
    return !state.is_null() && !state.empty() ? reject("episode-already-exists") : accept("episode-opened");
  }
  if (operation == "episode.append") {
    if (input.value("index", -1) != state.value("nextIndex", -2)) {
      return reject("episode-index-gap");
    }
    return is_root(input.value("claimRoot", json{})) ? accept("episode-appended")
                                                     : reject("episode-claim-root-missing");
  }
  if (operation == "episode.commit") {
    return is_root(input.value("contentRoot", json{})) ? accept("episode-committed")
                                                       : reject("episode-content-root-missing");
  }
  if (operation == "episode.interrupt") {
    return accept("episode-interrupted");
  }
  if (operation == "episode.seal") {
    return state.value("status", "") == "committed" ? accept("episode-sealed") : reject("episode-not-committed");
  }
  if (operation == "episode.replay") {
    return input.value("semanticRoot", "") == input.value("observedRoot", "") ? accept("episode-replayed")
                                                                              : reject("episode-root-mismatch");
  }
  if (operation == "fact.propose") {
    return accept("fact-proposed");
  }
  if (operation == "fact.admit") {
    return input.value("verdictAuthority", "") == "receiver" ? accept("fact-admitted")
                                                             : reject("fact-admitter-unauthorized");
  }
  if (operation == "fact.reject") {
    return accept("fact-rejected");
  }
  if (operation == "fact.conflict") {
    const auto roots = input.value("roots", json::array());
    std::set<std::string> distinct;
    for (const auto &root : roots) {
      if (root.is_string()) {
        distinct.insert(root.get<std::string>());
      }
    }
    return distinct.size() >= 2 ? accept("fact-conflicted") : reject("fact-conflict-roots-incomplete");
  }
  if (operation == "fact.supersede") {
    const auto status = state.value("status", "");
    return status == "admitted" || status == "conflicted" ? accept("fact-superseded") : reject("fact-not-admitted");
  }
  return reject("operation-unsupported");
}

decision recovery(const std::string &operation, const json &input) {
  const auto state = input.value("state", json::object());
  if (operation == "runtime.crash") {
    return state.value("acknowledgedSeq", INT64_C(0)) <= state.value("durableSeq", INT64_C(0))
               ? accept("runtime-crash-bounded")
               : reject("runtime-ack-ahead-of-durability");
  }
  if (operation == "runtime.reopen") {
    return input.value("observedProviderRoot", "") == state.value("providerRoot", "")
               ? accept("runtime-reopened")
               : reject("runtime-provider-root-mismatch");
  }
  if (operation == "runtime.fsck") {
    return state.value("expectedRoot", "") == state.value("observedRoot", "") ? accept("runtime-fsck-clean")
                                                                              : reject("runtime-root-mismatch");
  }
  if (operation == "runtime.export") {
    return is_root(input.value("exportedRoot", json{})) ? accept("runtime-exported")
                                                        : reject("runtime-export-root-missing");
  }
  if (operation == "runtime.import") {
    return input.value("declaredRoot", "") == input.value("observedRoot", "") ? accept("runtime-imported")
                                                                              : reject("runtime-import-root-mismatch");
  }
  if (operation == "runtime.replay") {
    return is_contiguous(input.value("indexes", json::array())) ? accept("runtime-replayed")
                                                                : reject("runtime-replay-gap");
  }
  if (operation == "runtime.retry") {
    const auto previous = input.value("previous", json::object());
    const auto next = input.value("next", json::object());
    return previous.value("key", "") == next.value("key", "") &&
                   previous.value("exchangeRoot", "") == next.value("exchangeRoot", "")
               ? accept("runtime-retry-idempotent")
               : reject("runtime-idempotency-reuse");
  }
  if (operation == "runtime.reconnect") {
    const auto roots = input.value("conflictRoots", json::array());
    return string_array_contains(roots, input.value("localRoot", json{})) &&
                   string_array_contains(roots, input.value("remoteRoot", json{}))
               ? accept("runtime-conflict-retained")
               : reject("runtime-conflict-hidden");
  }
  return reject("operation-unsupported");
}

decision evaluate(const json &request) {
  if (!request.is_object() || !request.contains("input") || !request["input"].is_object()) {
    return reject("adapter-input-invalid");
  }
  const auto &input = request["input"];
  const auto category = input.value("category", "");
  const auto operation = input.value("operation", "");
  const auto transition = input.value("input", json::object());
  if (category == "pursuit") {
    return pursuit(operation, transition);
  }
  if (category == "atlas") {
    return atlas(operation, transition);
  }
  if (category == "warrant") {
    return warrant(operation, transition);
  }
  if (category == "action") {
    return action(operation, transition);
  }
  if (category == "episode-fact") {
    return episode_fact(operation, transition);
  }
  if (category == "recovery") {
    return recovery(operation, transition);
  }
  return reject("category-unsupported");
}

class runtime_boundary {
public:
  runtime_boundary() = default;
  runtime_boundary(const runtime_boundary &) = delete;
  runtime_boundary &operator=(const runtime_boundary &) = delete;
  ~runtime_boundary() {
    if (embedding_context_ != nullptr) {
      embedding_api_.context_close(embedding_context_);
    }
    if (storage_context_ != nullptr) {
      storage_api_.context_close(storage_context_);
    }
  }

  bool open(std::string &code) {
    const auto *configured = std::getenv("KUNGFU_KFD_RUNTIME_DIR");
    if (configured == nullptr || configured[0] == '\0') {
      code = "runtime-dir-required";
      return false;
    }
    runtime_dir_ = std::filesystem::absolute(configured).lexically_normal().string();
    embedding_root_ = runtime_dir_ + ".embedding";
    std::error_code error;
    std::filesystem::create_directories(std::filesystem::path(runtime_dir_).parent_path(), error);
    if (error) {
      code = "runtime-parent-unavailable";
      return false;
    }

    if (kungfu_native_storage_get_api(KF_NATIVE_STORAGE_ABI_V1, sizeof(storage_api_), &storage_api_) !=
        KF_NATIVE_STORAGE_OK) {
      code = "native-storage-abi-unavailable";
      return false;
    }
    kf_native_storage_context_config_v1 storage_config{};
    storage_config.struct_size = sizeof(storage_config);
    storage_config.runtime_dir = runtime_dir_.c_str();
    if (storage_api_.context_open(&storage_config, &storage_context_) != KF_NATIVE_STORAGE_OK) {
      code = "native-storage-context-unavailable";
      return false;
    }
    if (storage_api_.context_capabilities(storage_context_, &storage_capabilities_) != KF_NATIVE_STORAGE_OK ||
        (storage_capabilities_ & REQUIRED_STORAGE_CAPABILITIES) != REQUIRED_STORAGE_CAPABILITIES) {
      code = "native-storage-capability-missing";
      return false;
    }

    if (kungfu_embedding_get_api(KF_EMBEDDING_ABI_V5, sizeof(embedding_api_), &embedding_api_) != KF_EMBEDDING_OK) {
      code = "embedding-abi-unavailable";
      return false;
    }
    if ((embedding_api_.capabilities & REQUIRED_EMBEDDING_CAPABILITIES) != REQUIRED_EMBEDDING_CAPABILITIES) {
      code = "embedding-api-capability-missing";
      return false;
    }
    kf_embedding_context_config_v1 embedding_config{};
    embedding_config.struct_size = sizeof(embedding_config);
    embedding_config.root = embedding_root_.c_str();
    embedding_config.host_namespace = "kungfu.kfd";
    embedding_config.host_name = "agent-runtime-adapter";
    embedding_config.mode = KF_EMBEDDING_MODE_LIVE;
    if (embedding_api_.context_open(&embedding_config, &embedding_context_) != KF_EMBEDDING_OK ||
        embedding_api_.context_capabilities(embedding_context_, &embedding_context_capabilities_) != KF_EMBEDDING_OK ||
        (embedding_context_capabilities_ & KF_EMBEDDING_CAP_READ_JOURNAL_BATCH) == 0) {
      code = "embedding-context-capability-missing";
      return false;
    }
    return true;
  }

  bool retain_accepted_transition(const std::string &request_id, const std::string &operation, std::string &code) {
    const auto episode_id = episode_id_for_request(request_id);
    ++accepted_count_;
    const auto begin = json{{"episode_id", episode_id},
                            {"begin_time", static_cast<int64_t>(accepted_count_ * 2)},
                            {"title", "KFD Agent Runtime accepted transition"},
                            {"actor", "kungfu-kfd-agent-runtime"},
                            {"source", operation}};
    if (!execute("episode_begin", begin)) {
      code = "runtime-episode-begin-failed";
      return false;
    }
    const auto end = json{{"episode_id", episode_id},
                          {"end_time", static_cast<int64_t>(accepted_count_ * 2 + 1)},
                          {"frame_count", 0},
                          {"reason", "KFD transition accepted"}};
    if (!execute("episode_end", end)) {
      code = "runtime-episode-end-failed";
      return false;
    }
    return true;
  }

  json observations() const {
    return {{"semanticBoundary", "preserved"},
            {"runtimeBoundary", "libkungfu-public-c-abi"},
            {"nativeStorageAbi", storage_api_.abi_version},
            {"embeddingAbi", embedding_api_.abi_version},
            {"nativeStorageCapabilities", storage_capabilities_},
            {"embeddingApiCapabilities", embedding_api_.capabilities},
            {"embeddingContextCapabilities", embedding_context_capabilities_}};
  }

private:
  bool execute(const char *operation, const json &request) {
    const auto payload = request.dump();
    kf_native_storage_result_v1 result{};
    result.struct_size = sizeof(result);
    if (storage_api_.execute(storage_context_, operation, payload.data(), payload.size(), &result) !=
            KF_NATIVE_STORAGE_OK ||
        result.token == 0 || result.json_data == nullptr) {
      return false;
    }
    return storage_api_.release_result(storage_context_, result.token) == KF_NATIVE_STORAGE_OK;
  }

  std::string runtime_dir_;
  std::string embedding_root_;
  kf_native_storage_api_v1 storage_api_{};
  kf_native_storage_context *storage_context_ = nullptr;
  uint64_t storage_capabilities_ = 0;
  kf_embedding_api_v5 embedding_api_{};
  kf_embedding_context *embedding_context_ = nullptr;
  uint64_t embedding_context_capabilities_ = 0;
  uint64_t accepted_count_ = 0;
};

json response(const std::string &request_id, const decision &result, const json &observations) {
  return {{"schemaVersion", 1},
          {"contract", RESPONSE_CONTRACT},
          {"requestId", request_id},
          {"adapter",
           {{"id", "kungfu-libkungfu-kfd-agent-runtime"},
            {"version", "0.1.0"},
            {"topology", "in-process-libkungfu-public-c-abi"}}},
          {"status", result.status},
          {"code", result.code},
          {"observations", observations}};
}

} // namespace

int main() {
  runtime_boundary boundary;
  bool ready = false;
  std::string readiness_code = "handshake-required";
  for (std::string line; std::getline(std::cin, line);) {
    json envelope;
    std::string request_id = "invalid";
    try {
      envelope = json::parse(line);
      request_id = envelope.value("requestId", "invalid");
      if (envelope.value("schemaVersion", 0) != 1 || envelope.value("contract", "") != REQUEST_CONTRACT ||
          request_id == "invalid") {
        throw std::runtime_error("invalid request envelope");
      }
      const auto operation = envelope.value("operation", "");
      if (operation == "handshake") {
        if (!ready) {
          ready = boundary.open(readiness_code);
        }
        const decision result = ready ? accept("adapter-ready") : decision{"error", readiness_code};
        auto observations = ready ? boundary.observations() : json{{"failClosed", true}};
        observations["profile"] = PROFILE;
        observations["protocol"] = "jsonl-stdio/v1";
        observations["topology"] = "in-process-libkungfu-public-c-abi";
        std::cout << response(request_id, result, observations).dump() << '\n';
        continue;
      }
      if (operation != "evaluate") {
        std::cout << response(request_id, reject("adapter-operation-unsupported"), {{"failClosed", true}}).dump()
                  << '\n';
        continue;
      }
      if (!ready) {
        std::cout << response(request_id, {"error", readiness_code}, {{"failClosed", true}}).dump() << '\n';
        continue;
      }
      auto result = evaluate(envelope);
      if (result.status == "accepted") {
        std::string storage_code;
        const auto kfd_operation = envelope.value("input", json::object()).value("operation", "");
        if (!boundary.retain_accepted_transition(request_id, kfd_operation, storage_code)) {
          result = {"error", storage_code};
        }
      }
      const auto observations = result.status == "accepted" ? boundary.observations() : json{{"failClosed", true}};
      std::cout << response(request_id, result, observations).dump() << '\n';
    } catch (const std::exception &) {
      std::cout << response(request_id, {"error", "adapter-request-invalid"}, {{"failClosed", true}}).dump() << '\n';
    }
  }
  return 0;
}
