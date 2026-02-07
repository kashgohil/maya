#pragma once

#include <string>
#include <fstream>
#include <sstream>

namespace maya {

class FileSystem {
public:
    static std::string read_text(const std::string& path) {
        std::ifstream file(path);
        if (!file.is_open()) {
            return "";
        }
        std::stringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }
};

} // namespace maya
