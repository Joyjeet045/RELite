#pragma once

#include <string>
#include <vector>

#include "vm/value.hpp"

namespace db::vm {

struct ResultSet {
    bool isQuery = false;
    std::vector<std::string> columns;
    std::vector<std::vector<Value>> rows;
    std::string message;
};

}
