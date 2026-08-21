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

namespace yy_storage = kungfu::yijinjing::storage;

using namespace fact_query_internal;

namespace {

std::string event_text(const nlohmann::json &row, const std::string &field) {
  if (!row.contains(field) || row.at(field).is_null()) {
    return {};
  }
  if (row.at(field).is_string()) {
    return row.at(field).get<std::string>();
  }
  if (row.at(field).is_number_integer()) {
    return std::to_string(row.at(field).get<int64_t>());
  }
  if (row.at(field).is_number_unsigned()) {
    return std::to_string(row.at(field).get<uint64_t>());
  }
  if (row.at(field).is_boolean()) {
    return row.at(field).get<bool>() ? "true" : "false";
  }
  return {};
}

bool event_matches(const nlohmann::json &row, const event_predicate &predicate) {
  return event_text(row, predicate.field) == predicate.equals;
}

bool event_time(const nlohmann::json &row, const std::string &field, int64_t &value) {
  if (!row.contains(field) || row.at(field).is_null()) {
    return false;
  }
  try {
    value = int64_value(row.at(field), field.c_str());
    return true;
  } catch (const std::invalid_argument &) {
    return false;
  }
}

nlohmann::json pattern_event_json(const nlohmann::json &row) {
  nlohmann::json event = nlohmann::json::object();
  for (const auto *field : {"episode_id", "title", "actor", "source", "status", "begin_time", "end_time", "reason",
                            "content_root", "content_root_status"}) {
    if (row.contains(field)) {
      event[field] = row.at(field);
    }
  }
  return event;
}

std::vector<dynamic_row> evaluate_temporal_pattern(const logical_plan &plan,
                                                   const std::vector<nlohmann::json> &input_rows, lineage &proof) {
  const auto &pattern = plan.definition.pattern;
  struct ordered_event {
    int64_t time = 0;
    uint64_t episode_id = 0;
    const nlohmann::json *row = nullptr;
  };
  std::map<std::string, std::vector<ordered_event>> partitions;
  for (const auto &row : input_rows) {
    const auto partition = event_text(row, pattern.partition_by);
    int64_t time = 0;
    if (partition.empty() || !event_time(row, pattern.order_by, time) || time <= 0) {
      proof.unverifiable_inputs.push_back(
          unverifiable_temporal_input{event_text(row, "episode_id"), "missing partition/order field"});
      continue;
    }
    if (time > pattern.as_of_time) {
      continue;
    }
    partitions[partition].push_back({time, optional_uint64(row, "episode_id"), &row});
  }

  std::vector<dynamic_row> matches;
  for (auto &[partition, events] : partitions) {
    std::sort(events.begin(), events.end(), [](const ordered_event &left, const ordered_event &right) {
      return left.time < right.time || (left.time == right.time && left.episode_id < right.episode_id);
    });
    size_t cursor = 0;
    std::vector<const ordered_event *> matched;
    int64_t start_time = 0;
    int64_t end_time = 0;
    uint64_t repeat_count = 0;
    while (cursor < events.size() && repeat_count < pattern.repeat_max) {
      size_t first = cursor;
      while (first < events.size() && !event_matches(*events[first].row, pattern.sequence[0])) {
        ++first;
      }
      if (first == events.size()) {
        break;
      }
      if (matched.empty()) {
        start_time = events[first].time;
      }
      const auto max_time = pattern.within_ns > static_cast<uint64_t>(std::numeric_limits<int64_t>::max() - start_time)
                                ? std::numeric_limits<int64_t>::max()
                                : start_time + static_cast<int64_t>(pattern.within_ns);
      if (events[first].time > max_time) {
        break;
      }
      size_t second = first + 1;
      while (second < events.size() && events[second].time <= max_time &&
             !event_matches(*events[second].row, pattern.sequence[1])) {
        ++second;
      }
      if (second == events.size() || events[second].time > max_time) {
        break;
      }
      matched.push_back(&events[first]);
      matched.push_back(&events[second]);
      end_time = events[second].time;
      ++repeat_count;
      cursor = second + 1;
    }
    if (repeat_count < pattern.repeat_min) {
      continue;
    }
    const auto window_end = pattern.within_ns > static_cast<uint64_t>(std::numeric_limits<int64_t>::max() - start_time)
                                ? std::numeric_limits<int64_t>::max()
                                : start_time + static_cast<int64_t>(pattern.within_ns);
    if (pattern.as_of_time < window_end) {
      proof.missing_inputs.push_back(missing_temporal_window{partition, window_end, pattern.as_of_time});
      continue;
    }
    bool terminal_present = false;
    if (pattern.has_absence) {
      terminal_present = std::any_of(events.begin(), events.end(), [&](const ordered_event &event) {
        return event.time >= start_time && event.time <= window_end && event_matches(*event.row, pattern.absence);
      });
    }
    if (terminal_present) {
      continue;
    }

    auto episode_ids = nlohmann::json::array();
    auto matched_events = nlohmann::json::array();
    auto evidence_refs = nlohmann::json::array();
    nlohmann::json attribution_counts = nlohmann::json::object();
    for (const auto *event : matched) {
      episode_ids.push_back(event->episode_id);
      matched_events.push_back(pattern_event_json(*event->row));
      evidence_refs.push_back({{"episode_id", std::to_string(event->episode_id)},
                               {"content_root", event->row->value("content_root", nlohmann::json(nullptr))},
                               {"content_root_status", event->row->value("content_root_status", "unverifiable")}});
      const auto attribution = event_text(*event->row, "actor");
      if (!attribution.empty()) {
        attribution_counts[attribution] = attribution_counts.value(attribution, 0) + 1;
      }
    }
    nlohmann::json absence = nullptr;
    if (pattern.has_absence) {
      absence = event_predicate_json(pattern.absence);
    }
    // A match key must survive head advancement. The full QueryDefinition hash
    // includes the resolved historical cut during changelog reconstruction, so
    // deriving the key from it would turn a valid correction into a false Gap.
    const auto match_id = json_hash({{"temporal_pattern", temporal_pattern_json(pattern)},
                                     {"partition_key", partition},
                                     {"matched_episode_ids", episode_ids}});
    matches.push_back(make_dynamic_row(plan.row_schema, {{"match_id", match_id},
                                                         {"partition_key", partition},
                                                         {"repeat_count", repeat_count},
                                                         {"start_time", start_time},
                                                         {"end_time", end_time},
                                                         {"as_of_time", pattern.as_of_time},
                                                         {"elapsed_ns", pattern.as_of_time - start_time},
                                                         {"matched_episode_ids", episode_ids},
                                                         {"matched_events", matched_events},
                                                         {"attribution_counts", attribution_counts},
                                                         {"absence", absence},
                                                         {"attention_required", true},
                                                         {"evidence_refs", evidence_refs}}));
    if (matches.size() >= plan.definition.limit) {
      break;
    }
  }
  return matches;
}

} // namespace

