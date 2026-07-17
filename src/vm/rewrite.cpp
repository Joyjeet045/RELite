#include "vm/rewrite.hpp"

#include <memory>
#include <utility>

#include "vm/expression_eval.hpp"
#include "vm/tuple.hpp"
#include "vm/value.hpp"

namespace db::vm {

using parser::ArithmeticExpr;
using parser::ArithmeticOp;
using parser::BinaryExpr;
using parser::Expression;
using parser::ExpressionPtr;
using parser::LiteralExpr;
using parser::LogicalExpr;
using parser::LogicalOp;
using parser::UnaryExpr;

namespace {

/* A subtree that contains only literals combined by arithmetic, comparison,
 * logical, or unary operators can be evaluated to a constant. */
bool isFoldable(const Expression& e) {
    if (dynamic_cast<const LiteralExpr*>(&e)) return true;
    if (auto* a = dynamic_cast<const ArithmeticExpr*>(&e)) {
        return a->left && a->right && isFoldable(*a->left) && isFoldable(*a->right);
    }
    if (auto* b = dynamic_cast<const BinaryExpr*>(&e)) {
        return b->left && b->right && isFoldable(*b->left) && isFoldable(*b->right);
    }
    if (auto* l = dynamic_cast<const LogicalExpr*>(&e)) {
        return l->left && l->right && isFoldable(*l->left) && isFoldable(*l->right);
    }
    if (auto* u = dynamic_cast<const UnaryExpr*>(&e)) {
        return u->operand && isFoldable(*u->operand);
    }
    return false;
}

ExpressionPtr makeLiteral(const Value& v) {
    auto lit = std::make_unique<LiteralExpr>();
    switch (v.type) {
        case ValueType::Int:
            lit->kind = LiteralExpr::Kind::Integer;
            lit->intValue = v.intValue;
            lit->resolvedType = parser::DataType::Int;
            break;
        case ValueType::Double:
            lit->kind = LiteralExpr::Kind::Float;
            lit->doubleValue = v.doubleValue;
            lit->resolvedType = parser::DataType::Float;
            break;
        case ValueType::Bool:
            lit->kind = LiteralExpr::Kind::Boolean;
            lit->boolValue = v.boolValue;
            lit->resolvedType = parser::DataType::Bool;
            break;
        case ValueType::Text:
            lit->kind = LiteralExpr::Kind::String;
            lit->stringValue = v.textValue;
            lit->resolvedType = parser::DataType::Text;
            break;
        case ValueType::Null:
            lit->kind = LiteralExpr::Kind::Null;
            break;
    }
    return lit;
}

ExpressionPtr makeBoolLiteral(bool b) {
    auto lit = std::make_unique<LiteralExpr>();
    lit->kind = LiteralExpr::Kind::Boolean;
    lit->boolValue = b;
    lit->resolvedType = parser::DataType::Bool;
    return lit;
}

const LiteralExpr* asLiteral(const Expression* e) {
    return dynamic_cast<const LiteralExpr*>(e);
}

bool isNumericLiteral(const Expression* e, double want) {
    const LiteralExpr* lit = asLiteral(e);
    if (lit == nullptr) return false;
    if (lit->kind == LiteralExpr::Kind::Integer) {
        return static_cast<double>(lit->intValue) == want;
    }
    if (lit->kind == LiteralExpr::Kind::Float) return lit->doubleValue == want;
    return false;
}

bool isBoolLiteral(const Expression* e, bool want) {
    const LiteralExpr* lit = asLiteral(e);
    return lit != nullptr && lit->kind == LiteralExpr::Kind::Boolean &&
           lit->boolValue == want;
}

}  // namespace

ExpressionPtr rewriteExpression(ExpressionPtr expr) {
    if (!expr) return expr;

    if (isFoldable(*expr)) {
        Tuple empty;
        Value v = evalExpression(*expr, empty);
        return makeLiteral(v);
    }

    if (auto* a = dynamic_cast<ArithmeticExpr*>(expr.get())) {
        a->left = rewriteExpression(std::move(a->left));
        a->right = rewriteExpression(std::move(a->right));
        switch (a->op) {
            case ArithmeticOp::Add:
                if (isNumericLiteral(a->right.get(), 0.0)) return std::move(a->left);
                if (isNumericLiteral(a->left.get(), 0.0)) return std::move(a->right);
                break;
            case ArithmeticOp::Sub:
                if (isNumericLiteral(a->right.get(), 0.0)) return std::move(a->left);
                break;
            case ArithmeticOp::Mul:
                if (isNumericLiteral(a->right.get(), 1.0)) return std::move(a->left);
                if (isNumericLiteral(a->left.get(), 1.0)) return std::move(a->right);
                break;
            case ArithmeticOp::Div:
                if (isNumericLiteral(a->right.get(), 1.0)) return std::move(a->left);
                break;
        }
        return expr;
    }

    if (auto* l = dynamic_cast<LogicalExpr*>(expr.get())) {
        l->left = rewriteExpression(std::move(l->left));
        l->right = rewriteExpression(std::move(l->right));
        if (l->op == LogicalOp::And) {
            if (isBoolLiteral(l->left.get(), false) ||
                isBoolLiteral(l->right.get(), false)) {
                return makeBoolLiteral(false);
            }
            if (isBoolLiteral(l->left.get(), true)) return std::move(l->right);
            if (isBoolLiteral(l->right.get(), true)) return std::move(l->left);
        } else {
            if (isBoolLiteral(l->left.get(), true) ||
                isBoolLiteral(l->right.get(), true)) {
                return makeBoolLiteral(true);
            }
            if (isBoolLiteral(l->left.get(), false)) return std::move(l->right);
            if (isBoolLiteral(l->right.get(), false)) return std::move(l->left);
        }
        return expr;
    }

    if (auto* b = dynamic_cast<BinaryExpr*>(expr.get())) {
        b->left = rewriteExpression(std::move(b->left));
        b->right = rewriteExpression(std::move(b->right));
        return expr;
    }

    if (auto* u = dynamic_cast<UnaryExpr*>(expr.get())) {
        u->operand = rewriteExpression(std::move(u->operand));
        return expr;
    }

    return expr;
}

void optimizeSelect(parser::SelectStatement& node) {
    if (node.where) node.where = rewriteExpression(std::move(node.where));
    if (node.having) node.having = rewriteExpression(std::move(node.having));
    for (auto& col : node.columns) {
        if (col && col->computed) {
            col->computed = rewriteExpression(std::move(col->computed));
        }
    }
}

}
