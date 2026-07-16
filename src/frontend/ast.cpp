#include "frontend/ast.hpp"

#include <stdexcept>

namespace db::parser {

std::string_view dataTypeName(DataType type) {
    switch (type) {
        case DataType::Int: return "INT";
        case DataType::Bool: return "BOOL";
        case DataType::Text: return "TEXT";
        case DataType::Varchar: return "VARCHAR";
        case DataType::Float: return "FLOAT";
        case DataType::Date: return "DATE";
        case DataType::Timestamp: return "TIMESTAMP";
    }
    return "UNKNOWN";
}

std::string_view comparisonOpName(ComparisonOp op) {
    switch (op) {
        case ComparisonOp::Eq: return "=";
        case ComparisonOp::Neq: return "!=";
        case ComparisonOp::Lt: return "<";
        case ComparisonOp::Leq: return "<=";
        case ComparisonOp::Gt: return ">";
        case ComparisonOp::Geq: return ">=";
    }
    return "?";
}

namespace {

std::string quoteString(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "''";
        else out += c;
    }
    out += "'";
    return out;
}

std::string literalText(const LiteralExpr& l) {
    switch (l.kind) {
        case LiteralExpr::Kind::Integer: return std::to_string(l.intValue);
        case LiteralExpr::Kind::Float: return std::to_string(l.doubleValue);
        case LiteralExpr::Kind::String: return quoteString(l.stringValue);
        case LiteralExpr::Kind::Boolean: return l.boolValue ? "TRUE" : "FALSE";
        case LiteralExpr::Kind::Null: return "NULL";
    }
    return "NULL";
}

}

std::string expressionToString(const Expression& e) {
    if (auto* l = dynamic_cast<const LiteralExpr*>(&e)) {
        return literalText(*l);
    }
    if (auto* c = dynamic_cast<const ColumnRef*>(&e)) {
        return c->table.empty() ? c->column : (c->table + "." + c->column);
    }
    if (auto* b = dynamic_cast<const BinaryExpr*>(&e)) {
        return "(" + expressionToString(*b->left) + " " +
               std::string(comparisonOpName(b->op)) + " " +
               expressionToString(*b->right) + ")";
    }
    if (auto* a = dynamic_cast<const ArithmeticExpr*>(&e)) {
        const char* op = "+";
        switch (a->op) {
            case ArithmeticOp::Add: op = "+"; break;
            case ArithmeticOp::Sub: op = "-"; break;
            case ArithmeticOp::Mul: op = "*"; break;
            case ArithmeticOp::Div: op = "/"; break;
        }
        return "(" + expressionToString(*a->left) + " " + op + " " +
               expressionToString(*a->right) + ")";
    }
    if (auto* lg = dynamic_cast<const LogicalExpr*>(&e)) {
        std::string op = (lg->op == LogicalOp::And) ? " AND " : " OR ";
        return "(" + expressionToString(*lg->left) + op +
               expressionToString(*lg->right) + ")";
    }
    if (auto* u = dynamic_cast<const UnaryExpr*>(&e)) {
        return "(NOT " + expressionToString(*u->operand) + ")";
    }
    if (auto* n = dynamic_cast<const IsNullExpr*>(&e)) {
        return "(" + expressionToString(*n->operand) +
               (n->negated ? " IS NOT NULL)" : " IS NULL)");
    }
    if (auto* bt = dynamic_cast<const BetweenExpr*>(&e)) {
        return "(" + expressionToString(*bt->value) +
               (bt->negated ? " NOT BETWEEN " : " BETWEEN ") +
               expressionToString(*bt->lo) + " AND " + expressionToString(*bt->hi) + ")";
    }
    if (auto* lk = dynamic_cast<const LikeExpr*>(&e)) {
        return "(" + expressionToString(*lk->value) +
               (lk->negated ? " NOT LIKE " : " LIKE ") +
               expressionToString(*lk->pattern) + ")";
    }
    if (auto* in = dynamic_cast<const InExpr*>(&e)) {
        if (in->subquery) throw std::runtime_error("subquery not allowed in CHECK");
        std::string out =
            "(" + expressionToString(*in->value) + (in->negated ? " NOT IN (" : " IN (");
        for (std::size_t i = 0; i < in->items.size(); ++i) {
            if (i) out += ", ";
            out += expressionToString(*in->items[i]);
        }
        out += "))";
        return out;
    }
    if (auto* c = dynamic_cast<const CallExpr*>(&e)) {
        if (c->isCast) {
            return "CAST(" + expressionToString(*c->args[0]) + " AS " +
                   std::string(dataTypeName(c->castType)) + ")";
        }
        std::string out = c->name + "(";
        for (std::size_t i = 0; i < c->args.size(); ++i) {
            if (i) out += ", ";
            out += expressionToString(*c->args[i]);
        }
        out += ")";
        return out;
    }
    if (auto* cs = dynamic_cast<const CaseExpr*>(&e)) {
        std::string out = "CASE";
        for (const auto& br : cs->branches) {
            out += " WHEN " + expressionToString(*br.when) + " THEN " +
                   expressionToString(*br.then);
        }
        if (cs->elseExpr) out += " ELSE " + expressionToString(*cs->elseExpr);
        out += " END";
        return out;
    }
    if (auto* w = dynamic_cast<const WindowExpr*>(&e)) {
        std::string out = w->name + "(";
        if (w->argument) out += w->argument->column;
        out += ") OVER";
        return out;
    }
    if (auto* sq = dynamic_cast<const SubqueryExpr*>(&e)) {
        return sq->kind == SubqueryExpr::Kind::Exists ? "EXISTS(...)" : "(subquery)";
    }
    throw std::runtime_error("unsupported expression in CHECK constraint");
}

void LiteralExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void ColumnRef::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void BinaryExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void ArithmeticExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void LogicalExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void UnaryExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void IsNullExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void InExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void BetweenExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void LikeExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void FunctionExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void CallExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void CaseExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void WindowExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void SubqueryExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void CreateStatement::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void CreateIdxStatement::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void InsertStatement::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void SelectStatement::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void DeleteStatement::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void UpdateStatement::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void DropStatement::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void AlterStatement::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void TransactionStatement::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void SetOpStatement::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void CreateViewStatement::accept(ASTVisitor& visitor) { visitor.visit(*this); }

}
