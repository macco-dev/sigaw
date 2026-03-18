#include <iostream>

#include "run/launcher_env.h"

namespace {

bool test_prepends_missing_library() {
    const auto value = sigaw::run::prepend_preload("/usr/lib/libOther.so", "/opt/libSigawGL.so");
    if (value != "/opt/libSigawGL.so /usr/lib/libOther.so") {
        std::cerr << "launcher should prepend the Sigaw preload library\n";
        return false;
    }
    return true;
}

bool test_preserves_existing_entries_and_separators() {
    const auto value = sigaw::run::prepend_preload("/usr/lib/libA.so:/usr/lib/libB.so", "/opt/libSigawGL.so");
    if (!sigaw::run::preload_contains(value, "/usr/lib/libA.so") ||
        !sigaw::run::preload_contains(value, "/usr/lib/libB.so") ||
        !sigaw::run::preload_contains(value, "/opt/libSigawGL.so")) {
        std::cerr << "launcher should preserve existing preload entries\n";
        return false;
    }
    return true;
}

bool test_duplicate_library_is_not_inserted_twice() {
    const auto value = sigaw::run::prepend_preload("/opt/libSigawGL.so /usr/lib/libOther.so",
                                                   "/opt/libSigawGL.so");
    if (value != "/opt/libSigawGL.so /usr/lib/libOther.so") {
        std::cerr << "launcher should not duplicate the Sigaw preload path\n";
        return false;
    }
    return true;
}

} // namespace

int main() {
    if (!test_prepends_missing_library()) {
        return 1;
    }
    if (!test_preserves_existing_entries_and_separators()) {
        return 1;
    }
    if (!test_duplicate_library_is_not_inserted_twice()) {
        return 1;
    }
    return 0;
}
