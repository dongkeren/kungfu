// SPDX-License-Identifier: Apache-2.0

#include <kungfu/runtime/query/fact_query.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <map>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <kungfu/runtime/facts/fact_admission.h>
#include <kungfu/runtime/storage/episode_manifest_projection.h>
#include <kungfu/yijinjing/storage/content_hash.h>
#include <kungfu/yijinjing/storage/episode_manifest.h>

#include "fact_query_internal.h"

namespace kungfu::runtime::query {

using namespace fact_query_internal;

namespace {

void parse_query_shape(const nlohmann::json &value, query_definition &definition) {
  definition.schema = optional_text(value, "schema", QUERY_DEFINITION_SCHEMA_V1);
  if (definition.schema != QUERY_DEFINITION_SCHEMA_V1) {
    throw std::invalid_argument("unsupported query definition schema: " + definition.schema);
  }
  definition.object = optional_text(value, "object", "episodes");
  if (definition.object != "episodes" && definition.object != "fact-state") {
    throw std::invalid_argument(
        "KF-ADR-019f86da-4f90-7e38-b72f-ef8829e14104 supports only object=episodes or object=fact-state");
  }
  if (value.contains("subject_keys")) {
    if (!value.at("subject_keys").is_array() || value.at("subject_keys").size() > 256) {
      throw std::invalid_argument("definition.subject_keys must be an array with at most 256 entries");
    }
    for (const auto &item : value.at("subject_keys")) {
      if (!item.is_string() || item.get<std::string>().empty() || item.get<std::string>().size() > 512) {
        throw std::invalid_argument("definition.subject_keys entries must contain 1..512 bytes");
      }
      definition.subject_keys.push_back(item.get<std::string>());
    }
    std::sort(definition.subject_keys.begin(), definition.subject_keys.end());
    definition.subject_keys.erase(std::unique(definition.subject_keys.begin(), definition.subject_keys.end()),
                                  definition.subject_keys.end());
  }
  if (definition.object == "episodes" && !definition.subject_keys.empty()) {
    throw std::invalid_argument("definition.subject_keys is supported only for object=fact-state");
  }
  definition.limit = optional_uint64(value, "limit", 100);
  if (definition.limit == 0 || definition.limit > 1000) {
    throw std::invalid_argument("KF-ADR-019f86da-4f90-7e38-b72f-ef8829e14104 Q0 requires 1 <= limit <= 1000");
  }
  definition.evidence = optional_text(value, "evidence", "proof");
  if (definition.evidence != "proof") {
    throw std::invalid_argument("KF-ADR-019f86da-4f90-7e38-b72f-ef8829e14104 Q1 supports only evidence=proof");
  }
}

void parse_temporal_definition(const nlohmann::json &value, query_definition &definition) {
  if (value.contains("temporal_pattern")) {
    const auto &pattern = value.at("temporal_pattern");
    reject_unknown_fields(
        pattern, {"schema", "partition_by", "order_by", "sequence", "repeat", "within_ns", "as_of_time", "absence"},
        "definition.temporal_pattern");
    for (const auto *required :
         {"schema", "partition_by", "order_by", "sequence", "repeat", "within_ns", "as_of_time"}) {
      if (!pattern.contains(required)) {
        throw std::invalid_argument(std::string("definition.temporal_pattern requires field: ") + required);
      }
    }
    definition.has_temporal_pattern = true;
    definition.pattern.schema = optional_text(pattern, "schema", QUERY_TEMPORAL_PATTERN_SCHEMA_V1);
    if (definition.pattern.schema != QUERY_TEMPORAL_PATTERN_SCHEMA_V1) {
      throw std::invalid_argument("unsupported temporal pattern schema: " + definition.pattern.schema);
    }
    definition.pattern.partition_by = optional_text(pattern, "partition_by", "source");
    definition.pattern.order_by = optional_text(pattern, "order_by", "begin_time");
    static const std::regex field_name("^[a-z][a-z0-9_]{0,63}$");
    if (!std::regex_match(definition.pattern.partition_by, field_name) ||
        !std::regex_match(definition.pattern.order_by, field_name)) {
      throw std::invalid_argument("temporal pattern partition_by/order_by must be safe field names");
    }
    static constexpr const char *PARTITION_FIELDS[] = {
        "episode_id", "title", "actor", "source", "status", "reason", "content_root_status"};
    if (std::find(std::begin(PARTITION_FIELDS), std::end(PARTITION_FIELDS), definition.pattern.partition_by) ==
        std::end(PARTITION_FIELDS)) {
      throw std::invalid_argument("temporal pattern partition_by uses unsupported Episode field");
    }
    if (definition.pattern.order_by != "begin_time" && definition.pattern.order_by != "end_time") {
      throw std::invalid_argument("temporal pattern order_by must be begin_time or end_time");
    }
    if (!pattern.contains("sequence") || !pattern.at("sequence").is_array() || pattern.at("sequence").size() != 2) {
      throw std::invalid_argument("temporal pattern sequence requires exactly two predicates");
    }
    definition.pattern.sequence.push_back(
        parse_event_predicate(pattern.at("sequence").at(0), "definition.temporal_pattern.sequence[0]"));
    definition.pattern.sequence.push_back(
        parse_event_predicate(pattern.at("sequence").at(1), "definition.temporal_pattern.sequence[1]"));
    const auto repeat = pattern.value("repeat", nlohmann::json::object());
    reject_unknown_fields(repeat, {"min", "max"}, "definition.temporal_pattern.repeat");
    definition.pattern.repeat_min = optional_uint64(repeat, "min", 1);
    definition.pattern.repeat_max = optional_uint64(repeat, "max", definition.pattern.repeat_min);
    if (definition.pattern.repeat_min == 0 || definition.pattern.repeat_max < definition.pattern.repeat_min ||
        definition.pattern.repeat_max > 16) {
      throw std::invalid_argument("temporal pattern repeat requires 1 <= min <= max <= 16");
    }
    definition.pattern.within_ns = optional_uint64(pattern, "within_ns");
    constexpr uint64_t MAX_PATTERN_WINDOW_NS = 30ULL * 24ULL * 60ULL * 60ULL * 1000000000ULL;
    if (definition.pattern.within_ns == 0 || definition.pattern.within_ns > MAX_PATTERN_WINDOW_NS) {
      throw std::invalid_argument("temporal pattern within_ns must be between 1ns and 30 days");
    }
    if (!pattern.contains("as_of_time")) {
      throw std::invalid_argument("temporal pattern requires explicit as_of_time");
    }
    definition.pattern.as_of_time = int64_value(pattern.at("as_of_time"), "temporal_pattern.as_of_time");
    if (definition.pattern.as_of_time <= 0) {
      throw std::invalid_argument("temporal pattern as_of_time must be positive");
    }
    if (pattern.contains("absence")) {
      definition.pattern.has_absence = true;
      definition.pattern.absence = parse_event_predicate(pattern.at("absence"), "definition.temporal_pattern.absence");
    }
  }
}

void parse_query_basis(const nlohmann::json &value, query_definition &definition) {
  const auto basis = value.value("basis", nlohmann::json::object());
  reject_unknown_fields(
      basis, {"contract_world", "fact_surfaces", "scope", "episode_id", "perspective", "cut", "policy", "time_basis"},
      "definition.basis");
  if (!basis.contains("contract_world") || !basis.contains("fact_surfaces")) {
    throw std::invalid_argument("definition.basis requires explicit contract_world and fact_surfaces declarations");
  }
  definition.basis.contract_world =
      parse_declaration_reference(basis.at("contract_world"), "definition.basis.contract_world");
  const auto &fact_surfaces = basis.at("fact_surfaces");
  if (!fact_surfaces.is_array() || fact_surfaces.empty() || fact_surfaces.size() > 16) {
    throw std::invalid_argument("definition.basis.fact_surfaces requires 1..16 declaration references");
  }
  for (size_t index = 0; index < fact_surfaces.size(); ++index) {
    const auto path = std::string("definition.basis.fact_surfaces[") + std::to_string(index) + "]";
    definition.basis.fact_surfaces.push_back(parse_declaration_reference(fact_surfaces.at(index), path.c_str()));
  }
  const auto expected_scope = definition.object == "episodes" ? "episode-manifest" : "domain-fact-ledger";
  definition.basis.scope = optional_text(basis, "scope", expected_scope);
  if (definition.basis.scope != expected_scope) {
    throw std::invalid_argument("query basis scope does not match the selected object");
  }
  definition.basis.episode_id = optional_uint64(basis, "episode_id");
  if (definition.object == "fact-state" && definition.basis.episode_id != 0) {
    throw std::invalid_argument("object=fact-state does not accept basis.episode_id");
  }
  const auto expected_perspective =
      definition.object == "episodes" ? "manifest-append-order" : "system-time-then-observation-id";
  definition.basis.perspective = optional_text(basis, "perspective", expected_perspective);
  if (definition.basis.perspective != expected_perspective) {
    throw std::invalid_argument("query basis perspective does not match the selected object");
  }

  const auto selected_cut = basis.value("cut", nlohmann::json{{"kind", "head"}});
  reject_unknown_fields(selected_cut, {"kind", "manifest_frame_uid", "system_time"}, "definition.basis.cut");
  const auto cut_name = optional_text(selected_cut, "kind", "head");
  if (cut_name == "head") {
    if (selected_cut.contains("manifest_frame_uid") || selected_cut.contains("system_time")) {
      throw std::invalid_argument("head cut must not include a cut token");
    }
    definition.basis.selected_cut.kind = cut_kind::Head;
  } else if (cut_name == "manifest_frame_uid") {
    if (definition.object != "episodes") {
      throw std::invalid_argument("manifest_frame_uid cuts are supported only for object=episodes");
    }
    definition.basis.selected_cut.kind = cut_kind::ManifestFrameUid;
    definition.basis.selected_cut.manifest_frame_uid = optional_uint64(selected_cut, "manifest_frame_uid");
    if (definition.basis.selected_cut.manifest_frame_uid == 0) {
      throw std::invalid_argument("manifest_frame_uid cut requires a non-zero token");
    }
    if (selected_cut.contains("system_time")) {
      throw std::invalid_argument("manifest_frame_uid cut must not include system_time");
    }
  } else if (cut_name == "system_time") {
    if (definition.object != "fact-state" || !selected_cut.contains("system_time")) {
      throw std::invalid_argument("system_time cuts require object=fact-state and a system_time token");
    }
    definition.basis.selected_cut.kind = cut_kind::SystemTime;
    definition.basis.selected_cut.system_time = int64_value(selected_cut.at("system_time"), "cut.system_time");
    if (definition.basis.selected_cut.system_time <= 0) {
      throw std::invalid_argument("system_time cut requires a positive token");
    }
    if (selected_cut.contains("manifest_frame_uid")) {
      throw std::invalid_argument("system_time cut must not include manifest_frame_uid");
    }
  } else {
    throw std::invalid_argument("unsupported query cut: " + cut_name);
  }

  const auto policy = basis.value("policy", nlohmann::json::object());
  reject_unknown_fields(policy, {"fold", "schema", "engine", "conflict", "redaction"}, "definition.basis.policy");
  definition.basis.policy.fold = optional_text(policy, "fold", definition.basis.policy.fold);
  definition.basis.policy.schema = optional_text(policy, "schema", definition.basis.policy.schema);
  definition.basis.policy.engine = optional_text(policy, "engine", definition.basis.policy.engine);
  definition.basis.policy.conflict = optional_text(policy, "conflict", definition.basis.policy.conflict);
  definition.basis.policy.redaction = optional_text(policy, "redaction", definition.basis.policy.redaction);
  const auto time_basis = basis.value("time_basis", nlohmann::json::object());
  reject_unknown_fields(time_basis, {"valid_time", "system_time", "causal_time"}, "definition.basis.time_basis");
  definition.basis.valid_time = optional_text(time_basis, "valid_time", definition.basis.valid_time);
  definition.basis.system_time = optional_text(time_basis, "system_time", definition.basis.system_time);
  definition.basis.causal_time = optional_text(time_basis, "causal_time", definition.basis.causal_time);
  const query_policy supported_policy =
      definition.object == "episodes"
          ? query_policy{}
          : query_policy{"latest-admitted-per-source/v1", "kungfu.facts.domain-fact-event/v1", "fact-authority-scan/v1",
                         "preserve-source-claims/v1", "hash-and-ref/v1"};
  if (definition.basis.policy.fold != supported_policy.fold ||
      definition.basis.policy.schema != supported_policy.schema ||
      definition.basis.policy.engine != supported_policy.engine ||
      definition.basis.policy.conflict != supported_policy.conflict ||
      definition.basis.policy.redaction != supported_policy.redaction) {
    throw std::invalid_argument("unsupported KF-ADR-019f86da-4f90-7e38-b72f-ef8829e14104 Q0 policy version");
  }
  const query_basis supported_basis =
      definition.object == "episodes"
          ? query_basis{}
          : query_basis{
                {},         {},      "domain-fact-ledger", 0, "system-time-then-observation-id", {}, supported_policy,
                "explicit", "event", "event-parent"};
  if (definition.basis.valid_time != supported_basis.valid_time ||
      definition.basis.system_time != supported_basis.system_time ||
      definition.basis.causal_time != supported_basis.causal_time) {
    throw std::invalid_argument("unsupported KF-ADR-019f86da-4f90-7e38-b72f-ef8829e14104 Q0 time basis");
  }
  if (definition.has_temporal_pattern && definition.object != "episodes") {
    throw std::invalid_argument("temporal pattern requires object=episodes");
  }
  if (definition.has_temporal_pattern && definition.basis.episode_id != 0) {
    throw std::invalid_argument("temporal pattern requires basis.episode_id=0 so the partition can span Episodes");
  }
}

} // namespace

query_definition parse_query_definition(const nlohmann::json &value) {
  if (!value.is_object()) {
    throw std::invalid_argument("query definition must be an object");
  }
  reject_unknown_fields(value, {"schema", "basis", "object", "subject_keys", "limit", "evidence", "temporal_pattern"},
                        "definition");
  query_definition definition;
  parse_query_shape(value, definition);
  parse_temporal_definition(value, definition);
  parse_query_basis(value, definition);
  return definition;
}

nlohmann::json query_definition_json(const query_definition &definition) {
  auto normalized = definition;
  normalize_declaration_basis(normalized.basis);
  auto fact_surfaces = nlohmann::json::array();
  for (const auto &surface : normalized.basis.fact_surfaces) {
    fact_surfaces.push_back(declaration_reference_json(surface));
  }
  nlohmann::json value = {{"schema", normalized.schema},
                          {"basis",
                           {{"contract_world", declaration_reference_json(normalized.basis.contract_world)},
                            {"fact_surfaces", fact_surfaces},
                            {"scope", normalized.basis.scope},
                            {"episode_id", std::to_string(normalized.basis.episode_id)},
                            {"perspective", normalized.basis.perspective},
                            {"cut", cut_json(normalized.basis.selected_cut)},
                            {"policy", policy_json(normalized.basis.policy)},
                            {"time_basis",
                             {{"valid_time", normalized.basis.valid_time},
                              {"system_time", normalized.basis.system_time},
                              {"causal_time", normalized.basis.causal_time}}}}},
                          {"object", normalized.object},
                          {"limit", normalized.limit},
                          {"evidence", normalized.evidence}};
  if (!normalized.subject_keys.empty()) {
    value["subject_keys"] = normalized.subject_keys;
  }
  if (normalized.has_temporal_pattern) {
    value["temporal_pattern"] = temporal_pattern_json(normalized.pattern);
  }
  return value;
}

logical_plan plan_query(const query_definition &definition) {
  logical_plan plan;
  plan.definition = definition;
  normalize_declaration_basis(plan.definition.basis);
  const auto &normalized = plan.definition;
  plan.row_schema = normalized.object == "fact-state"
                        ? fact_state_result_schema()
                        : (normalized.has_temporal_pattern ? temporal_match_result_schema() : episode_result_schema());
  plan.query_definition_hash = json_hash(query_definition_json(normalized));
  plan.operators.push_back({"authority_scan", authority_scan_operator{normalized.object, normalized.basis}});
  if (normalized.basis.episode_id != 0) {
    plan.operators.push_back(
        {"filter", scalar_filter_operator{"episode_id", "eq", std::to_string(normalized.basis.episode_id)}});
  }
  if (!normalized.subject_keys.empty()) {
    plan.operators.push_back({"filter", set_filter_operator{"subject_key", "in", normalized.subject_keys}});
  }
  if (normalized.has_temporal_pattern) {
    plan.operators.push_back({"temporal_match", normalized.pattern});
  } else if (normalized.object == "episodes") {
    plan.operators.push_back({"order", order_operator{"episode_id", "asc"}});
  } else {
    plan.operators.push_back({"order", order_operator{"observation_id", "asc"}});
  }
  plan.operators.push_back({"limit", limit_operator{normalized.limit}});
  plan.operators.push_back({"project", project_operator{plan.row_schema}});
  plan.operators.push_back({"evidence", evidence_operator{normalized.evidence}});
  plan.logical_plan_hash = json_hash(logical_plan_payload_json(plan));
  return plan;
}

namespace yy_storage = kungfu::yijinjing::storage;

namespace fact_query_internal {

void reject_unknown_fields(const nlohmann::json &object, std::initializer_list<const char *> allowed,
                           const char *path) {
  if (!object.is_object()) {
    throw std::invalid_argument(std::string(path) + " must be an object");
  }
  for (const auto &[field, value] : object.items()) {
    (void)value;
    const auto known =
        std::any_of(allowed.begin(), allowed.end(), [&field](const char *candidate) { return field == candidate; });
    if (!known) {
      throw std::invalid_argument(std::string("unsupported query field: ") + path + "." + field);
    }
  }
}

uint64_t uint64_value(const nlohmann::json &value, const char *field) {
  if (value.is_number_unsigned()) {
    return value.get<uint64_t>();
  }
  if (value.is_number_integer()) {
    const auto signed_value = value.get<int64_t>();
    if (signed_value >= 0) {
      return static_cast<uint64_t>(signed_value);
    }
  }
  if (value.is_string()) {
    const auto text = value.get<std::string>();
    uint64_t parsed = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (error == std::errc{} && end == text.data() + text.size()) {
      return parsed;
    }
  }
  throw std::invalid_argument(std::string("invalid unsigned query field: ") + field);
}

uint64_t optional_uint64(const nlohmann::json &object, const char *field, uint64_t fallback) {
  if (!object.is_object() || !object.contains(field) || object.at(field).is_null()) {
    return fallback;
  }
  return uint64_value(object.at(field), field);
}

int64_t int64_value(const nlohmann::json &value, const char *field) {
  if (value.is_number_integer()) {
    return value.get<int64_t>();
  }
  if (value.is_number_unsigned()) {
    const auto parsed = value.get<uint64_t>();
    if (parsed <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
      return static_cast<int64_t>(parsed);
    }
  }
  if (value.is_string()) {
    const auto text = value.get<std::string>();
    int64_t parsed = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (error == std::errc{} && end == text.data() + text.size()) {
      return parsed;
    }
  }
  throw std::invalid_argument(std::string("invalid signed query field: ") + field);
}

std::string optional_text(const nlohmann::json &object, const char *field, const std::string &fallback) {
  if (!object.is_object() || !object.contains(field) || object.at(field).is_null()) {
    return fallback;
  }
  if (!object.at(field).is_string()) {
    throw std::invalid_argument(std::string("invalid text query field: ") + field);
  }
  return object.at(field).get<std::string>();
}

std::string ascii_lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return value;
}

event_predicate parse_event_predicate(const nlohmann::json &value, const char *path) {
  reject_unknown_fields(value, {"field", "equals"}, path);
  event_predicate predicate{optional_text(value, "field"), optional_text(value, "equals")};
  static const std::regex field_name("^[a-z][a-z0-9_]{0,63}$");
  if (!std::regex_match(predicate.field, field_name) || predicate.equals.empty() || predicate.equals.size() > 256) {
    throw std::invalid_argument(std::string(path) + " requires a safe field and 1..256 byte equals value");
  }
  static constexpr const char *SUPPORTED_FIELDS[] = {
      "episode_id", "title",        "actor",       "source",    "status",         "begin_time",         "end_time",
      "reason",     "record_count", "frame_count", "ref_count", "last_frame_uid", "content_root_status"};
  if (std::find(std::begin(SUPPORTED_FIELDS), std::end(SUPPORTED_FIELDS), predicate.field) ==
      std::end(SUPPORTED_FIELDS)) {
    throw std::invalid_argument(std::string(path) + " uses unsupported Episode field: " + predicate.field);
  }
  return predicate;
}

nlohmann::json event_predicate_json(const event_predicate &predicate) {
  return {{"field", predicate.field}, {"equals", predicate.equals}};
}

nlohmann::json temporal_pattern_json(const temporal_pattern &pattern) {
  auto sequence = nlohmann::json::array();
  for (const auto &step : pattern.sequence) {
    sequence.push_back(event_predicate_json(step));
  }
  nlohmann::json value = {{"schema", pattern.schema},
                          {"partition_by", pattern.partition_by},
                          {"order_by", pattern.order_by},
                          {"sequence", sequence},
                          {"repeat", {{"min", pattern.repeat_min}, {"max", pattern.repeat_max}}},
                          {"within_ns", std::to_string(pattern.within_ns)},
                          {"as_of_time", std::to_string(pattern.as_of_time)}};
  if (pattern.has_absence) {
    value["absence"] = event_predicate_json(pattern.absence);
  }
  return value;
}

nlohmann::json cut_json(const cut &value) {
  if (value.kind == cut_kind::Head) {
    return {{"kind", "head"}};
  }
  if (value.kind == cut_kind::SystemTime) {
    return {{"kind", "system_time"}, {"system_time", std::to_string(value.system_time)}};
  }
  return {{"kind", "manifest_frame_uid"}, {"manifest_frame_uid", std::to_string(value.manifest_frame_uid)}};
}

nlohmann::json policy_json(const query_policy &policy) {
  return {{"fold", policy.fold},
          {"schema", policy.schema},
          {"engine", policy.engine},
          {"conflict", policy.conflict},
          {"redaction", policy.redaction}};
}

nlohmann::json result_schema_json(const result_schema &schema) {
  auto fields = nlohmann::json::array();
  for (const auto &field : schema.fields) {
    if (field.externally_declared) {
      fields.push_back({{"name", field.name}, {"type", field.type}, {"nullable", field.nullable}});
    }
  }
  return {{"schema", schema.schema}, {"fields", fields}};
}

nlohmann::json declaration_reference_json(const declaration_reference &reference);

nlohmann::json query_value_json(const query_value &value) {
  return std::visit(
      [](const auto &item) -> nlohmann::json {
        using item_type = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<item_type, missing_value>) {
          throw std::invalid_argument("missing query value cannot be rendered outside a RowSchema");
        } else if constexpr (std::is_same_v<item_type, query_array>) {
          auto values = nlohmann::json::array();
          for (const auto &entry : item) {
            values.push_back(query_value_json(entry));
          }
          return values;
        } else if constexpr (std::is_same_v<item_type, query_object>) {
          auto values = nlohmann::json::object();
          for (const auto &[key, entry] : item) {
            values[key] = query_value_json(entry);
          }
          return values;
        } else {
          return item;
        }
      },
      value.data);
}