static query_result run_episode_fold(const logical_plan &plan, const yy_storage::episode_manifest_fold &fold,
                                     query_execution execution) {
  const auto &definition = plan.definition;

  query_result result;
  result.definition = definition;
  result.plan = plan;
  result.row_schema = plan.row_schema;
  result.proof.query_definition_hash = plan.query_definition_hash;
  result.proof.logical_plan_hash = plan.logical_plan_hash;
  result.proof.contract_world_declaration = definition.basis.contract_world;
  result.proof.fact_surface_declarations = definition.basis.fact_surfaces;
  result.proof.authority = episode_authority{"yijinjing-journal",
                                             yy_storage::EPISODE_MANIFEST_SCHEMA_V1,
                                             fold.first_manifest_frame_uid,
                                             fold.last_manifest_frame_uid,
                                             static_cast<uint64_t>(fold.total_record_count),
                                             static_cast<uint64_t>(fold.unknown_record_count),
                                             static_cast<uint64_t>(fold.unfolded_record_count)};
  const auto resolved = !fold.cut_found ? resolved_query_cut{resolved_cut_kind::Unresolved}
                        : fold.total_record_count == 0
                            ? resolved_query_cut{resolved_cut_kind::Empty}
                            : resolved_query_cut{resolved_cut_kind::ManifestFrameUid, fold.last_manifest_frame_uid};
  result.proof.cut = {definition.basis.selected_cut, resolved, true};
  result.proof.policy_versions = definition.basis.policy;
  result.proof.time_basis = {definition.basis.valid_time, definition.basis.system_time, definition.basis.causal_time};
  result.proof.execution = std::move(execution);

  auto known_record_count = static_cast<uint64_t>(fold.total_record_count);
  const auto unknown_record_count = std::min(known_record_count, static_cast<uint64_t>(fold.unknown_record_count));
  known_record_count -= unknown_record_count;
  const auto unfolded_record_count = std::min(known_record_count, static_cast<uint64_t>(fold.unfolded_record_count));
  known_record_count -= unfolded_record_count;
  auto declaration_outcome =
      declaration_admission_evidence(definition.basis, static_cast<uint64_t>(fold.total_record_count));
  const auto declarations_admitted = declaration_outcome.outcome == admission_outcome::Admitted;
  if (declarations_admitted) {
    declaration_outcome.record_count = known_record_count;
  } else {
    result.proof.unverifiable_inputs.push_back(
        unverifiable_declaration_basis{declaration_outcome.outcome, declaration_outcome.reason});
    result.proof.determinism = "unverifiable";
  }
  result.proof.admission_outcomes.push_back(declaration_outcome);

  if (!fold.cut_found) {
    result.proof.missing_inputs.push_back(missing_manifest_cut{definition.basis.selected_cut.manifest_frame_uid});
    result.proof.determinism = "unverifiable";
  }
  if (fold.unknown_record_count != 0) {
    result.proof.unverifiable_inputs.push_back(unverifiable_record_count{
        unverifiable_record_kind::ManifestUnknown, static_cast<uint64_t>(fold.unknown_record_count)});
    result.proof.determinism = "unverifiable";
    result.proof.admission_outcomes.push_back(
        {admission_outcome::Unverifiable, definition.basis.fact_surfaces.front().id,
         static_cast<uint64_t>(fold.unknown_record_count), "unknown manifest records are not admitted"});
  }
  if (fold.unfolded_record_count != 0) {
    result.proof.unverifiable_inputs.push_back(unverifiable_record_count{
        unverifiable_record_kind::ManifestUnfolded, static_cast<uint64_t>(fold.unfolded_record_count)});
    result.proof.determinism = "unverifiable";
    result.proof.admission_outcomes.push_back(
        {admission_outcome::Unverifiable, definition.basis.fact_surfaces.front().id,
         static_cast<uint64_t>(fold.unfolded_record_count), "unfolded manifest records are not admitted"});
  }

  result.proof.canonical_state =
      declarations_admitted && fold.cut_found && fold.unknown_record_count == 0 && fold.unfolded_record_count == 0;

  std::vector<nlohmann::json> pattern_input_rows;
  bool pattern_input_truncated = false;
  if (fold.cut_found && declarations_admitted) {
    for (const auto &[episode_id, view] : fold.episodes) {
      if (definition.basis.episode_id != 0 && episode_id != definition.basis.episode_id) {
        continue;
      }
      if (definition.has_temporal_pattern && pattern_input_rows.size() >= 1000) {
        pattern_input_truncated = true;
        break;
      }
      auto row = yy_storage::episode_summary_json(view);
      const auto content_root = yy_storage::episode_content_root_json(view, fold.unknown_record_count);
      row["content_root_status"] = content_root.at("status");
      if (!row.contains("content_root")) {
        row["content_root"] = nullptr;
      }
      if (definition.has_temporal_pattern) {
        pattern_input_rows.push_back(row);
      } else {
        result.rows.push_back(make_dynamic_row(result.row_schema, row));
      }
      const auto nullable = [](const nlohmann::json &value) {
        return nullable_value{true, value.is_null() ? std::optional<query_value>{}
                                                    : std::optional<query_value>{query_value_from_json(value)}};
      };
      result.proof.episode_content_roots.push_back(
          episode_content_root{episode_id, content_root.at("status").get<std::string>(),
                               nullable(content_root.at("recorded")), nullable(content_root.at("computed"))});
      if (!definition.has_temporal_pattern && definition.limit != 0 && result.rows.size() >= definition.limit) {
        break;
      }
    }
  }
  if (definition.has_temporal_pattern) {
    if (pattern_input_truncated) {
      result.proof.missing_inputs.push_back(
          missing_temporal_input_limit{1000, static_cast<uint64_t>(fold.episodes.size())});
    }
    result.rows = evaluate_temporal_pattern(plan, pattern_input_rows, result.proof);
    result.proof.execution.has_temporal_pattern = true;
    result.proof.execution.pattern = definition.pattern;
    result.proof.execution.pattern_input_rows = pattern_input_rows.size();
    if (!result.proof.missing_inputs.empty() || !result.proof.unverifiable_inputs.empty()) {
      result.proof.determinism = "unverifiable";
      result.proof.canonical_state = false;
    }
  }
  if (definition.basis.episode_id != 0 && result.rows.empty() && fold.cut_found) {
    result.proof.missing_inputs.push_back(missing_episode{definition.basis.episode_id});
  }

  result.result_hash = json_hash({{"result_schema", result_schema_json(result.row_schema)},
                                  {"rows", dynamic_rows_json(result.row_schema, result.rows)}});
  return result;
}

