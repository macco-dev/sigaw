#include "launcher_env.h"

#include <cctype>

namespace sigaw::run {

namespace {

bool is_preload_separator(char ch) {
    return ch == ':' || std::isspace(static_cast<unsigned char>(ch));
}

} /* namespace */

bool preload_contains(std::string_view preload, std::string_view library_path) {
    size_t pos = 0;
    while (pos < preload.size()) {
        while (pos < preload.size() && is_preload_separator(preload[pos])) {
            ++pos;
        }
        if (pos >= preload.size()) {
            break;
        }

        const size_t start = pos;
        while (pos < preload.size() && !is_preload_separator(preload[pos])) {
            ++pos;
        }

        if (preload.substr(start, pos - start) == library_path) {
            return true;
        }
    }

    return false;
}

std::string prepend_preload(std::string_view preload, std::string_view library_path) {
    if (library_path.empty() || preload_contains(preload, library_path)) {
        return std::string(preload);
    }

    if (preload.empty()) {
        return std::string(library_path);
    }

    return std::string(library_path) + " " + std::string(preload);
}

} /* namespace sigaw::run */