query_value query_value_from_json(const nlohmann::json &value) {
  if (value.is_null()) {
    return nullptr;
  }
  if (value.is_boolean()) {
    return value.get<bool>();
  }
  if (value.is_number_unsigned()) {
    return value.get<uint64_t>();
  }
  if (value.is_number_integer()) {
    return value.get<int64_t>();
  }
  if (value.is_number_float()) {
    return value.get<double>();
  }
  if (value.is_string()) {
    return value.get<std::string>();
  }
  if (value.is_array()) {
    query_array result;
    result.reserve(value.size());
    for (const auto &entry : value) {
      result.push_back(query_value_from_json(entry));
    }
    return result;
  }
  if (value.is_object()) {
    query_object result;
    for (const auto &[key, entry] : value.items()) {
      result.emplace(key, query_value_from_json(entry));
    }
    return result;
  }
  throw std::invalid_argument("query row contains an unsupported value");
}

bool value_matches_type(const nlohmann::json &value, const std::string &type) {
  if (value.is_null()) {
    return true;
  }
  if (type == "string") {
    return value.is_string();
  }
  if (type == "boolean") {
    return value.is_boolean();
  }
  if (type == "uint64") {
    return value.is_number_unsigned() || (value.is_number_integer() && value.get<int64_t>() >= 0) || value.is_string();
  }
  if (type == "int64") {
    return value.is_number_integer() || value.is_number_unsigned() || value.is_string();
  }
  if (type == "object") {
    return value.is_object();
  }
  if (type.starts_with("array<")) {
    return value.is_array();
  }
  return false;
}