query_result run_episode_authority_scan(const std::string &runtime_dir, const logical_plan &plan) {
  const auto &definition = plan.definition;
  yy_storage::episode_manifest_store store(runtime_dir);
  const auto fold = definition.basis.selected_cut.kind == cut_kind::Head
                        ? store.fold_typed_records()
                        : store.fold_typed_records_until(definition.basis.selected_cut.manifest_frame_uid);
  return run_episode_fold(plan, fold, query_execution{"episode-authority-scan/v1", "journal"});
}

namespace {

struct fact_selection {
  std::string contract_world_id;
  std::set<std::string> surface_ids;
  std::set<std::string> subject_keys;

  explicit fact_selection(const query_definition &definition)
      : contract_world_id(definition.basis.contract_world.id),
        subject_keys(definition.subject_keys.begin(), definition.subject_keys.end()) {
    for (const auto &surface : definition.basis.fact_surfaces) {
      surface_ids.insert(surface.id);
    }
  }

  bool matches(const nlohmann::json &row) const {
    return optional_text(row, "contract_world_id") == contract_world_id &&
           surface_ids.contains(optional_text(row, "fact_surface_id")) &&
           (subject_keys.empty() || subject_keys.contains(optional_text(row, "subject_key")));
  }
};

query_result initialize_fact_result(const logical_plan &plan, int64_t resolved_cut) {
  const auto &definition = plan.definition;
  query_result result;
  result.definition = definition;
  result.plan = plan;
  result.row_schema = plan.row_schema;
  result.proof.query_definition_hash = plan.query_definition_hash;
  result.proof.logical_plan_hash = plan.logical_plan_hash;
  result.proof.contract_world_declaration = definition.basis.contract_world;
  result.proof.fact_surface_declarations = definition.basis.fact_surfaces;
  result.proof.cut = {definition.basis.selected_cut,
                      resolved_query_cut{definition.basis.selected_cut.kind == cut_kind::SystemTime
                                             ? resolved_cut_kind::SystemTime
                                             : resolved_cut_kind::Head,
                                         0, resolved_cut},
                      true};
  result.proof.policy_versions = definition.basis.policy;
  result.proof.time_basis = {definition.basis.valid_time, definition.basis.system_time, definition.basis.causal_time};
  result.proof.execution = query_execution{"fact-authority-scan/v1", "journal"};
  return result;
}

std::map<std::string, uint64_t> collect_fact_history(const nlohmann::json &state, const fact_selection &selection,
                                                     query_result &result) {
  uint64_t selected_history_count = 0;
  std::map<std::string, uint64_t> outcome_counts;
  for (const auto &row : state.at("observation_history")) {
    if (!selection.matches(row)) {
      continue;
    }
    ++selected_history_count;
    ++outcome_counts[optional_text(row, "outcome", "unverifiable")];
  }
  result.proof.authority =
      fact_authority{"yijinjing-journal", facts::DOMAIN_FACT_EVENT_SCHEMA_V1,
                     uint64_value(state.at("proof").at("record_count"), "proof.record_count"), selected_history_count};
  return outcome_counts;
}

std::pair<uint64_t, uint64_t> declaration_catalog_matches(const declaration_reference &reference,
                                                          const nlohmann::json &catalog, int64_t resolved_cut) {
  uint64_t exact = 0;
  uint64_t identity = 0;
  for (const auto &entry : catalog) {
    const auto from = int64_value(entry.at("effective_from"), "declaration.effective_from");
    const auto until = int64_value(entry.at("effective_until"), "declaration.effective_until");
    const auto effective = resolved_cut >= from && (until == 0 || resolved_cut < until);
    if (optional_text(entry, "id") != reference.id || optional_text(entry, "version") != reference.version ||
        !effective) {
      continue;
    }
    ++identity;
    if (optional_text(entry, "root") == reference.root) {
      ++exact;
    }
  }
  return {exact, identity};
}

bool admit_fact_declarations(const nlohmann::json &state, const fact_selection &selection,
                             const query_definition &definition, int64_t resolved_cut, query_result &result) {
  const auto &catalog = state.at("catalog");
  const auto [world_exact, world_identity] =
      declaration_catalog_matches(definition.basis.contract_world, catalog.at("contract_worlds"), resolved_cut);
  bool declarations_admitted = world_exact == 1;
  if (!declarations_admitted) {
    result.proof.unverifiable_inputs.push_back(unverifiable_contract_world{
        definition.basis.contract_world.id,
        world_identity == 0 ? "unregistered-or-ineffective" : "root-mismatch-or-ambiguous"});
  }
  for (const auto &surface : definition.basis.fact_surfaces) {
    const auto [exact, identity] = declaration_catalog_matches(surface, catalog.at("fact_surfaces"), resolved_cut);
    const auto row_count = static_cast<uint64_t>(
        std::count_if(state.at("canonical_facts").begin(), state.at("canonical_facts").end(), [&](const auto &row) {
          return selection.matches(row) && optional_text(row, "fact_surface_id") == surface.id;
        }));
    if (exact == 1 && world_exact == 1) {
      result.proof.admission_outcomes.push_back(
          {admission_outcome::Admitted, surface.id, row_count, "facts satisfy the pinned effective declaration"});
      continue;
    }
    declarations_admitted = false;
    result.proof.admission_outcomes.push_back(
        {identity == 0 ? admission_outcome::UnregisteredSurface : admission_outcome::Unverifiable, surface.id,
         row_count,
         identity == 0 ? "fact surface is unregistered or ineffective"
                       : "fact surface declaration root is mismatched or ambiguous"});
  }
  return declarations_admitted;
}

void append_fact_outcomes(const std::map<std::string, uint64_t> &outcome_counts, query_result &result) {
  const auto append = [&](const char *name, admission_outcome outcome) {
    const auto found = outcome_counts.find(name);
    if (found != outcome_counts.end() && found->second != 0) {
      result.proof.admission_outcomes.push_back({outcome, "", found->second, "selected observation admission outcome"});
    }
  };
  append("unregistered-surface", admission_outcome::UnregisteredSurface);
  append("incompatible-schema", admission_outcome::IncompatibleSchema);
  append("ambiguous-authority", admission_outcome::AmbiguousAuthority);
  append("unverifiable", admission_outcome::Unverifiable);
}

void collect_fact_conflicts(const nlohmann::json &state, const fact_selection &selection, query_result &result) {
  for (const auto &conflict : state.at("conflicts")) {
    if (selection.subject_keys.empty() || selection.subject_keys.contains(optional_text(conflict, "subject_key"))) {
      result.proof.conflicts.push_back(query_conflict{optional_text(conflict, "subject_key"),
                                                      conflict.at("observation_ids").get<std::vector<std::string>>(),
                                                      conflict.at("source_ids").get<std::vector<std::string>>()});
    }
  }
}

uint64_t project_fact_rows(const std::string &runtime_dir, const nlohmann::json &state, const fact_selection &selection,
                           const query_definition &definition, query_result &result) {
  yy_storage::episode_manifest_store episodes(runtime_dir);
  std::set<uint64_t> rooted_episodes;
  uint64_t matching_rows = 0;
  for (auto row : state.at("canonical_facts")) {
    if (!selection.matches(row)) {
      continue;
    }
    ++matching_rows;
    if (result.rows.size() >= definition.limit) {
      continue;
    }
    const auto episode_id = uint64_value(row.at("episode_id"), "fact-state.episode_id");
    row["episode_id"] = std::to_string(episode_id);
    row["system_time"] = std::to_string(int64_value(row.at("system_time"), "fact-state.system_time"));
    result.rows.push_back(make_dynamic_row(result.row_schema, row));
    if (!rooted_episodes.insert(episode_id).second) {
      continue;
    }
    const auto inspected = episodes.inspect_typed(episode_id);
    if (inspected.content_root.status == yy_storage::episode_content_root_status::Verified &&
        inspected.content_root.computed.has_value()) {
      const auto &root = *inspected.content_root.computed;
      result.proof.episode_content_roots.push_back(
          episode_content_root{episode_id, "verified", {}, {true, root.algorithm + ":" + root.value}});
    } else {
      result.proof.unverifiable_inputs.push_back(unverifiable_fact_episode_root{episode_id});
    }
  }
  return matching_rows;
}

uint64_t bad_fact_outcome_count(const std::map<std::string, uint64_t> &outcome_counts) {
  uint64_t count = 0;
  for (const auto *name : {"unregistered-surface", "incompatible-schema", "ambiguous-authority", "unverifiable"}) {
    const auto found = outcome_counts.find(name);
    if (found != outcome_counts.end()) {
      count += found->second;
    }
  }
  return count;
}

} // namespace

