#pragma once

#include <string>
#include <vector>

#include "frontend/ast.hpp"
#include "vm/value.hpp"

namespace db::vm {

using Schema = std::vector<parser::DataType>;

class Tuple {
public:
    Tuple() = default;
    explicit Tuple(std::vector<Value> values) : values_(std::move(values)) {}

    const std::vector<Value>& values() const { return values_; }
    std::vector<Value>& values() { return values_; }
    const Value& at(std::size_t i) const { return values_[i]; }
    std::size_t size() const { return values_.size(); }

    std::string serialize(const Schema& schema) const;
    static Tuple deserialize(const std::string& bytes, const Schema& schema);

private:
    std::vector<Value> values_;
};

}
