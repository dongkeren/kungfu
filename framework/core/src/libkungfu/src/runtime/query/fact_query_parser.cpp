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

// Bounded SQL front-end. The dialect is a closed, non-recursive grammar
// (KF-ADR-019f86da-4f90-7e38-b72f-ef8829e14104), so it is spelled out as a tokenizer plus a recursive-descent
// parser rather than one opaque expression. Each clause is its own function,
// which is what keeps a new clause a local addition instead of an edit to a
// shared pattern.
//
// Accepted grammar (case-insensitive):
//   SELECT * FROM episodes
//     [WHERE episode_id = <u64>]
//     [ORDER BY episode_id ASC]
//     [LIMIT <1..1000>]
//   SELECT * FROM episodes MATCH_RECOGNIZE (
//     PARTITION BY <field> ORDER BY <field> ASC
//     PATTERN ((A B){<u64>,<u64>})
//     DEFINE A AS <field> = '<value>', B AS <field> = '<value>'
//     WITHIN <u64> AS OF <u64> [ABSENT <field> = '<value>'])
//     [LIMIT <u64>]
// A single optional trailing semicolon is accepted. Everything else fails
// closed rather than being delegated to SQLite.

constexpr size_t SQL_MAX_FIELD_NAME = 64;
constexpr size_t SQL_MAX_QUOTED_VALUE = 256;
constexpr const char *SQL_ACCEPTED_FORMS = "accepted forms: SELECT * FROM episodes [WHERE episode_id = N] "
                                           "[ORDER BY episode_id ASC] [LIMIT N], or the bounded MATCH_RECOGNIZE form";

enum class sql_token_kind { word, number, quoted, symbol, end };

struct sql_token {
  sql_token_kind kind{sql_token_kind::end};
  // Lexeme for words and numbers, body for quoted values, single character for
  // symbols.
  std::string text;
  size_t offset{0};
  // Whitespace immediately precedes this token. The dialect requires a
  // separator in some junctions (SELECT *) and allows none in others (a='x'),
  // so the distinction has to survive tokenization.
  bool spaced{false};
};

