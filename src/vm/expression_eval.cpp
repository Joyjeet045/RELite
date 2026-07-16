#include "vm/expression_eval.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <string>

namespace db::vm {

namespace {

bool asBool(const Value& v, bool& out) {
    if (v.type == ValueType::Bool) {
        out = v.boolValue;
        return true;
    }
    return false;
}

bool isNumeric(const Value& v) {
    return v.type == ValueType::Int || v.type == ValueType::Double;
}

double numAsDouble(const Value& v) {
    return v.type == ValueType::Double ? v.doubleValue
                                       : static_cast<double>(v.intValue);
}

std::string valueAsString(const Value& v) {
    return v.type == ValueType::Text ? v.textValue : v.toString();
}

Value castValue(const Value& v, parser::DataType target) {
    if (v.isNull()) return Value::null();
    switch (target) {
        case parser::DataType::Int:
            if (v.type == ValueType::Int) return v;
            if (v.type == ValueType::Double)
                return Value::makeInt(static_cast<std::int64_t>(v.doubleValue));
            if (v.type == ValueType::Bool) return Value::makeInt(v.boolValue ? 1 : 0);
            if (v.type == ValueType::Text) {
                try { return Value::makeInt(std::stoll(v.textValue)); }
                catch (...) { return Value::null(); }
            }
            return Value::null();
        case parser::DataType::Float:
            if (isNumeric(v)) return Value::makeDouble(numAsDouble(v));
            if (v.type == ValueType::Text) {
                try { return Value::makeDouble(std::stod(v.textValue)); }
                catch (...) { return Value::null(); }
            }
            return Value::null();
        case parser::DataType::Bool:
            if (v.type == ValueType::Bool) return v;
            if (v.type == ValueType::Int) return Value::makeBool(v.intValue != 0);
            return Value::null();
        case parser::DataType::Text:
        case parser::DataType::Varchar:
        case parser::DataType::Date:
        case parser::DataType::Timestamp:
            return Value::makeText(valueAsString(v));
    }
    return Value::null();
}

Value evalCall(const parser::CallExpr& call, const std::vector<Value>& a) {
    const std::string& fn = call.name;
    if (call.isCast) return castValue(a[0], call.castType);

    if (fn == "COALESCE") {
        for (const Value& v : a)
            if (!v.isNull()) return v;
        return Value::null();
    }
    if (fn == "NULLIF") {
        if (a[0].isNull()) return Value::null();
        auto cmp = compareValues(a[0], a[1]);
        if (cmp.has_value() && *cmp == 0) return Value::null();
        return a[0];
    }
    if (fn == "UPPER" || fn == "LOWER") {
        if (a[0].isNull()) return Value::null();
        std::string s = valueAsString(a[0]);
        for (char& c : s) {
            c = static_cast<char>(fn == "UPPER"
                    ? std::toupper(static_cast<unsigned char>(c))
                    : std::tolower(static_cast<unsigned char>(c)));
        }
        return Value::makeText(s);
    }
    if (fn == "LENGTH") {
        if (a[0].isNull()) return Value::null();
        return Value::makeInt(static_cast<std::int64_t>(valueAsString(a[0]).size()));
    }
    if (fn == "TRIM") {
        if (a[0].isNull()) return Value::null();
        std::string s = valueAsString(a[0]);
        auto notSpace = [](unsigned char c) { return !std::isspace(c); };
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
        s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
        return Value::makeText(s);
    }
    if (fn == "SUBSTR") {
        if (a[0].isNull() || a[1].isNull()) return Value::null();
        std::string s = valueAsString(a[0]);
        long long start = a[1].intValue;
        if (start < 1) start = 1;
        std::size_t from = static_cast<std::size_t>(start - 1);
        if (from >= s.size()) return Value::makeText("");
        std::size_t len = s.size() - from;
        if (a.size() == 3 && !a[2].isNull()) {
            long long l = a[2].intValue;
            len = l < 0 ? 0 : static_cast<std::size_t>(l);
        }
        return Value::makeText(s.substr(from, len));
    }
    if (fn == "ABS") {
        if (a[0].isNull()) return Value::null();
        if (a[0].type == ValueType::Double)
            return Value::makeDouble(std::fabs(a[0].doubleValue));
        return Value::makeInt(a[0].intValue < 0 ? -a[0].intValue : a[0].intValue);
    }
    if (fn == "CEIL" || fn == "FLOOR") {
        if (a[0].isNull() || !isNumeric(a[0])) return Value::null();
        double d = numAsDouble(a[0]);
        double r = (fn == "CEIL") ? std::ceil(d) : std::floor(d);
        return Value::makeInt(static_cast<std::int64_t>(r));
    }
    if (fn == "ROUND") {
        if (a[0].isNull() || !isNumeric(a[0])) return Value::null();
        double d = numAsDouble(a[0]);
        int digits = (a.size() == 2 && !a[1].isNull())
                         ? static_cast<int>(a[1].intValue) : 0;
        double factor = std::pow(10.0, digits);
        return Value::makeDouble(std::round(d * factor) / factor);
    }
    if (fn == "MOD") {
        if (a[0].isNull() || a[1].isNull()) return Value::null();
        if (a[1].intValue == 0) return Value::null();
        return Value::makeInt(a[0].intValue % a[1].intValue);
    }
    return Value::null();
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
        if (col->outerRef) {
            return col->bound ? cachedToValue(col->boundValue) : Value::null();
        }
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

    if (auto* call = dynamic_cast<const CallExpr*>(&expr)) {
        std::vector<Value> args;
        args.reserve(call->args.size());
        for (const auto& arg : call->args) args.push_back(evalExpression(*arg, tuple));
        return evalCall(*call, args);
    }

    if (auto* cs = dynamic_cast<const CaseExpr*>(&expr)) {
        for (const auto& br : cs->branches) {
            Value cond = evalExpression(*br.when, tuple);
            if (cond.type == ValueType::Bool && cond.boolValue) {
                return evalExpression(*br.then, tuple);
            }
        }
        if (cs->elseExpr) return evalExpression(*cs->elseExpr, tuple);
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
