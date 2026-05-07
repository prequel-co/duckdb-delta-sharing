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

json ParsedOperandJSON(ParsedExpression &expr) {
    json res{};

    if (expr.expression_class == ExpressionClass::COLUMN_REF) {
        auto &column = expr.Cast<ColumnRefExpression>();
        res["op"] = "column";
        res["name"] = column.GetColumnName();
        res["valueType"] = "string";  // Default type, will be inferred from context
    } else if (expr.expression_class == ExpressionClass::CONSTANT) {
        auto &literal = expr.Cast<ConstantExpression>();
        res["op"] = "literal";
        res["value"] = literal.value.ToString();
        // Infer valueType from Value type
        switch (literal.value.type().id()) {
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

json ParsedBinaryOpJSON(ParsedExpression &expr, std::string op) {
    json res{};
    res["op"] = op;
    res["children"] = json::array();
    auto &comp_expr = expr.Cast<ComparisonExpression>();
    if (comp_expr.left) res["children"].push_back(ParsedOperandJSON(*comp_expr.left));
    if (comp_expr.right) res["children"].push_back(ParsedOperandJSON(*comp_expr.right));
    return res;
}

json ParseParsedExpressionHint(ParsedExpression &expr) {
    json res{};
    std::string op;

    switch (expr.type) {
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
            if (!op_expr.children.empty()) {
                res["children"].push_back(ParsedOperandJSON(*op_expr.children[0]));
            }
        } break;
        case ExpressionType::OPERATOR_IS_NOT_NULL: {
            res["op"] = "not";
            auto &op_expr = expr.Cast<OperatorExpression>();
            res["children"] = json::array();
            json is_null_child{};
            is_null_child["op"] = "isNull";
            is_null_child["children"] = json::array();
            if (!op_expr.children.empty()) {
                is_null_child["children"].push_back(ParsedOperandJSON(*op_expr.children[0]));
            }
            res["children"].push_back(is_null_child);
        } break;
        case ExpressionType::CONJUNCTION_AND: {
            auto &expr_node = expr.Cast<ConjunctionExpression>();
            res["op"] = "and";
            res["children"] = json::array();
            for (auto &child : expr_node.children) {
                res["children"].push_back(ParseParsedExpressionHint(*child));
            }
        } break;
        case ExpressionType::CONJUNCTION_OR: {
            auto &expr_node = expr.Cast<ConjunctionExpression>();
            res["op"] = "or";
            res["children"] = json::array();
            for (auto &child : expr_node.children) {
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
            lower_cmp["children"].push_back(ParsedOperandJSON(*between.input));
            lower_cmp["children"].push_back(ParsedOperandJSON(*between.lower));

            json upper_cmp{};
            upper_cmp["op"] = "lessThanOrEqual";
            upper_cmp["children"] = json::array();
            upper_cmp["children"].push_back(ParsedOperandJSON(*between.input));
            upper_cmp["children"].push_back(ParsedOperandJSON(*between.upper));

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
        auto expression_list = Parser::ParseExpressionList(predicate_string);

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