query_result run_fact_state_authority_scan(const std::string &runtime_dir, const logical_plan &plan) {
  const auto &definition = plan.definition;
  if (definition.object != "fact-state") {
    throw std::invalid_argument("fact-state authority scan requires object=fact-state");
  }
  const auto cut_system_time =
      definition.basis.selected_cut.kind == cut_kind::SystemTime ? definition.basis.selected_cut.system_time : 0;
  const auto state = facts::query_fact_state(runtime_dir, cut_system_time);
  const auto resolved_cut = int64_value(state.at("cut").at("system_time"), "state.cut.system_time");
  const fact_selection selection(definition);
  auto result = initialize_fact_result(plan, resolved_cut);
  const auto outcome_counts = collect_fact_history(state, selection, result);
  const auto declarations_admitted = admit_fact_declarations(state, selection, definition, resolved_cut, result);
  append_fact_outcomes(outcome_counts, result);
  collect_fact_conflicts(state, selection, result);
  const auto matching_rows = project_fact_rows(runtime_dir, state, selection, definition, result);
  if (matching_rows > result.rows.size()) {
    result.proof.missing_inputs.push_back(missing_fact_state_result_limit{matching_rows, definition.limit});
  }
  const auto bad_outcomes = bad_fact_outcome_count(outcome_counts);
  result.proof.canonical_state = declarations_admitted && bad_outcomes == 0 &&
                                 result.proof.unverifiable_inputs.empty() && result.proof.missing_inputs.empty();
  result.proof.determinism = result.proof.canonical_state ? "deterministic" : "unverifiable";
  result.result_hash = json_hash({{"result_schema", result_schema_json(result.row_schema)},
                                  {"rows", dynamic_rows_json(result.row_schema, result.rows)}});
  return result;
}

query_result run_episode_sqlite_projection(const std::string &runtime_dir, const logical_plan &plan) {
  const auto &definition = plan.definition;
  storage_service_api::episode_manifest_projection projection(runtime_dir);
  const auto verification = projection.verify_typed();
  if (!verification.projection_present || !verification.ok) {
    throw std::runtime_error("Episode SQLite query projection is absent or stale; rebuild it before execution");
  }
  const auto fold = definition.basis.selected_cut.kind == cut_kind::Head
                        ? projection.fold_typed_records()
                        : projection.fold_typed_records_until(definition.basis.selected_cut.manifest_frame_uid);
  query_execution execution{"episode-sqlite-projection/v1", "sqlite-projection"};
  execution.has_projection = true;
  execution.projection_verified = true;
  execution.projection_status = verification.status;
  execution.projection_schema = storage_service_api::EPISODE_MANIFEST_PROJECTION_SCHEMA_V1;
  return run_episode_fold(plan, fold, std::move(execution));
}

} // namespace kungfu::runtime::query