dynamic_row make_dynamic_row(const result_schema &schema, const nlohmann::json &value) {
  if (!value.is_object()) {
    throw std::invalid_argument("query row fields do not exactly match RowSchema");
  }
  std::set<std::string> schema_fields;
  for (const auto &field : schema.fields) {
    schema_fields.insert(field.name);
  }
  for (const auto &[name, unused] : value.items()) {
    (void)unused;
    if (!schema_fields.contains(name)) {
      throw std::invalid_argument("query row field is not declared by RowSchema: " + name);
    }
  }
  dynamic_row row;
  row.values.reserve(schema.fields.size());
  for (const auto &field : schema.fields) {
    if (!value.contains(field.name)) {
      if (!field.nullable) {
        throw std::invalid_argument("query row is missing non-nullable RowSchema field: " + field.name);
      }
      row.values.emplace_back(missing_value{});
      continue;
    }
    const auto &field_value = value.at(field.name);
    if (field_value.is_null() && !field.nullable) {
      throw std::invalid_argument("query row has null for non-nullable field: " + field.name);
    }
    if (!value_matches_type(field_value, field.type)) {
      throw std::invalid_argument("query row field has wrong type: " + field.name);
    }
    if (!field_value.is_null() && field.type == "uint64") {
      (void)uint64_value(field_value, field.name.c_str());
    } else if (!field_value.is_null() && field.type == "int64") {
      (void)int64_value(field_value, field.name.c_str());
    }
    row.values.push_back(query_value_from_json(field_value));
  }
  return row;
}

nlohmann::json dynamic_row_json(const result_schema &schema, const dynamic_row &row) {
  if (row.values.size() != schema.fields.size()) {
    throw std::invalid_argument("typed query row width does not match RowSchema");
  }
  auto value = nlohmann::json::object();
  for (size_t index = 0; index < schema.fields.size(); ++index) {
    if (!std::holds_alternative<missing_value>(row.values[index].data)) {
      value[schema.fields[index].name] = query_value_json(row.values[index]);
    }
  }
  return value;
}

nlohmann::json dynamic_rows_json(const result_schema &schema, const std::vector<dynamic_row> &rows) {
  auto values = nlohmann::json::array();
  for (const auto &row : rows) {
    values.push_back(dynamic_row_json(schema, row));
  }
  return values;
}

nlohmann::json logical_operator_json(const logical_operator &operation) {
  const auto arguments = std::visit(
      [](const auto &value) -> nlohmann::json {
        using value_type = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<value_type, authority_scan_operator>) {
          auto fact_surfaces = nlohmann::json::array();
          for (const auto &surface : value.basis.fact_surfaces) {
            fact_surfaces.push_back(declaration_reference_json(surface));
          }
          return {{"object", value.object},
                  {"contract_world", declaration_reference_json(value.basis.contract_world)},
                  {"fact_surfaces", fact_surfaces},
                  {"scope", value.basis.scope},
                  {"perspective", value.basis.perspective},
                  {"cut", cut_json(value.basis.selected_cut)},
                  {"policy", policy_json(value.basis.policy)},
                  {"time_basis",
                   {{"valid_time", value.basis.valid_time},
                    {"system_time", value.basis.system_time},
                    {"causal_time", value.basis.causal_time}}}};
        } else if constexpr (std::is_same_v<value_type, scalar_filter_operator>) {
          return {{"field", value.field}, {"operator", value.operation}, {"value", value.value}};
        } else if constexpr (std::is_same_v<value_type, set_filter_operator>) {
          return {{"field", value.field}, {"operator", value.operation}, {"values", value.values}};
        } else if constexpr (std::is_same_v<value_type, temporal_pattern>) {
          return temporal_pattern_json(value);
        } else if constexpr (std::is_same_v<value_type, order_operator>) {
          return {{"field", value.field}, {"direction", value.direction}};
        } else if constexpr (std::is_same_v<value_type, limit_operator>) {
          return {{"count", value.count}};
        } else if constexpr (std::is_same_v<value_type, project_operator>) {
          auto fields = nlohmann::json::array();
          for (const auto &field : value.schema.fields) {
            if (field.externally_declared) {
              fields.push_back(field.name);
            }
          }
          return {{"schema", value.schema.schema}, {"fields", fields}};
        } else {
          return {{"level", value.level}};
        }
      },
      operation.arguments);
  return {{"kind", operation.kind}, {"arguments", arguments}};
}

nlohmann::json logical_plan_payload_json(const logical_plan &plan) {
  auto operators = nlohmann::json::array();
  for (const auto &operation : plan.operators) {
    operators.push_back(logical_operator_json(operation));
  }
  return {{"schema", plan.schema},
          {"query_definition_hash", plan.query_definition_hash},
          {"operators", operators},
          {"result_schema", result_schema_json(plan.row_schema)}};
}

std::string json_hash(const nlohmann::json &value) {
  return yy_storage::format_content_hash(yy_storage::compute_content_hash(value.dump(-1, ' ', false)));
}

constexpr const char *EPISODE_CONTRACT_WORLD_ID = "kungfu.runtime";
constexpr const char *EPISODE_CONTRACT_WORLD_VERSION = "1";
constexpr const char *EPISODE_FACT_SURFACE_ID = "kungfu.runtime.episode-manifest";
constexpr const char *EPISODE_FACT_SURFACE_VERSION = "1";

