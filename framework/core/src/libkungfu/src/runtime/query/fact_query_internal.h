// SPDX-License-Identifier: Apache-2.0

#ifndef KUNGFU_RUNTIME_QUERY_FACT_QUERY_INTERNAL_H
#define KUNGFU_RUNTIME_QUERY_FACT_QUERY_INTERNAL_H

#include <initializer_list>
#include <string>

#include <kungfu/runtime/query/fact_query.h>

namespace kungfu::runtime::query::fact_query_internal {

void reject_unknown_fields(const nlohmann::json &object, std::initializer_list<const char *> allowed, const char *path);
uint64_t uint64_value(const nlohmann::json &value, const char *field);
uint64_t optional_uint64(const nlohmann::json &object, const char *field, uint64_t fallback = 0);
int64_t int64_value(const nlohmann::json &value, const char *field);
std::string optional_text(const nlohmann::json &object, const char *field, const std::string &fallback = {});
std::string ascii_lower(std::string value);
event_predicate parse_event_predicate(const nlohmann::json &value, const char *path);
nlohmann::json event_predicate_json(const event_predicate &predicate);
nlohmann::json temporal_pattern_json(const temporal_pattern &pattern);
nlohmann::json cut_json(const cut &value);
nlohmann::json policy_json(const query_policy &policy);
nlohmann::json result_schema_json(const result_schema &schema);
nlohmann::json query_value_json(const query_value &value);
query_value query_value_from_json(const nlohmann::json &value);
dynamic_row make_dynamic_row(const result_schema &schema, const nlohmann::json &value);
nlohmann::json dynamic_row_json(const result_schema &schema, const dynamic_row &row);
nlohmann::json dynamic_rows_json(const result_schema &schema, const std::vector<dynamic_row> &rows);
nlohmann::json logical_plan_payload_json(const logical_plan &plan);
std::string json_hash(const nlohmann::json &value);
declaration_reference parse_declaration_reference(const nlohmann::json &value, const char *path);
nlohmann::json declaration_reference_json(const declaration_reference &reference);
void normalize_declaration_basis(query_basis &basis);
std::string admission_outcome_name(admission_outcome outcome);
nlohmann::json admission_evidence_json(const admission_evidence &evidence);
admission_evidence declaration_admission_evidence(const query_basis &basis, uint64_t record_count);
result_schema episode_result_schema();
result_schema temporal_match_result_schema();
result_schema fact_state_result_schema();

} // namespace kungfu::runtime::query::fact_query_internal

namespace kungfu::runtime::query {

nlohmann::json query_authority_json(const query_authority &authority);
nlohmann::json resolved_query_cut_json(const resolved_query_cut &cut);
nlohmann::json query_cut_proof_json(const query_cut_proof &proof);
nlohmann::json query_time_basis_json(const query_time_basis &basis);
nlohmann::json query_execution_json(const query_execution &execution);
nlohmann::json episode_content_root_json(const episode_content_root &root);
nlohmann::json missing_input_json(const missing_input &input);
nlohmann::json unverifiable_input_json(const unverifiable_input &input);
nlohmann::json query_conflict_json(const query_conflict &conflict);

} // namespace kungfu::runtime::query

#endif // KUNGFU_RUNTIME_QUERY_FACT_QUERY_INTERNAL_H
