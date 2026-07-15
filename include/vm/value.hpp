#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace db::vm {

enum class ValueType { Null, Int, Bool, Text, Double };

struct Value {
    ValueType type = ValueType::Null;
    std::int64_t intValue = 0;
    bool boolValue = false;
    double doubleValue = 0.0;
    std::string textValue;

    static Value null() { return Value{}; }
    static Value makeInt(std::int64_t v) {
        Value x;
        x.type = ValueType::Int;
        x.intValue = v;
        return x;
    }
    static Value makeBool(bool v) {
        Value x;
        x.type = ValueType::Bool;
        x.boolValue = v;
        return x;
    }
    static Value makeText(std::string v) {
        Value x;
        x.type = ValueType::Text;
        x.textValue = std::move(v);
        return x;
    }
    static Value makeDouble(double v) {
        Value x;
        x.type = ValueType::Double;
        x.doubleValue = v;
        return x;
    }

    bool isNull() const { return type == ValueType::Null; }
    std::string toString() const;
};

std::optional<int> compareValues(const Value& a, const Value& b);

bool valueLess(const Value& a, const Value& b);

}