nlohmann::json episode_contract_world_declaration_payload() {
  return {{"schema", "kungfu.kfd.contract-world-declaration/v1"},
          {"id", EPISODE_CONTRACT_WORLD_ID},
          {"version", EPISODE_CONTRACT_WORLD_VERSION},
          {"fact_surfaces", nlohmann::json::array({EPISODE_FACT_SURFACE_ID})},
          {"effective_system_time", "built-in"}};
}

declaration_reference episode_contract_world_reference() {
  return {EPISODE_CONTRACT_WORLD_ID, EPISODE_CONTRACT_WORLD_VERSION,
          json_hash(episode_contract_world_declaration_payload())};
}

nlohmann::json episode_fact_surface_declaration_payload() {
  const auto contract_world = episode_contract_world_reference();
  return {{"schema", "kungfu.kfd.fact-surface-declaration/v1"},
          {"id", EPISODE_FACT_SURFACE_ID},
          {"version", EPISODE_FACT_SURFACE_VERSION},
          {"contract_world",
           {{"id", contract_world.id}, {"version", contract_world.version}, {"root", contract_world.root}}},
          {"schema_owner", yy_storage::EPISODE_MANIFEST_SCHEMA_V1},
          {"source_authority", "yijinjing-episode-manifest-journal"},
          {"identity", "episode-id"},
          {"valid_time", "not-projected"},
          {"system_time", "manifest-gen-time"},
          {"causal_time", "manifest-order-and-episode-refs"},
          {"admission_policy", "typed-episode-manifest-fold/v1"}};
}

declaration_reference episode_fact_surface_reference() {
  return {EPISODE_FACT_SURFACE_ID, EPISODE_FACT_SURFACE_VERSION, json_hash(episode_fact_surface_declaration_payload())};
}

nlohmann::json declaration_reference_json(const declaration_reference &reference) {
  return {{"id", reference.id}, {"version", reference.version}, {"root", reference.root}};
}

bool is_canonical_sha256_root(const std::string &root) {
  if (root.size() != 71 || !root.starts_with("sha256:")) {
    return false;
  }
  return std::all_of(root.begin() + 7, root.end(),
                     [](unsigned char value) { return std::isdigit(value) != 0 || (value >= 'a' && value <= 'f'); });
}

declaration_reference parse_declaration_reference(const nlohmann::json &value, const char *path) {
  reject_unknown_fields(value, {"id", "version", "root"}, path);
  declaration_reference reference;
  reference.id = optional_text(value, "id");
  reference.version = optional_text(value, "version");
  reference.root = optional_text(value, "root");
  if (reference.id.empty() || reference.version.empty() || !is_canonical_sha256_root(reference.root)) {
    throw std::invalid_argument(std::string(path) + " requires non-empty id/version and canonical sha256 root");
  }
  return reference;
}

void normalize_declaration_basis(query_basis &basis) {
  if (basis.contract_world.id.empty() && basis.contract_world.version.empty() && basis.contract_world.root.empty()) {
    basis.contract_world = episode_contract_world_reference();
  }
  if (basis.fact_surfaces.empty()) {
    basis.fact_surfaces.push_back(episode_fact_surface_reference());
  }
}

std::string admission_outcome_name(admission_outcome outcome) {
  switch (outcome) {
  case admission_outcome::Admitted:
    return "admitted";
  case admission_outcome::UnregisteredSurface:
    return "unregistered-surface";
  case admission_outcome::IncompatibleSchema:
    return "incompatible-schema";
  case admission_outcome::AmbiguousAuthority:
    return "ambiguous-authority";
  case admission_outcome::Unverifiable:
    return "unverifiable";
  }
  throw std::invalid_argument("unknown admission outcome");
}

nlohmann::json admission_evidence_json(const admission_evidence &evidence) {
  return {{"outcome", admission_outcome_name(evidence.outcome)},
          {"fact_surface_id", evidence.fact_surface_id},
          {"record_count", evidence.record_count},
          {"reason", evidence.reason}};
}

bool same_declaration(const declaration_reference &left, const declaration_reference &right) {
  return left.id == right.id && left.version == right.version && left.root == right.root;
}

admission_evidence declaration_admission_evidence(const query_basis &basis, uint64_t record_count) {
  const auto expected_world = episode_contract_world_reference();
  const auto expected_surface = episode_fact_surface_reference();
  if (basis.fact_surfaces.size() != 1) {
    return {admission_outcome::AmbiguousAuthority, "", record_count,
            "episode-manifest queries require exactly one fact-surface declaration"};
  }
  const auto &surface = basis.fact_surfaces.front();
  if (basis.contract_world.id != expected_world.id || surface.id != expected_surface.id) {
    return {admission_outcome::UnregisteredSurface, surface.id, record_count,
            "declaration id is not registered for the episode-manifest authority"};
  }
  if (basis.contract_world.version != expected_world.version || surface.version != expected_surface.version) {
    return {admission_outcome::IncompatibleSchema, surface.id, record_count,
            "declaration version is incompatible with the episode-manifest authority"};
  }
  if (!same_declaration(basis.contract_world, expected_world) || !same_declaration(surface, expected_surface)) {
    return {admission_outcome::Unverifiable, surface.id, record_count,
            "declaration root does not match the registered built-in declaration"};
  }
  return {admission_outcome::Admitted, surface.id, record_count,
          "records satisfy the built-in typed Episode manifest declaration"};
}

result_schema episode_result_schema() {
  return {QUERY_RESULT_ROW_SCHEMA_V1,
          {{"episode_id", "uint64", false},
           {"status", "string", false},
           {"opened", "boolean", false},
           {"closed", "boolean", false},
           {"begin_time", "int64", true},
           {"end_time", "int64", true},
           {"record_count", "uint64", false},
           {"frame_count", "uint64", false},
           {"ref_count", "uint64", false},
           {"content_root", "string", true},
           {"content_root_status", "string", false},
           {"title", "string", true},
           {"actor", "string", true},
           {"source", "string", true},
           {"reason", "string", true},
           {"schema", "string", false, false},
           {"record_kind", "string", true, false},
           {"schema_version", "uint64", true, false},
           {"location_uid", "uint64", true, false},
           {"parent_episode_id", "uint64", true, false},
           {"root_trigger_frame_uid", "uint64", true, false},
           {"manifest_frame_uid", "uint64", true, false},
           {"manifest_gen_time", "int64", true, false},
           {"update_time", "int64", true, false},
           {"last_frame_uid", "uint64", true, false},
           {"payload_ref_count", "uint64", false, false},
           {"schema_ref_count", "uint64", false, false},
           {"content_root_algorithm", "string", true, false}}};
}

result_schema temporal_match_result_schema() {
  return {QUERY_TEMPORAL_MATCH_ROW_SCHEMA_V1,
          {{"match_id", "string", false},
           {"partition_key", "string", false},
           {"repeat_count", "uint64", false},
           {"start_time", "int64", false},
           {"end_time", "int64", false},
           {"as_of_time", "int64", false},
           {"elapsed_ns", "int64", false},
           {"matched_episode_ids", "array<uint64>", false},
           {"matched_events", "array<object>", false},
           {"attribution_counts", "object", false},
           {"absence", "object", true},
           {"attention_required", "boolean", false},
           {"evidence_refs", "array<object>", false}}};
}

result_schema fact_state_result_schema() {
  return {QUERY_FACT_STATE_ROW_SCHEMA_V1,
          {{"observation_id", "string", false},
           {"contract_world_id", "string", false},
           {"fact_surface_id", "string", false},
           {"schema_owner_root", "string", false},
           {"subject_key", "string", false},
           {"source_id", "string", false},
           {"payload_hash", "string", false},
           {"payload_ref", "string", false},
           {"valid_time", "object", false},
           {"system_time", "int64", false},
           {"episode_id", "uint64", false},
           {"causal_parent_event_id", "string", true}}};
}

} // namespace fact_query_internal

using namespace fact_query_internal;

nlohmann::json logical_plan_json(const logical_plan &plan) {
  auto value = logical_plan_payload_json(plan);
  value["definition"] = query_definition_json(plan.definition);
  value["logical_plan_hash"] = plan.logical_plan_hash;
  return value;
}

