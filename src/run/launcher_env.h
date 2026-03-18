#ifndef SIGAW_LAUNCHER_ENV_H
#define SIGAW_LAUNCHER_ENV_H

#include <string>
#include <string_view>

namespace sigaw::run {

bool preload_contains(std::string_view preload, std::string_view library_path);
std::string prepend_preload(std::string_view preload, std::string_view library_path);

} /* namespace sigaw::run */

#endif /* SIGAW_LAUNCHER_ENV_H */
