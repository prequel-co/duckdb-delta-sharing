#include "duckdb_delta_sharing_functions.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_between_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/parser/expression/between_expression.hpp"

namespace duckdb {








// ParsedExpression to JSON helpers for string predicate parsing

json ParsedOperandJSON(const ParsedExpression &expr) {
    json res{};

    if (expr.GetExpressionClass() == ExpressionClass::COLUMN_REF) {
        auto &column = expr.Cast<ColumnRefExpression>();
        res["op"] = "column";
        res["name"] = column.GetColumnName().GetIdentifierName();
        res["valueType"] = "string";  // Default type, will be inferred from context
    } else if (expr.GetExpressionClass() == ExpressionClass::CONSTANT) {
        auto &literal = expr.Cast<ConstantExpression>();
        auto val = literal.GetLiteral().ToValue();
        res["op"] = "literal";
        res["value"] = val.ToString();
        // Infer valueType from Value type
        switch (val.type().id()) {
            case LogicalTypeId::BOOLEAN:
            case LogicalTypeId::TINYINT:
            case LogicalTypeId::SMALLINT:
            case LogicalTypeId::INTEGER:
            case LogicalTypeId::BIGINT:
            case LogicalTypeId::FLOAT:
            case LogicalTypeId::DOUBLE:
                res["valueType"] = "int";
                break;
            case LogicalTypeId::VARCHAR:
            default:
                res["valueType"] = "string";
                break;
        }
    }
    return res;
}

json ParsedBinaryOpJSON(const ParsedExpression &expr, std::string op) {
    json res{};
    res["op"] = op;
    res["children"] = json::array();
    auto &comp_expr = expr.Cast<ComparisonExpression>();
    res["children"].push_back(ParsedOperandJSON(comp_expr.Left()));
    res["children"].push_back(ParsedOperandJSON(comp_expr.Right()));
    return res;
}

json ParseParsedExpressionHint(const ParsedExpression &expr) {
    json res{};
    std::string op;

    switch (expr.GetExpressionType()) {
        case ExpressionType::COMPARE_EQUAL: {
            op = "equal";
            res = ParsedBinaryOpJSON(expr, op);
        } break;
        case ExpressionType::COMPARE_LESSTHAN: {
            op = "lessThan";
            res = ParsedBinaryOpJSON(expr, op);
        } break;
        case ExpressionType::COMPARE_GREATERTHAN: {
            op = "greaterThan";
            res = ParsedBinaryOpJSON(expr, op);
        } break;
        case ExpressionType::COMPARE_LESSTHANOREQUALTO: {
            op = "lessThanOrEqual";
            res = ParsedBinaryOpJSON(expr, op);
        } break;
        case ExpressionType::COMPARE_GREATERTHANOREQUALTO: {
            op = "greaterThanOrEqual";
            res = ParsedBinaryOpJSON(expr, op);
        } break;
        case ExpressionType::COMPARE_NOTEQUAL: {
            op = "equal";
            res["op"] = "not";
            res["children"] = json::array();
            json equal_predicate = ParsedBinaryOpJSON(expr, op);
            res["children"].push_back(equal_predicate);
        } break;
        case ExpressionType::OPERATOR_IS_NULL: {
            res["op"] = "isNull";
            auto &op_expr = expr.Cast<OperatorExpression>();
            res["children"] = json::array();
            if (!op_expr.GetChildren().empty()) {
                res["children"].push_back(ParsedOperandJSON(*op_expr.GetChildren()[0]));
            }
        } break;
        case ExpressionType::OPERATOR_IS_NOT_NULL: {
            res["op"] = "not";
            auto &op_expr = expr.Cast<OperatorExpression>();
            res["children"] = json::array();
            json is_null_child{};
            is_null_child["op"] = "isNull";
            is_null_child["children"] = json::array();
            if (!op_expr.GetChildren().empty()) {
                is_null_child["children"].push_back(ParsedOperandJSON(*op_expr.GetChildren()[0]));
            }
            res["children"].push_back(is_null_child);
        } break;
        case ExpressionType::CONJUNCTION_AND: {
            auto &expr_node = expr.Cast<ConjunctionExpression>();
            res["op"] = "and";
            res["children"] = json::array();
            for (auto &child : expr_node.GetChildren()) {
                res["children"].push_back(ParseParsedExpressionHint(*child));
            }
        } break;
        case ExpressionType::CONJUNCTION_OR: {
            auto &expr_node = expr.Cast<ConjunctionExpression>();
            res["op"] = "or";
            res["children"] = json::array();
            for (auto &child : expr_node.GetChildren()) {
                res["children"].push_back(ParseParsedExpressionHint(*child));
            }
        } break;
        case ExpressionType::COMPARE_BETWEEN: {
            res["op"] = "and";
            res["children"] = json::array();
            auto &between = expr.Cast<BetweenExpression>();
            // Create >= lower and <= upper comparisons
            json lower_cmp{};
            lower_cmp["op"] = "greaterThanOrEqual";
            lower_cmp["children"] = json::array();
            lower_cmp["children"].push_back(ParsedOperandJSON(between.Input()));
            lower_cmp["children"].push_back(ParsedOperandJSON(between.LowerBound()));

            json upper_cmp{};
            upper_cmp["op"] = "lessThanOrEqual";
            upper_cmp["children"] = json::array();
            upper_cmp["children"].push_back(ParsedOperandJSON(between.Input()));
            upper_cmp["children"].push_back(ParsedOperandJSON(between.UpperBound()));

            res["children"].push_back(lower_cmp);
            res["children"].push_back(upper_cmp);
        } break;
        default:
            break;
    }
    return res;
}

json ParsePredicateStringToJson(const std::string &predicate_string) {
    if (predicate_string.empty()) {
        return json{};
    }

    try {
        // Use DuckDB's parser to parse the predicate string
        auto parser = Parser::GetBuiltinParser();
        auto expression_list = parser.ParseExpressionList(predicate_string);

        if (expression_list.empty()) {
            return json{};
        }

        // Combine multiple expressions with AND if needed
        if (expression_list.size() == 1) {
            return ParseParsedExpressionHint(*expression_list[0]);
        }

        json combined{};
        combined["op"] = "and";
        combined["children"] = json::array();
        for (auto &expr : expression_list) {
            combined["children"].push_back(ParseParsedExpressionHint(*expr));
        }
        return combined;
    } catch (const std::exception &e) {
        throw IOException("ParsePredicateStringToJson error: failed to parse predicate '" +
                         predicate_string + "': " + std::string(e.what()));
    }
}

} // namespace duckdb
