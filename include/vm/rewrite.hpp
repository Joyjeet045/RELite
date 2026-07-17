#pragma once

#include "frontend/ast.hpp"

namespace db::vm {

/*
 * Rule-based query rewrites applied before execution:
 *  - constant folding: a subtree built only from literals and arithmetic,
 *    comparison, logical, or unary operators is evaluated once and replaced by
 *    its literal result (e.g. 2 + 3 * 4 -> 14, 1 = 1 -> TRUE);
 *  - boolean simplification: TRUE AND x -> x, FALSE AND x -> FALSE,
 *    FALSE OR x -> x, TRUE OR x -> TRUE;
 *  - arithmetic identities: x + 0 -> x, x - 0 -> x, x * 1 -> x.
 * These are null-safe and type-preserving, so they never change results.
 */

/* Rewrite an expression, returning a possibly-new tree (ownership transfers). */
parser::ExpressionPtr rewriteExpression(parser::ExpressionPtr expr);

/* Rewrite a SELECT's WHERE, HAVING, and computed projection expressions. */
void optimizeSelect(parser::SelectStatement& node);

}
