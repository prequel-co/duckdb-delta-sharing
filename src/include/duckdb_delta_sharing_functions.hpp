#pragma once

#include "duckdb.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/parser/parsed_expression.hpp"
#include "delta_sharing_json.hpp"
#include <nlohmann/json.hpp>
#include <unordered_set>

namespace duckdb {

using json = nlohmann::json;

// ParsedExpression to JSON helpers (for string predicate parsing)
json ParsedOperandJSON(ParsedExpression &expr);

json ParsedBinaryOpJSON(ParsedExpression &expr, std::string op);

json ParseParsedExpressionHint(ParsedExpression &expr);

json ParsePredicateStringToJson(const std::string &predicate_string);

} // namespace duckdb