// The classic "C" locale space set the previous std::regex \s resolved to.
bool is_sql_space(char ch) { return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\v' || ch == '\f' || ch == '\r'; }

bool is_sql_alpha(char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'); }

bool is_sql_digit(char ch) { return ch >= '0' && ch <= '9'; }

bool is_sql_word_start(char ch) { return is_sql_alpha(ch) || ch == '_'; }

bool is_sql_word_char(char ch) { return is_sql_word_start(ch) || is_sql_digit(ch); }

bool is_sql_symbol(char ch) {
  return ch == '*' || ch == '(' || ch == ')' || ch == '{' || ch == '}' || ch == ',' || ch == '=' || ch == ';';
}

// The quoted-value class carries the whole trust boundary: no quote, no escape,
// no backslash can appear inside a value, so a value can never re-enter the
// grammar. Widening it is a security decision, not a parser detail.
bool is_sql_quoted_char(char ch) {
  return is_sql_word_char(ch) || ch == '.' || ch == ':' || ch == '@' || ch == '/' || ch == '+' || ch == '-';
}

[[noreturn]] void sql_error(size_t offset, const std::string &message) {
  throw std::invalid_argument("unsupported SQL at byte " + std::to_string(offset) + ": " + message + "; " +
                              SQL_ACCEPTED_FORMS);
}

std::vector<sql_token> tokenize_bounded_sql(const std::string &sql) {
  std::vector<sql_token> tokens;
  size_t index = 0;
  bool spaced = false;
  while (index < sql.size()) {
    const char ch = sql[index];
    if (is_sql_space(ch)) {
      spaced = true;
      ++index;
      continue;
    }
    sql_token token;
    token.offset = index;
    token.spaced = spaced;
    if (is_sql_word_start(ch)) {
      const auto begin = index;
      while (index < sql.size() && is_sql_word_char(sql[index])) {
        ++index;
      }
      token.kind = sql_token_kind::word;
      token.text = sql.substr(begin, index - begin);
    } else if (is_sql_digit(ch)) {
      const auto begin = index;
      while (index < sql.size() && is_sql_digit(sql[index])) {
        ++index;
      }
      token.kind = sql_token_kind::number;
      token.text = sql.substr(begin, index - begin);
    } else if (ch == '\'') {
      const auto begin = index + 1;
      auto scan = begin;
      while (scan < sql.size() && sql[scan] != '\'') {
        ++scan;
      }
      if (scan >= sql.size()) {
        sql_error(token.offset, "unterminated quoted value");
      }
      token.kind = sql_token_kind::quoted;
      token.text = sql.substr(begin, scan - begin);
      index = scan + 1;
    } else if (is_sql_symbol(ch)) {
      token.kind = sql_token_kind::symbol;
      token.text = std::string(1, ch);
      ++index;
    } else {
      sql_error(index, "unexpected character");
    }
    tokens.push_back(std::move(token));
    spaced = false;
  }
  sql_token end_token;
  end_token.kind = sql_token_kind::end;
  end_token.offset = sql.size();
  end_token.spaced = spaced;
  tokens.push_back(std::move(end_token));
  return tokens;
}

class bounded_sql_parser {
public:
  explicit bounded_sql_parser(const std::string &sql) : tokens_(tokenize_bounded_sql(sql)) {}

  // SELECT * FROM episodes <tail>
  query_definition parse(const query_definition &base_definition) {
    expect_word("select", false);
    expect_symbol('*', true);
    expect_word("from", true);
    expect_word("episodes", true);
    auto compiled = base_definition;
    compiled.basis.episode_id = 0;
    if (at_word("match_recognize")) {
      parse_match_recognize(compiled);
    } else {
      parse_plain_tail(compiled);
    }
    expect_statement_end();
    // Round-trip through the strict parser so a caller cannot use SQL as a way
    // around QueryDefinition validation.
    return parse_query_definition(query_definition_json(compiled));
  }

private:
  const sql_token &peek() const { return tokens_[index_]; }

  const sql_token &advance() { return tokens_[index_++]; }

  bool at_word(const char *lowercase_keyword) const {
    return peek().kind == sql_token_kind::word && ascii_lower(peek().text) == lowercase_keyword;
  }

  bool at_symbol(char symbol) const { return peek().kind == sql_token_kind::symbol && peek().text.front() == symbol; }

  void require_gap(bool required, const std::string &before) const {
    if (required && !peek().spaced) {
      sql_error(peek().offset, "expected whitespace before " + before);
    }
  }

  void expect_word(const char *lowercase_keyword, bool require_space) {
    require_gap(require_space, lowercase_keyword);
    if (!at_word(lowercase_keyword)) {
      sql_error(peek().offset, std::string("expected ") + lowercase_keyword);
    }
    advance();
  }

  void expect_symbol(char symbol, bool require_space) {
    require_gap(require_space, std::string("'") + symbol + "'");
    if (!at_symbol(symbol)) {
      sql_error(peek().offset, std::string("expected '") + symbol + "'");
    }
    advance();
  }

  // Numeric range is not checked here; the QueryDefinition parser owns it.
  std::string expect_number(const char *field, bool require_space) {
    require_gap(require_space, field);
    if (peek().kind != sql_token_kind::number) {
      sql_error(peek().offset, std::string("expected an unsigned number for ") + field);
    }
    return advance().text;
  }

  std::string expect_field_name(const char *field, bool require_space) {
    require_gap(require_space, field);
    const auto &token = peek();
    if (token.kind != sql_token_kind::word) {
      sql_error(token.offset, std::string("expected a field name for ") + field);
    }
    if (!is_sql_alpha(token.text.front()) || token.text.size() > SQL_MAX_FIELD_NAME) {
      sql_error(token.offset, std::string("invalid field name for ") + field + "; expected [A-Za-z][A-Za-z0-9_]{0,63}");
    }
    return ascii_lower(advance().text);
  }

  std::string expect_quoted_value(const char *field, bool require_space) {
    require_gap(require_space, field);
    const auto &token = peek();
    if (token.kind != sql_token_kind::quoted) {
      sql_error(token.offset, std::string("expected a quoted value for ") + field);
    }
    if (token.text.empty() || token.text.size() > SQL_MAX_QUOTED_VALUE) {
      sql_error(token.offset, std::string("invalid quoted value for ") + field + "; expected 1..256 characters");
    }
    if (!std::all_of(token.text.begin(), token.text.end(), is_sql_quoted_char)) {
      sql_error(token.offset,
                std::string("invalid quoted value for ") + field + "; expected only [A-Za-z0-9_.:@/+-] characters");
    }
    return advance().text;
  }

  // [WHERE episode_id = <u64>] [ORDER BY episode_id ASC] [LIMIT <1..1000>]
  void parse_plain_tail(query_definition &compiled) {
    compiled.has_temporal_pattern = false;
    if (at_word("where")) {
      expect_word("where", true);
      expect_word("episode_id", true);
      expect_symbol('=', false);
      compiled.basis.episode_id = uint64_value(expect_number("sql.episode_id", false), "sql.episode_id");
    }
    if (at_word("order")) {
      expect_word("order", true);
      expect_word("by", true);
      expect_word("episode_id", true);
      expect_word("asc", true);
    }
    if (at_word("limit")) {
      expect_word("limit", true);
      compiled.limit = uint64_value(expect_number("sql.limit", true), "sql.limit");
      if (compiled.limit == 0 || compiled.limit > 1000) {
        throw std::invalid_argument("sql LIMIT must be between 1 and 1000");
      }
    }
  }

  // MATCH_RECOGNIZE (...) [LIMIT <u64>]
  void parse_match_recognize(query_definition &compiled) {
    compiled.has_temporal_pattern = true;
    compiled.pattern = {};
    expect_word("match_recognize", true);
    expect_symbol('(', false);
    parse_partition_clause(compiled.pattern);
    parse_order_clause(compiled.pattern);
    parse_pattern_clause(compiled.pattern);
    parse_define_clause(compiled.pattern);
    parse_within_clause(compiled.pattern);
    parse_as_of_clause(compiled.pattern);
    parse_optional_absent_clause(compiled.pattern);
    expect_symbol(')', false);
    if (at_word("limit")) {
      expect_word("limit", true);
      compiled.limit = uint64_value(expect_number("sql.limit", true), "sql.limit");
    }
  }

  // PARTITION BY <field>
  void parse_partition_clause(temporal_pattern &pattern) {
    expect_word("partition", false);
    expect_word("by", true);
    pattern.partition_by = expect_field_name("sql.pattern.partition_by", true);
  }

  // ORDER BY <field> ASC
  void parse_order_clause(temporal_pattern &pattern) {
    expect_word("order", true);
    expect_word("by", true);
    pattern.order_by = expect_field_name("sql.pattern.order_by", true);
    expect_word("asc", true);
  }

  // PATTERN ((A B){<u64>,<u64>})
  void parse_pattern_clause(temporal_pattern &pattern) {
    expect_word("pattern", true);
    expect_symbol('(', false);
    expect_symbol('(', false);
    expect_word("a", false);
    expect_word("b", true);
    expect_symbol(')', false);
    expect_symbol('{', false);
    pattern.repeat_min = uint64_value(expect_number("sql.pattern.repeat_min", false), "sql.pattern.repeat_min");
    expect_symbol(',', false);
    pattern.repeat_max = uint64_value(expect_number("sql.pattern.repeat_max", false), "sql.pattern.repeat_max");
    expect_symbol('}', false);
    expect_symbol(')', false);
  }

  // DEFINE A AS <field> = '<value>', B AS <field> = '<value>'
  void parse_define_clause(temporal_pattern &pattern) {
    expect_word("define", true);
    expect_word("a", true);
    expect_word("as", true);
    const auto first_field = expect_field_name("sql.pattern.sequence[0].field", true);
    expect_symbol('=', false);
    const auto first_value = expect_quoted_value("sql.pattern.sequence[0].equals", false);
    expect_symbol(',', false);
    expect_word("b", false);
    expect_word("as", true);
    const auto second_field = expect_field_name("sql.pattern.sequence[1].field", true);
    expect_symbol('=', false);
    const auto second_value = expect_quoted_value("sql.pattern.sequence[1].equals", false);
    pattern.sequence = {{first_field, first_value}, {second_field, second_value}};
  }

  // WITHIN <u64>
  void parse_within_clause(temporal_pattern &pattern) {
    expect_word("within", true);
    pattern.within_ns = uint64_value(expect_number("sql.pattern.within_ns", true), "sql.pattern.within_ns");
  }

  // AS OF <u64>
  void parse_as_of_clause(temporal_pattern &pattern) {
    expect_word("as", true);
    expect_word("of", true);
    pattern.as_of_time = int64_value(expect_number("sql.pattern.as_of_time", true), "sql.pattern.as_of_time");
  }

  // [ABSENT <field> = '<value>']
  void parse_optional_absent_clause(temporal_pattern &pattern) {
    if (!at_word("absent")) {
      return;
    }
    expect_word("absent", true);
    const auto field = expect_field_name("sql.pattern.absence.field", true);
    expect_symbol('=', false);
    const auto value = expect_quoted_value("sql.pattern.absence.equals", false);
    pattern.has_absence = true;
    pattern.absence = {field, value};
  }

  // An optional trailing semicolon, then nothing.
  void expect_statement_end() {
    if (at_symbol(';')) {
      advance();
    }
    if (peek().kind != sql_token_kind::end) {
      sql_error(peek().offset, "unexpected trailing input");
    }
  }

  std::vector<sql_token> tokens_;
  size_t index_{0};
};

} // namespace

query_definition compile_episode_sql(const std::string &sql, const query_definition &base_definition) {
  if (sql.empty() || sql.size() > 4096) {
    throw std::invalid_argument("bounded SQL must contain 1..4096 bytes");
  }
  return bounded_sql_parser(sql).parse(base_definition);
}

} // namespace kungfu::runtime::query