nlohmann::json query_capabilities_json() {
  const auto contract_world = episode_contract_world_reference();
  const auto fact_surface = episode_fact_surface_reference();
  return {
      {"schema", QUERY_CAPABILITIES_SCHEMA_V1},
      {"query_definition_schema", QUERY_DEFINITION_SCHEMA_V1},
      {"logical_plan_schema", LOGICAL_PLAN_SCHEMA_V1},
      {"objects", nlohmann::json::array({"episodes", "fact-state"})},
      {"operators",
       nlohmann::json::array({"authority_scan", "filter", "order", "temporal_match", "limit", "project", "evidence"})},
      {"cuts", nlohmann::json::array({"head", "manifest_frame_uid", "system_time"})},
      {"admission_outcomes", nlohmann::json::array({"admitted", "unregistered-surface", "incompatible-schema",
                                                    "ambiguous-authority", "unverifiable"})},
      {"builtin_declarations",
       {{"contract_world",
         {{"reference", declaration_reference_json(contract_world)},
          {"declaration", episode_contract_world_declaration_payload()}}},
        {"fact_surfaces", nlohmann::json::array({{{"reference", declaration_reference_json(fact_surface)},
                                                  {"declaration", episode_fact_surface_declaration_payload()}}})}}},
      {"formats", nlohmann::json::array({"json", "ndjson", "tsv"})},
      {"frontends", nlohmann::json::array({"query-definition", "bounded-sql"})},
      {"continuous",
       {{"schema", QUERY_CHANGELOG_SCHEMA_V1},
        {"resume_token_schema", QUERY_RESUME_TOKEN_SCHEMA_V1},
        {"messages", nlohmann::json::array({"SnapshotBegin", "RowUpsert", "RowRetract", "Progress", "SchemaChange",
                                            "SnapshotEnd", "Gap"})},
        {"ordering", "batch-index"},
        {"replay", "message-idempotent"},
        {"backpressure", "bounded-pages"}}},
      {"saved_view_schema", QUERY_VIEW_SCHEMA_V1},
      {"temporal_patterns",
       {{"schema", QUERY_TEMPORAL_PATTERN_SCHEMA_V1},
        {"partition", "one field"},
        {"order", "one explicit time field"},
        {"sequence_steps", 2},
        {"sequence_semantics", "ordered-subsequence"},
        {"repeat", "1..16"},
        {"within", "1ns..30d"},
        {"absence", "optional, closed by explicit as_of_time"},
        {"unsupported",
         nlohmann::json::array({"alternation", "nested patterns", "unbounded waits", "inferred causality"})}}},
      {"sql",
       {{"object", "episodes"},
        {"accepted", nlohmann::json::array(
                         {"SELECT * FROM episodes",
                          "SELECT * FROM episodes WHERE episode_id = <u64> ORDER BY episode_id ASC LIMIT <1..1000>",
                          "SELECT * FROM episodes MATCH_RECOGNIZE (PARTITION BY <field> ORDER BY <field> ASC "
                          "PATTERN ((A B){<1..16>,<1..16>}) DEFINE A AS <field> = '<value>', B AS <field> = "
                          "'<value>' WITHIN <ns> AS OF <ns> [ABSENT <field> = '<value>']) [LIMIT <1..1000>]"})},
        {"rejected",
         nlohmann::json::array({"column projections", "joins", "subqueries", "OR", "non-equality predicates",
                                "descending order", "pattern alternation", "nested or unbounded patterns"})},
        {"basis_owner", "QueryDefinition"}}},
      {"execution_engines",
       nlohmann::json::array({"episode-authority-scan/v1", "episode-sqlite-projection/v1", "fact-authority-scan/v1"})},
      {"commands", nlohmann::json::array({"capabilities", "schema", "describe", "examples", "compile-sql", "validate",
                                          "explain", "prove", "changelog", "saved-view", "saved"})},
      {"limits", {{"minimum", 1}, {"maximum", 1000}}},
      {"error_codes",
       nlohmann::json::array({"KF_QUERY_INPUT", "KF_QUERY_VALIDATION", "KF_QUERY_EXECUTION", "KF_QUERY_OUTPUT",
                              "KF_QUERY_CHANGELOG", "KF_QUERY_VIEW", "KF_SAVED_QUERY", "KF_SAVED_QUERY_RUN"})},
      {"physical_plan", {{"public", false}, {"reason", "engine-private-and-replaceable"}}}};
}

nlohmann::json query_definition_schema_json() {
  const auto declaration_reference_schema =
      nlohmann::json{{"type", "object"},
                     {"required", nlohmann::json::array({"id", "version", "root"})},
                     {"properties",
                      {{"id", {{"type", "string"}, {"minLength", 1}}},
                       {"version", {{"type", "string"}, {"minLength", 1}}},
                       {"root", {{"type", "string"}, {"pattern", "^sha256:[0-9a-f]{64}$"}}}}},
                     {"additionalProperties", false}};
  const auto cut_schema = nlohmann::json{
      {"oneOf", nlohmann::json::array(
                    {{{"type", "object"},
                      {"required", nlohmann::json::array({"kind"})},
                      {"properties", {{"kind", {{"const", "head"}}}}},
                      {"additionalProperties", false}},
                     {{"type", "object"},
                      {"required", nlohmann::json::array({"kind", "manifest_frame_uid"})},
                      {"properties",
                       {{"kind", {{"const", "manifest_frame_uid"}}},
                        {"manifest_frame_uid",
                         {{"oneOf", nlohmann::json::array({{{"type", "integer"}, {"minimum", 1}},
                                                           {{"type", "string"}, {"pattern", "^[1-9][0-9]*$"}}})}}}}},
                      {"additionalProperties", false}},
                     {{"type", "object"},
                      {"required", nlohmann::json::array({"kind", "system_time"})},
                      {"properties",
                       {{"kind", {{"const", "system_time"}}},
                        {"system_time",
                         {{"oneOf", nlohmann::json::array({{{"type", "integer"}, {"minimum", 1}},
                                                           {{"type", "string"}, {"pattern", "^[1-9][0-9]*$"}}})}}}}},
                      {"additionalProperties", false}}})}};
  const auto policy_schema = nlohmann::json{
      {"type", "object"},
      {"properties",
       {{"fold", {{"enum", nlohmann::json::array({"episode-manifest-fold/v1", "latest-admitted-per-source/v1"})}}},
        {"schema",
         {{"enum", nlohmann::json::array({"kungfu.episode.manifest/v1", "kungfu.facts.domain-fact-event/v1"})}}},
        {"engine", {{"enum", nlohmann::json::array({"episode-authority-scan/v1", "fact-authority-scan/v1"})}}},
        {"conflict", {{"const", "preserve-source-claims/v1"}}},
        {"redaction", {{"enum", nlohmann::json::array({"report-missing-evidence/v1", "hash-and-ref/v1"})}}}}},
      {"additionalProperties", false}};
  const auto time_basis_schema = nlohmann::json{
      {"type", "object"},
      {"properties",
       {{"valid_time", {{"enum", nlohmann::json::array({"not-projected", "explicit"})}}},
        {"system_time", {{"enum", nlohmann::json::array({"manifest-gen-time", "event"})}}},
        {"causal_time", {{"enum", nlohmann::json::array({"manifest-order-and-episode-refs", "event-parent"})}}}}},
      {"additionalProperties", false}};
  const auto event_predicate_schema = nlohmann::json{
      {"type", "object"},
      {"required", nlohmann::json::array({"field", "equals"})},
      {"properties",
       {{"field",
         {{"enum", nlohmann::json::array({"episode_id", "title", "actor", "source", "status", "begin_time", "end_time",
                                          "reason", "record_count", "frame_count", "ref_count", "last_frame_uid",
                                          "content_root_status"})}}},
        {"equals", {{"type", "string"}, {"minLength", 1}, {"maxLength", 256}}}}},
      {"additionalProperties", false}};
  const auto temporal_pattern_schema = nlohmann::json{
      {"type", "object"},
      {"required",
       nlohmann::json::array({"schema", "partition_by", "order_by", "sequence", "repeat", "within_ns", "as_of_time"})},
      {"properties",
       {{"schema", {{"const", QUERY_TEMPORAL_PATTERN_SCHEMA_V1}}},
        {"partition_by",
         {{"enum", nlohmann::json::array(
                       {"episode_id", "title", "actor", "source", "status", "reason", "content_root_status"})}}},
        {"order_by", {{"enum", nlohmann::json::array({"begin_time", "end_time"})}}},
        {"sequence", {{"type", "array"}, {"minItems", 2}, {"maxItems", 2}, {"items", event_predicate_schema}}},
        {"repeat",
         {{"type", "object"},
          {"required", nlohmann::json::array({"min", "max"})},
          {"properties",
           {{"min", {{"type", "integer"}, {"minimum", 1}, {"maximum", 16}}},
            {"max", {{"type", "integer"}, {"minimum", 1}, {"maximum", 16}}}}},
          {"additionalProperties", false}}},
        {"within_ns",
         {{"oneOf", nlohmann::json::array(
                        {{{"type", "integer"}, {"minimum", 1}}, {{"type", "string"}, {"pattern", "^[1-9][0-9]*$"}}})}}},
        {"as_of_time",
         {{"oneOf", nlohmann::json::array(
                        {{{"type", "integer"}, {"minimum", 1}}, {{"type", "string"}, {"pattern", "^[1-9][0-9]*$"}}})}}},
        {"absence", event_predicate_schema}}},
      {"additionalProperties", false}};
  return {
      {"$schema", "https://json-schema.org/draft/2020-12/schema"},
      {"$id", QUERY_DEFINITION_SCHEMA_V1},
      {"title", "Kungfu QueryDefinition"},
      {"type", "object"},
      {"required", nlohmann::json::array({"schema", "basis", "object"})},
      {"properties",
       {{"schema", {{"const", QUERY_DEFINITION_SCHEMA_V1}}},
        {"basis",
         {{"type", "object"},
          {"required", nlohmann::json::array({"contract_world", "fact_surfaces", "scope", "perspective", "cut"})},
          {"properties",
           {{"contract_world", declaration_reference_schema},
            {"fact_surfaces",
             {{"type", "array"}, {"minItems", 1}, {"maxItems", 16}, {"items", declaration_reference_schema}}},
            {"scope", {{"enum", nlohmann::json::array({"episode-manifest", "domain-fact-ledger"})}}},
            {"episode_id",
             {{"oneOf", nlohmann::json::array(
                            {{{"type", "integer"}, {"minimum", 0}}, {{"type", "string"}, {"pattern", "^[0-9]+$"}}})}}},
            {"perspective",
             {{"enum", nlohmann::json::array({"manifest-append-order", "system-time-then-observation-id"})}}},
            {"cut", cut_schema},
            {"policy", policy_schema},
            {"time_basis", time_basis_schema}}},
          {"additionalProperties", false}}},
        {"object", {{"enum", nlohmann::json::array({"episodes", "fact-state"})}}},
        {"subject_keys",
         {{"type", "array"}, {"maxItems", 256}, {"items", {{"type", "string"}, {"minLength", 1}, {"maxLength", 512}}}}},
        {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 1000}}},
        {"evidence", {{"const", "proof"}}},
        {"temporal_pattern", temporal_pattern_schema}}},
      {"additionalProperties", false}};
}

nlohmann::json query_object_description_json(const std::string &object) {
  if (object == "fact-state") {
    return {{"schema", "kungfu.query.object-description/v1"},
            {"object", "fact-state"},
            {"authority", "yijinjing domain fact admission journal"},
            {"result_schema", result_schema_json(fact_state_result_schema())},
            {"basis_filter", "subject_keys"},
            {"canonical_order", nlohmann::json{{"field", "observation_id"}, {"direction", "asc"}}},
            {"supported_cuts", nlohmann::json::array({"head", "system_time"})}};
  }
  if (object != "episodes") {
    throw std::invalid_argument("unsupported query object: " + object);
  }
  return {{"schema", "kungfu.query.object-description/v1"},
          {"object", "episodes"},
          {"authority", "yijinjing Episode manifest journal"},
          {"result_schema", result_schema_json(episode_result_schema())},
          {"basis_filter", "basis.episode_id"},
          {"canonical_order", nlohmann::json{{"field", "episode_id"}, {"direction", "asc"}}},
          {"supported_cuts", nlohmann::json::array({"head", "manifest_frame_uid"})}};
}

