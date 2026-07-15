#include "vm/value.hpp"

#include <cmath>
#include <sstream>

namespace db::vm {

namespace {

std::string formatDouble(double v) {
    if (std::isfinite(v) && v == std::floor(v) &&
        std::abs(v) < 1e15) {
        std::ostringstream os;
        os << static_cast<std::int64_t>(v) << ".0";
        return os.str();
    }
    std::ostringstream os;
    os << v;
    return os.str();
}

bool isNumeric(ValueType t) { return t == ValueType::Int || t == ValueType::Double; }

double asDouble(const Value& v) {
    return v.type == ValueType::Double ? v.doubleValue
                                       : static_cast<double>(v.intValue);
}

}

std::string Value::toString() const {
    switch (type) {
        case ValueType::Null: return "NULL";
        case ValueType::Int: return std::to_string(intValue);
        case ValueType::Bool: return boolValue ? "TRUE" : "FALSE";
        case ValueType::Text: return textValue;
        case ValueType::Double: return formatDouble(doubleValue);
    }
    return "NULL";
}

std::optional<int> compareValues(const Value& a, const Value& b) {
    if (a.isNull() || b.isNull()) {
        return std::nullopt;
    }
    if (isNumeric(a.type) && isNumeric(b.type)) {
        if (a.type == ValueType::Int && b.type == ValueType::Int) {
            if (a.intValue < b.intValue) return -1;
            if (a.intValue > b.intValue) return 1;
            return 0;
        }
        double da = asDouble(a);
        double db = asDouble(b);
        if (da < db) return -1;
        if (da > db) return 1;
        return 0;
    }
    switch (a.type) {
        case ValueType::Bool:
            if (b.type != ValueType::Bool) return std::nullopt;
            return static_cast<int>(a.boolValue) - static_cast<int>(b.boolValue);
        case ValueType::Text:
            if (b.type != ValueType::Text) return std::nullopt;
            return a.textValue.compare(b.textValue);
        default:
            return std::nullopt;
    }
}

bool valueLess(const Value& a, const Value& b) {
    if (a.isNull() || b.isNull()) {
        return a.isNull() && !b.isNull();
    }
    if (isNumeric(a.type) && isNumeric(b.type)) {
        auto cmp = compareValues(a, b);
        return cmp.has_value() && *cmp < 0;
    }
    if (a.type != b.type) {
        return static_cast<int>(a.type) < static_cast<int>(b.type);
    }
    auto cmp = compareValues(a, b);
    return cmp.has_value() && *cmp < 0;
}

}
