#pragma once

#include <string>

namespace db::backend {

bool syncFileToDisk(const std::string& path);

}
