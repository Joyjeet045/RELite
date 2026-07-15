#pragma once

#include "frontend/ast.hpp"
#include "vm/tuple.hpp"
#include "vm/value.hpp"

namespace db::vm {

Value evalExpression(const parser::Expression& expr, const Tuple& tuple);

bool predicateTrue(const parser::Expression& expr, const Tuple& tuple);

}
