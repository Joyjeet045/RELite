#include "vm/expression_eval.hpp"

namespace db::vm {

namespace {

bool asBool(const Value& v, bool& out) {
    if (v.type == ValueType::Bool) {
        out = v.boolValue;
        return true;
    }
    return false;
}

Value cachedToValue(const parser::CachedValue& cv) {
    switch (cv.kind) {
        case parser::CachedValue::Kind::Int: return Value::makeInt(cv.intValue);
        case parser::CachedValue::Kind::Float: return Value::makeDouble(cv.doubleValue);
        case parser::CachedValue::Kind::Bool: return Value::makeBool(cv.boolValue);
        case parser::CachedValue::Kind::Text: return Value::makeText(cv.stringValue);
        case parser::CachedValue::Kind::Null: return Value::null();
    }
    return Value::null();
}

bool likeMatch(const std::string& s, const std::string& p) {
    std::size_t si = 0, pi = 0;
    std::size_t star = std::string::npos, mark = 0;
    while (si < s.size()) {
        if (pi < p.size() && (p[pi] == '_' || p[pi] == s[si])) {
            ++si;
            ++pi;
        } else if (pi < p.size() && p[pi] == '%') {
            star = pi++;
            mark = si;
        } else if (star != std::string::npos) {
            pi = star + 1;
            si = ++mark;
        } else {
            return false;
        }
    }
    while (pi < p.size() && p[pi] == '%') ++pi;
    return pi == p.size();
}

}

Value evalExpression(const parser::Expression& expr, const Tuple& tuple) {
    using namespace parser;

    if (auto* lit = dynamic_cast<const LiteralExpr*>(&expr)) {
        switch (lit->kind) {
            case LiteralExpr::Kind::Integer: return Value::makeInt(lit->intValue);
            case LiteralExpr::Kind::Float: return Value::makeDouble(lit->doubleValue);
            case LiteralExpr::Kind::String: return Value::makeText(lit->stringValue);
            case LiteralExpr::Kind::Boolean: return Value::makeBool(lit->boolValue);
            case LiteralExpr::Kind::Null: return Value::null();
        }
        return Value::null();
    }

    if (auto* col = dynamic_cast<const ColumnRef*>(&expr)) {
        if (col->columnIndex < 0 || col->columnIndex >= static_cast<int>(tuple.size())) {
            return Value::null();
        }
        return tuple.at(col->columnIndex);
    }

    if (auto* bin = dynamic_cast<const BinaryExpr*>(&expr)) {
        Value l = evalExpression(*bin->left, tuple);
        Value r = evalExpression(*bin->right, tuple);
        auto cmp = compareValues(l, r);
        if (!cmp.has_value()) {
            return Value::null();
        }
        bool res = false;
        switch (bin->op) {
            case ComparisonOp::Eq: res = (*cmp == 0); break;
            case ComparisonOp::Neq: res = (*cmp != 0); break;
            case ComparisonOp::Lt: res = (*cmp < 0); break;
            case ComparisonOp::Leq: res = (*cmp <= 0); break;
            case ComparisonOp::Gt: res = (*cmp > 0); break;
            case ComparisonOp::Geq: res = (*cmp >= 0); break;
        }
        return Value::makeBool(res);
    }

    if (auto* ar = dynamic_cast<const ArithmeticExpr*>(&expr)) {
        Value l = evalExpression(*ar->left, tuple);
        Value r = evalExpression(*ar->right, tuple);
        bool lNum = (l.type == ValueType::Int || l.type == ValueType::Double);
        bool rNum = (r.type == ValueType::Int || r.type == ValueType::Double);
        if (!lNum || !rNum) return Value::null();
        if (l.type == ValueType::Double || r.type == ValueType::Double) {
            double a = (l.type == ValueType::Double) ? l.doubleValue
                                                     : static_cast<double>(l.intValue);
            double b = (r.type == ValueType::Double) ? r.doubleValue
                                                     : static_cast<double>(r.intValue);
            switch (ar->op) {
                case ArithmeticOp::Add: return Value::makeDouble(a + b);
                case ArithmeticOp::Sub: return Value::makeDouble(a - b);
                case ArithmeticOp::Mul: return Value::makeDouble(a * b);
                case ArithmeticOp::Div:
                    if (b == 0.0) return Value::null();
                    return Value::makeDouble(a / b);
            }
            return Value::null();
        }
        std::int64_t a = l.intValue;
        std::int64_t b = r.intValue;
        switch (ar->op) {
            case ArithmeticOp::Add: return Value::makeInt(a + b);
            case ArithmeticOp::Sub: return Value::makeInt(a - b);
            case ArithmeticOp::Mul: return Value::makeInt(a * b);
            case ArithmeticOp::Div:
                if (b == 0) return Value::null();
                return Value::makeInt(a / b);
        }
        return Value::null();
    }

    if (auto* log = dynamic_cast<const LogicalExpr*>(&expr)) {
        Value l = evalExpression(*log->left, tuple);
        Value r = evalExpression(*log->right, tuple);
        bool lb = false, rb = false;
        bool lHas = asBool(l, lb), rHas = asBool(r, rb);
        if (log->op == LogicalOp::And) {
            if ((lHas && !lb) || (rHas && !rb)) return Value::makeBool(false);
            if (lHas && rHas) return Value::makeBool(true);
            return Value::null();
        }
        if ((lHas && lb) || (rHas && rb)) return Value::makeBool(true);
        if (lHas && rHas) return Value::makeBool(false);
        return Value::null();
    }

    if (auto* un = dynamic_cast<const UnaryExpr*>(&expr)) {
        Value o = evalExpression(*un->operand, tuple);
        if (o.type != ValueType::Bool) return Value::null();
        return Value::makeBool(!o.boolValue);
    }

    if (auto* isnull = dynamic_cast<const IsNullExpr*>(&expr)) {
        Value v = evalExpression(*isnull->operand, tuple);
        bool result = v.isNull();
        return Value::makeBool(isnull->negated ? !result : result);
    }

    if (auto* in = dynamic_cast<const InExpr*>(&expr)) {
        Value v = evalExpression(*in->value, tuple);
        if (v.isNull()) return Value::null();
        bool found = false, sawNull = false;
        if (in->subquery) {
            for (const auto& cv : in->subquery->results) {
                Value iv = cachedToValue(cv);
                if (iv.isNull()) {
                    sawNull = true;
                    continue;
                }
                auto c = compareValues(v, iv);
                if (c.has_value() && *c == 0) {
                    found = true;
                    break;
                }
            }
        } else {
            for (const auto& item : in->items) {
                Value iv = evalExpression(*item, tuple);
                if (iv.isNull()) {
                    sawNull = true;
                    continue;
                }
                auto c = compareValues(v, iv);
                if (c.has_value() && *c == 0) {
                    found = true;
                    break;
                }
            }
        }
        if (found) return Value::makeBool(!in->negated);
        if (sawNull) return Value::null();
        return Value::makeBool(in->negated);
    }

    if (auto* bt = dynamic_cast<const BetweenExpr*>(&expr)) {
        Value v = evalExpression(*bt->value, tuple);
        Value lo = evalExpression(*bt->lo, tuple);
        Value hi = evalExpression(*bt->hi, tuple);
        auto cl = compareValues(v, lo);
        auto ch = compareValues(v, hi);
        if (!cl.has_value() || !ch.has_value()) return Value::null();
        bool inRange = (*cl >= 0) && (*ch <= 0);
        return Value::makeBool(bt->negated ? !inRange : inRange);
    }

    if (auto* lk = dynamic_cast<const LikeExpr*>(&expr)) {
        Value v = evalExpression(*lk->value, tuple);
        Value p = evalExpression(*lk->pattern, tuple);
        if (v.isNull() || p.isNull()) return Value::null();
        if (v.type != ValueType::Text || p.type != ValueType::Text) return Value::null();
        bool m = likeMatch(v.textValue, p.textValue);
        return Value::makeBool(lk->negated ? !m : m);
    }

    if (auto* sub = dynamic_cast<const SubqueryExpr*>(&expr)) {
        if (sub->kind == SubqueryExpr::Kind::Exists) {
            return Value::makeBool(!sub->results.empty());
        }
        if (sub->results.empty()) return Value::null();
        return cachedToValue(sub->results.front());
    }

    return Value::null();
}

bool predicateTrue(const parser::Expression& expr, const Tuple& tuple) {
    Value v = evalExpression(expr, tuple);
    return v.type == ValueType::Bool && v.boolValue;
}

}