nlohmann::json query_examples_json() {
  const query_definition head_definition{};
  auto historical_definition = head_definition;
  historical_definition.basis.episode_id = 1048;
  historical_definition.basis.selected_cut.kind = cut_kind::ManifestFrameUid;
  historical_definition.basis.selected_cut.manifest_frame_uid = 123456789;
  auto attention_definition = head_definition;
  attention_definition.limit = 10;
  attention_definition.has_temporal_pattern = true;
  attention_definition.pattern.partition_by = "source";
  attention_definition.pattern.order_by = "begin_time";
  attention_definition.pattern.sequence = {{"title", "alpha_published"}, {"title", "gate_failed"}};
  attention_definition.pattern.repeat_min = 2;
  attention_definition.pattern.repeat_max = 8;
  attention_definition.pattern.within_ns = 3600000000000ULL;
  attention_definition.pattern.as_of_time = 7200000000000LL;
  attention_definition.pattern.has_absence = true;
  attention_definition.pattern.absence = {"title", "stable_published"};
  auto fact_state_definition = head_definition;
  fact_state_definition.object = "fact-state";
  fact_state_definition.subject_keys = {"atlas:mission-example", "atlas:goal-example"};
  fact_state_definition.basis.contract_world = {"example.world", "1", json_hash({{"id", "example.world"}})};
  fact_state_definition.basis.fact_surfaces = {
      {"example.world.mission", "1", json_hash({{"id", "example.world.mission"}})},
      {"example.world.go", "1", json_hash({{"id", "example.world.go"}})}};
  fact_state_definition.basis.scope = "domain-fact-ledger";
  fact_state_definition.basis.perspective = "system-time-then-observation-id";
  fact_state_definition.basis.policy = {"latest-admitted-per-source/v1", "kungfu.facts.domain-fact-event/v1",
                                        "fact-authority-scan/v1", "preserve-source-claims/v1", "hash-and-ref/v1"};
  fact_state_definition.basis.valid_time = "explicit";
  fact_state_definition.basis.system_time = "event";
  fact_state_definition.basis.causal_time = "event-parent";
  return {{"schema", "kungfu.query.examples/v1"},
          {"examples",
           nlohmann::json::array(
               {{{"name", "episode-head"}, {"definition", query_definition_json(head_definition)}},
                {{"name", "episode-historical-cut"}, {"definition", query_definition_json(historical_definition)}},
                {{"name", "buildchain-release-attention"},
                 {"definition", query_definition_json(attention_definition)},
                 {"interpretation", "recorded temporal qualification, not inferred causality"}},
                {{"name", "declared-fact-state"},
                 {"definition", query_definition_json(fact_state_definition)},
                 {"interpretation", "canonical admitted fact state for a bounded subject set"}}})}};
}

nlohmann::json query_authority_json(const query_authority &authority) {
  return std::visit(
      [](const auto &value) -> nlohmann::json {
        using value_type = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<value_type, episode_authority>) {
          return {{"kind", value.kind},
                  {"schema", value.schema},
                  {"first_manifest_frame_uid", std::to_string(value.first_manifest_frame_uid)},
                  {"last_manifest_frame_uid", std::to_string(value.last_manifest_frame_uid)},
                  {"record_count", value.record_count},
                  {"unknown_record_count", value.unknown_record_count},
                  {"unfolded_record_count", value.unfolded_record_count}};
        } else {
          return {{"kind", value.kind},
                  {"schema", value.schema},
                  {"record_count", value.record_count},
                  {"selected_observation_count", value.selected_observation_count}};
        }
      },
      authority);
}

nlohmann::json resolved_query_cut_json(const resolved_query_cut &cut) {
  switch (cut.kind) {
  case resolved_cut_kind::Empty:
    return {{"kind", "empty"}};
  case resolved_cut_kind::Head:
    return {{"kind", "head"}, {"system_time", std::to_string(cut.system_time)}};
  case resolved_cut_kind::ManifestFrameUid:
    return {{"kind", "manifest_frame_uid"}, {"manifest_frame_uid", std::to_string(cut.manifest_frame_uid)}};
  case resolved_cut_kind::SystemTime:
    return {{"kind", "system_time"}, {"system_time", std::to_string(cut.system_time)}};
  case resolved_cut_kind::Unresolved:
    return {{"kind", "unresolved"}};
  }
  throw std::invalid_argument("unknown resolved query cut");
}

nlohmann::json query_cut_proof_json(const query_cut_proof &proof) {
  return {{"declared", cut_json(proof.declared)},
          {"resolved", resolved_query_cut_json(proof.resolved)},
          {"inclusive", proof.inclusive}};
}

nlohmann::json query_time_basis_json(const query_time_basis &basis) {
  return {{"valid_time", basis.valid_time}, {"system_time", basis.system_time}, {"causal_time", basis.causal_time}};
}

nlohmann::json query_execution_json(const query_execution &execution) {
  nlohmann::json value = {{"engine", execution.engine}, {"source", execution.source}};
  if (execution.has_projection) {
    value["projection_verified"] = execution.projection_verified;
    value["projection_status"] = execution.projection_status;
    value["projection_schema"] = execution.projection_schema;
  }
  if (execution.has_temporal_pattern) {
    value["temporal_pattern"] = temporal_pattern_json(execution.pattern);
    value["pattern_input_rows"] = execution.pattern_input_rows;
  }
  return value;
}

nlohmann::json episode_content_root_json(const episode_content_root &root) {
  nlohmann::json value = {
      {"episode_id", std::to_string(root.episode_id)},
      {"status", root.status},
  };
  const auto render_optional = [&](const char *name, const nullable_value &entry) {
    if (entry.present) {
      value[name] = entry.value.has_value() ? query_value_json(*entry.value) : nlohmann::json(nullptr);
    }
  };
  render_optional("recorded", root.recorded);
  render_optional("computed", root.computed);
  return value;
}

nlohmann::json missing_input_json(const missing_input &input) {
  return std::visit(
      [](const auto &value) -> nlohmann::json {
        using value_type = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<value_type, missing_manifest_cut>) {
          return {{"kind", "manifest_cut"}, {"manifest_frame_uid", std::to_string(value.manifest_frame_uid)}};
        } else if constexpr (std::is_same_v<value_type, missing_temporal_window>) {
          return {{"kind", "temporal-pattern-window"},
                  {"partition_key", value.partition_key},
                  {"required_through", std::to_string(value.required_through)},
                  {"observed_through", std::to_string(value.observed_through)}};
        } else if constexpr (std::is_same_v<value_type, missing_temporal_input_limit>) {
          return {{"kind", "temporal-pattern-input-limit"}, {"limit", value.limit}, {"available", value.available}};
        } else if constexpr (std::is_same_v<value_type, missing_episode>) {
          return {{"kind", "episode"}, {"episode_id", std::to_string(value.episode_id)}};
        } else {
          return {{"kind", "fact-state-result-limit"}, {"available", value.available}, {"limit", value.limit}};
        }
      },
      input);
}

nlohmann::json unverifiable_input_json(const unverifiable_input &input) {
  return std::visit(
      [](const auto &value) -> nlohmann::json {
        using value_type = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<value_type, unverifiable_temporal_input>) {
          return {{"kind", "temporal-pattern-input"}, {"episode_id", value.episode_id}, {"reason", value.reason}};
        } else if constexpr (std::is_same_v<value_type, unverifiable_declaration_basis>) {
          return {{"kind", "declaration_basis"},
                  {"outcome", admission_outcome_name(value.outcome)},
                  {"reason", value.reason}};
        } else if constexpr (std::is_same_v<value_type, unverifiable_record_count>) {
          return {{"kind", value.kind == unverifiable_record_kind::ManifestUnknown ? "manifest_unknown_records"
                                                                                   : "manifest_unfolded_records"},
                  {"count", value.count}};
        } else if constexpr (std::is_same_v<value_type, unverifiable_contract_world>) {
          return {{"kind", "contract-world-declaration"}, {"id", value.id}, {"reason", value.reason}};
        } else {
          return {{"kind", "fact-episode-content-root"}, {"episode_id", std::to_string(value.episode_id)}};
        }
      },
      input);
}

nlohmann::json query_conflict_json(const query_conflict &conflict) {
  return {{"subject_key", conflict.subject_key},
          {"observation_ids", conflict.observation_ids},
          {"source_ids", conflict.source_ids}};
}

nlohmann::json query_result_json(const query_result &result) {
  auto rows = nlohmann::json::array();
  for (const auto &row : result.rows) {
    rows.push_back(dynamic_row_json(result.row_schema, row));
  }
  auto roots = nlohmann::json::array();
  for (const auto &root : result.proof.episode_content_roots) {
    roots.push_back(episode_content_root_json(root));
  }
  auto missing = nlohmann::json::array();
  for (const auto &item : result.proof.missing_inputs) {
    missing.push_back(missing_input_json(item));
  }
  auto unverifiable = nlohmann::json::array();
  for (const auto &item : result.proof.unverifiable_inputs) {
    unverifiable.push_back(unverifiable_input_json(item));
  }
  auto conflicts = nlohmann::json::array();
  for (const auto &item : result.proof.conflicts) {
    conflicts.push_back(query_conflict_json(item));
  }
  auto fact_surfaces = nlohmann::json::array();
  for (const auto &surface : result.proof.fact_surface_declarations) {
    fact_surfaces.push_back(declaration_reference_json(surface));
  }
  auto admission_outcomes = nlohmann::json::array();
  for (const auto &outcome : result.proof.admission_outcomes) {
    admission_outcomes.push_back(admission_evidence_json(outcome));
  }
  const auto lineage = nlohmann::json{
      {"schema", result.proof.schema},
      {"query_definition_hash", result.proof.query_definition_hash},
      {"logical_plan_hash", result.proof.logical_plan_hash},
      {"authority", query_authority_json(result.proof.authority)},
      {"cut", query_cut_proof_json(result.proof.cut)},
      {"policy_versions", policy_json(result.proof.policy_versions)},
      {"time_basis", query_time_basis_json(result.proof.time_basis)},
      {"execution", query_execution_json(result.proof.execution)},
      {"determinism", result.proof.determinism},
      {"canonical_state", result.proof.canonical_state},
      {"contract_world_declaration", declaration_reference_json(result.proof.contract_world_declaration)},
      {"fact_surface_declarations", fact_surfaces},
      {"admission_outcomes", admission_outcomes},
      {"episode_content_roots", roots},
      {"missing_inputs", missing},
      {"unverifiable_inputs", unverifiable},
      {"conflicts", conflicts}};
  return {{"schema", result.schema},
          {"definition", query_definition_json(result.definition)},
          {"logical_plan", logical_plan_json(result.plan)},
          {"result_schema", result_schema_json(result.row_schema)},
          {"rows", rows},
          {"row_count", rows.size()},
          {"result_hash", result.result_hash},
          {"query_definition_root", result.proof.query_definition_hash},
          {"query_proof_root", json_hash(lineage)},
          {"lineage", lineage}};
}

using namespace fact_query_internal;

namespace {

nlohmann::json frontier_json(const query_frontier &frontier) {
  if (frontier.kind == frontier_kind::Empty) {
    return {{"kind", "empty"}, {"record_count", "0"}};
  }
  if (frontier.kind == frontier_kind::SystemTime) {
    return {{"kind", "system_time"},
            {"system_time", std::to_string(frontier.system_time)},
            {"record_count", std::to_string(frontier.record_count)}};
  }
  return {{"kind", "manifest_frame_uid"},
          {"manifest_frame_uid", std::to_string(frontier.manifest_frame_uid)},
          {"record_count", std::to_string(frontier.record_count)}};
}

query_frontier parse_frontier(const nlohmann::json &value, const char *path) {
  reject_unknown_fields(value, {"kind", "manifest_frame_uid", "system_time", "record_count"}, path);
  const auto kind = optional_text(value, "kind");
  if (kind == "empty") {
    if (value.contains("manifest_frame_uid") || value.contains("system_time")) {
      throw std::invalid_argument(std::string(path) + " empty frontier must not carry a position");
    }
    return {};
  }
  if (kind == "manifest_frame_uid") {
    return {frontier_kind::ManifestFrameUid, optional_uint64(value, "manifest_frame_uid"), 0,
            optional_uint64(value, "record_count")};
  }
  if (kind == "system_time") {
    const auto system_time = int64_value(value.at("system_time"), "frontier.system_time");
    if (system_time <= 0) {
      throw std::invalid_argument(std::string(path) + " system_time must be positive");
    }
    return {frontier_kind::SystemTime, 0, system_time, optional_uint64(value, "record_count")};
  }
  throw std::invalid_argument(std::string(path) + " kind must be empty, manifest_frame_uid, or system_time");
}

query_frontier result_frontier(const query_result &result) {
  const auto &resolved = result.proof.cut.resolved;
  const auto record_count =
      std::visit([](const auto &authority) { return authority.record_count; }, result.proof.authority);
  if (record_count == 0) {
    return {};
  }
  if (result.definition.object == "fact-state") {
    if (resolved.kind != resolved_cut_kind::SystemTime && resolved.kind != resolved_cut_kind::Head) {
      throw std::runtime_error("fact-state query result has no system-time frontier");
    }
    return {frontier_kind::SystemTime, 0, resolved.system_time, record_count};
  }
  if (resolved.kind == resolved_cut_kind::Empty) {
    return {};
  }
  if (resolved.kind == resolved_cut_kind::ManifestFrameUid) {
    return {frontier_kind::ManifestFrameUid, resolved.manifest_frame_uid, 0, record_count};
  }
  throw std::runtime_error("query result has no resumable frontier");
}

nlohmann::json result_frontier_evidence_json(const query_result &result) {
  auto value = resolved_query_cut_json(result.proof.cut.resolved);
  value["record_count"] =
      std::to_string(std::visit([](const auto &authority) { return authority.record_count; }, result.proof.authority));
  return value;
}

bool same_frontier(const query_frontier &left, const query_frontier &right) {
  return left.kind == right.kind &&
         (left.kind == frontier_kind::Empty ||
          (left.manifest_frame_uid == right.manifest_frame_uid && left.system_time == right.system_time &&
           left.record_count == right.record_count));
}

bool frontier_regressed(const query_frontier &from, const query_frontier &target) {
  if (target.record_count < from.record_count) {
    return true;
  }
  // frame_uid is an opaque manifest identity, not an ordinal. Append order is
  // proved by the fold's record_count; comparing uid numerically can report a
  // false regression when a later frame happens to have a smaller hash-like id.
  if (from.kind == frontier_kind::SystemTime && target.kind == frontier_kind::SystemTime) {
    return target.system_time < from.system_time;
  }
  return false;
}

nlohmann::json resume_token_payload_json(const query_resume_token &token) {
  return {{"schema", token.schema},
          {"definition", query_definition_json(token.definition)},
          {"query_definition_hash", token.query_definition_hash},
          {"logical_plan_hash", token.logical_plan_hash},
          {"from", frontier_json(token.from)},
          {"from_result_hash", token.from_result_hash},
          {"target", frontier_json(token.target)},
          {"target_result_hash", token.target_result_hash},
          {"next_message_index", token.next_message_index},
          {"batch_id", token.batch_id}};
}

void seal_resume_token(query_resume_token &token) { token.token_hash = json_hash(resume_token_payload_json(token)); }

std::string changelog_batch_id(const logical_plan &plan, const query_frontier &from,
                               const std::string &from_result_hash, const query_frontier &target,
                               const std::string &target_result_hash) {
  return json_hash({{"query_definition_hash", plan.query_definition_hash},
                    {"logical_plan_hash", plan.logical_plan_hash},
                    {"from", frontier_json(from)},
                    {"from_result_hash", from_result_hash},
                    {"target", frontier_json(target)},
                    {"target_result_hash", target_result_hash}});
}

query_result run_at_frontier(const std::string &runtime_dir, const query_definition &definition,
                             const query_frontier &frontier) {
  if (frontier.kind == frontier_kind::Empty) {
    query_result empty;
    empty.definition = definition;
    empty.plan = plan_query(definition);
    empty.row_schema = empty.plan.row_schema;
    empty.result_hash =
        json_hash({{"result_schema", result_schema_json(empty.row_schema)}, {"rows", nlohmann::json::array()}});
    return empty;
  }
  auto bounded = definition;
  if (definition.object == "fact-state") {
    if (frontier.kind != frontier_kind::SystemTime) {
      throw std::runtime_error("fact-state changelog frontier must use system_time");
    }
    bounded.basis.selected_cut.kind = cut_kind::SystemTime;
    bounded.basis.selected_cut.system_time = frontier.system_time;
    return run_fact_state_authority_scan(runtime_dir, plan_query(bounded));
  }
  if (frontier.kind != frontier_kind::ManifestFrameUid) {
    throw std::runtime_error("episode changelog frontier must use manifest_frame_uid");
  }
  bounded.basis.selected_cut.kind = cut_kind::ManifestFrameUid;
  bounded.basis.selected_cut.manifest_frame_uid = frontier.manifest_frame_uid;
  return run_episode_authority_scan(runtime_dir, plan_query(bounded));
}

std::string row_key(const nlohmann::json &row) {
  if (!row.is_object()) {
    throw std::runtime_error("query changelog row must be an object");
  }
  if (row.contains("match_id") && row.at("match_id").is_string()) {
    return row.at("match_id").get<std::string>();
  }
  if (row.contains("fact_surface_id") && row.contains("subject_key") && row.contains("source_id")) {
    return json_hash({{"contract_world_id", row.value("contract_world_id", "")},
                      {"fact_surface_id", row.at("fact_surface_id")},
                      {"subject_key", row.at("subject_key")},
                      {"source_id", row.at("source_id")}});
  }
  if (!row.contains("episode_id")) {
    throw std::runtime_error("query changelog row has neither match_id nor episode_id key");
  }
  const auto &value = row.at("episode_id");
  if (value.is_string()) {
    return value.get<std::string>();
  }
  if (value.is_number_unsigned()) {
    return std::to_string(value.get<uint64_t>());
  }
  if (value.is_number_integer() && value.get<int64_t>() >= 0) {
    return std::to_string(value.get<int64_t>());
  }
  throw std::runtime_error("episode changelog key must be an unsigned integer");
}

std::map<std::string, nlohmann::json> rows_by_key(const query_result &result) {
  std::map<std::string, nlohmann::json> rows;
  for (const auto &row : result.rows) {
    auto value = dynamic_row_json(result.row_schema, row);
    const auto key = row_key(value);
    if (!rows.emplace(key, std::move(value)).second) {
      throw std::runtime_error("episode changelog contains duplicate key " + key);
    }
  }
  return rows;
}

nlohmann::json evidence_reference(const query_result &result, const nlohmann::json &row) {
  auto missing = nlohmann::json::array();
  for (const auto &item : result.proof.missing_inputs) {
    missing.push_back(missing_input_json(item));
  }
  auto unverifiable = nlohmann::json::array();
  for (const auto &item : result.proof.unverifiable_inputs) {
    unverifiable.push_back(unverifiable_input_json(item));
  }
  nlohmann::json reference = {{"content_root", row.value("content_root", nlohmann::json(nullptr))},
                              {"content_root_status", row.value("content_root_status", "unverifiable")},
                              {"canonical_state", result.proof.canonical_state},
                              {"determinism", result.proof.determinism},
                              {"missing_inputs", missing},
                              {"unverifiable_inputs", unverifiable}};
  if (row.contains("episode_id")) {
    // Preserve the Q3 evidence edge: uint64 identities are rendered as text.
    reference["episode_id"] = row_key(row);
    if (row.contains("fact_surface_id")) {
      reference["episode_id"] = row.at("episode_id");
      reference["payload_hash"] = row.value("payload_hash", "");
      reference["payload_ref"] = row.value("payload_ref", "");
    }
  }
  if (row.contains("match_id")) {
    reference["row_key"] = row_key(row);
    reference["match_id"] = row.at("match_id");
    reference["evidence_refs"] = row.value("evidence_refs", nlohmann::json::array());
  }
  return reference;
}

nlohmann::json changelog_payload_json(const changelog_payload &payload) {
  return std::visit(
      [](const auto &message) -> nlohmann::json {
        using message_type = std::decay_t<decltype(message)>;
        if constexpr (std::is_same_v<message_type, snapshot_begin>) {
          return {
              {"type", "SnapshotBegin"},
              {"basis", query_definition_json(query_definition{QUERY_DEFINITION_SCHEMA_V1, message.basis})["basis"]},
              {"result_schema", result_schema_json(message.schema)}};
        } else if constexpr (std::is_same_v<message_type, row_upsert>) {
          return {{"type", "RowUpsert"},
                  {"key", message.key},
                  {"row", dynamic_row_json(message.schema, message.row)},
                  {"evidence_ref", message.evidence_ref}};
        } else if constexpr (std::is_same_v<message_type, row_retract>) {
          return {{"type", "RowRetract"},
                  {"key", message.key},
                  {"before_hash", message.before_hash},
                  {"evidence_ref", message.evidence_ref}};
        } else if constexpr (std::is_same_v<message_type, progress>) {
          return {
              {"type", "Progress"}, {"frontier", frontier_json(message.frontier)}, {"watermark", message.watermark}};
        } else if constexpr (std::is_same_v<message_type, schema_change>) {
          return {{"type", "SchemaChange"},
                  {"old_schema", result_schema_json(message.old_schema)},
                  {"new_schema", result_schema_json(message.new_schema)},
                  {"compatibility", message.compatibility}};
        } else if constexpr (std::is_same_v<message_type, snapshot_end>) {
          return {{"type", "SnapshotEnd"},
                  {"result_hash", message.result_hash},
                  {"frontier", frontier_json(message.frontier)}};
        } else {
          return {{"type", "Gap"},
                  {"expected", message.expected},
                  {"observed", message.observed},
                  {"recovery_hint", message.recovery_hint}};
        }
      },
      payload);
}

} // namespace

query_resume_token parse_query_resume_token(const nlohmann::json &value) {
  reject_unknown_fields(value,
                        {"schema", "definition", "query_definition_hash", "logical_plan_hash", "from",
                         "from_result_hash", "target", "target_result_hash", "next_message_index", "batch_id",
                         "token_hash"},
                        "resume_token");
  query_resume_token token;
  token.schema = optional_text(value, "schema");
  if (token.schema != QUERY_RESUME_TOKEN_SCHEMA_V1) {
    throw std::invalid_argument("unsupported query resume token schema");
  }
  token.definition = parse_query_definition(value.at("definition"));
  token.query_definition_hash = optional_text(value, "query_definition_hash");
  token.logical_plan_hash = optional_text(value, "logical_plan_hash");
  token.from = parse_frontier(value.at("from"), "resume_token.from");
  token.from_result_hash = optional_text(value, "from_result_hash");
  token.target = parse_frontier(value.at("target"), "resume_token.target");
  token.target_result_hash = optional_text(value, "target_result_hash");
  token.next_message_index = optional_uint64(value, "next_message_index");
  token.batch_id = optional_text(value, "batch_id");
  token.token_hash = optional_text(value, "token_hash");
  if (token.batch_id.empty() || token.token_hash.empty() ||
      token.token_hash != json_hash(resume_token_payload_json(token))) {
    throw std::invalid_argument("query resume token integrity check failed");
  }
  return token;
}

nlohmann::json query_resume_token_json(const query_resume_token &token) {
  auto value = resume_token_payload_json(token);
  value["token_hash"] = token.token_hash;
  return value;
}

nlohmann::json changelog_page_json(const changelog_page &page) {
  auto messages = nlohmann::json::array();
  for (const auto &message : page.messages) {
    auto value = changelog_payload_json(message.payload);
    value["message_id"] = message.message_id;
    value["index"] = message.index;
    messages.push_back(std::move(value));
  }
  return {{"schema", page.schema},
          {"batch_id", page.batch_id},
          {"messages", std::move(messages)},
          {"resume_token", query_resume_token_json(page.resume_token)},
          {"complete", page.complete}};
}

changelog_page run_query_changelog(const std::string &runtime_dir, const logical_plan &plan,
                                   const nlohmann::json &resume_token, uint64_t max_messages) {
  if (plan.definition.basis.selected_cut.kind != cut_kind::Head) {
    throw std::invalid_argument("continuous query requires a head cut");
  }
  if (max_messages == 0 || max_messages > 10000) {
    throw std::invalid_argument("max_messages must be between 1 and 10000");
  }

  const auto current = plan.definition.object == "fact-state" ? run_fact_state_authority_scan(runtime_dir, plan)
                                                              : run_episode_authority_scan(runtime_dir, plan);
  const auto current_frontier = result_frontier(current);
  query_resume_token cursor;
  bool continuing_batch = false;
  if (resume_token.is_object() && !resume_token.empty()) {
    cursor = parse_query_resume_token(resume_token);
    const auto token_plan = plan_query(cursor.definition);
    if (token_plan.query_definition_hash != plan.query_definition_hash ||
        token_plan.logical_plan_hash != plan.logical_plan_hash ||
        query_definition_json(cursor.definition) != query_definition_json(plan.definition)) {
      throw std::invalid_argument("query resume token belongs to a different QueryDefinition");
    }
    continuing_batch = cursor.next_message_index != 0 || !same_frontier(cursor.from, cursor.target) ||
                       cursor.from_result_hash != cursor.target_result_hash;
  } else {
    cursor.definition = plan.definition;
    cursor.query_definition_hash = plan.query_definition_hash;
    cursor.logical_plan_hash = plan.logical_plan_hash;
  }

  const auto from = cursor.from;
  const auto from_hash = cursor.from_result_hash;
  const auto target = continuing_batch ? cursor.target : current_frontier;
  const auto target_hash = continuing_batch ? cursor.target_result_hash : current.result_hash;
  const auto batch_id = changelog_batch_id(plan, from, from_hash, target, target_hash);

  std::vector<changelog_message> all;
  auto append = [&](changelog_payload payload) {
    const auto index = static_cast<uint64_t>(all.size());
    all.push_back({json_hash({{"batch_id", batch_id}, {"index", index}}), index, std::move(payload)});
  };
  auto gap_page = [&](nlohmann::json expected, nlohmann::json observed) {
    append(gap{std::move(expected), std::move(observed), "discard-token-and-request-full-snapshot"});
  };

  query_result prior;
  bool valid = true;
  if (!from_hash.empty()) {
    prior = run_at_frontier(runtime_dir, plan.definition, from);
    if (prior.result_hash != from_hash) {
      gap_page({{"frontier", frontier_json(from)}, {"result_hash", from_hash}},
               {{"frontier", result_frontier_evidence_json(prior)}, {"result_hash", prior.result_hash}});
      valid = false;
    }
  } else {
    prior.definition = plan.definition;
    prior.plan = plan;
    prior.row_schema = plan.row_schema;
    prior.result_hash =
        json_hash({{"result_schema", result_schema_json(plan.row_schema)}, {"rows", nlohmann::json::array()}});
  }

  query_result target_result;
  if (valid) {
    target_result =
        same_frontier(target, current_frontier) ? current : run_at_frontier(runtime_dir, plan.definition, target);
    if (frontier_regressed(from, target) || target_result.result_hash != target_hash) {
      gap_page(
          {{"frontier", frontier_json(target)}, {"result_hash", target_hash}},
          {{"frontier", result_frontier_evidence_json(target_result)}, {"result_hash", target_result.result_hash}});
      valid = false;
    }
  }

  if (valid) {
    const auto prior_rows = rows_by_key(prior);
    const auto target_rows = rows_by_key(target_result);
    if (from_hash.empty()) {
      append(snapshot_begin{plan.definition.basis, target_result.row_schema});
    } else if (result_schema_json(prior.row_schema) != result_schema_json(target_result.row_schema)) {
      append(schema_change{prior.row_schema, target_result.row_schema, "consumer-must-revalidate"});
    }
    for (const auto &[key, row] : prior_rows) {
      if (!target_rows.contains(key)) {
        append(row_retract{key, json_hash(row), evidence_reference(prior, row)});
      }
    }
    for (const auto &[key, row] : target_rows) {
      const auto previous = prior_rows.find(key);
      if (previous == prior_rows.end() || previous->second != row) {
        append(row_upsert{key, target_result.row_schema, make_dynamic_row(target_result.row_schema, row),
                          evidence_reference(target_result, row)});
      }
    }
    if (from_hash.empty()) {
      append(snapshot_end{target_result.result_hash, target});
    } else {
      append(progress{target, {{"kind", "authority-frontier"}, {"inclusive", true}}});
    }
  }

  const auto begin = valid ? std::min<uint64_t>(cursor.next_message_index, all.size()) : 0;
  const auto end = std::min<uint64_t>(all.size(), begin + max_messages);
  changelog_page page;
  page.batch_id = batch_id;
  page.messages.insert(page.messages.end(), all.begin() + static_cast<std::ptrdiff_t>(begin),
                       all.begin() + static_cast<std::ptrdiff_t>(end));
  page.complete = !valid || end == all.size();
  page.resume_token.definition = plan.definition;
  page.resume_token.query_definition_hash = plan.query_definition_hash;
  page.resume_token.logical_plan_hash = plan.logical_plan_hash;
  page.resume_token.batch_id = batch_id;
  if (page.complete && valid) {
    page.resume_token.from = target;
    page.resume_token.from_result_hash = target_hash;
    page.resume_token.target = target;
    page.resume_token.target_result_hash = target_hash;
  } else {
    page.resume_token.from = from;
    page.resume_token.from_result_hash = from_hash;
    page.resume_token.target = target;
    page.resume_token.target_result_hash = target_hash;
    page.resume_token.next_message_index = end;
  }
  seal_resume_token(page.resume_token);
  return page;
}

changelog_page run_episode_changelog(const std::string &runtime_dir, const logical_plan &plan,
                                     const nlohmann::json &resume_token, uint64_t max_messages) {
  if (plan.definition.object != "episodes") {
    throw std::invalid_argument("episode changelog requires object=episodes");
  }
  return run_query_changelog(runtime_dir, plan, resume_token, max_messages);
}

} // namespace kungfu::runtime::query
