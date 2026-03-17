#include <cstdint>
#include <iostream>

#include "daemon/json_utils.h"

int main() {
    const json user = {
        {"id", "42"},
        {"global_name", nullptr},
        {"username", "Alpha"},
        {"avatar", nullptr},
    };

    if (sigaw::json_utils::display_name(user) != "Alpha") {
        std::cerr << "display_name fallback failed\n";
        return 1;
    }

    const json voice_with_nick = {
        {"nick", "[RTB] Alpha"},
        {"user", user},
    };

    if (sigaw::json_utils::voice_display_name(voice_with_nick) != "[RTB] Alpha") {
        std::cerr << "voice_display_name nick priority failed\n";
        return 1;
    }

    const json user_with_global = {
        {"global_name", "Display Alpha"},
        {"username", "alpha_handle"},
    };

    if (sigaw::json_utils::voice_display_name(json::object(), &user_with_global) != "Display Alpha") {
        std::cerr << "voice_display_name global_name fallback failed\n";
        return 1;
    }

    if (sigaw::json_utils::string_or(user, "avatar", "missing") != "missing") {
        std::cerr << "string_or null fallback failed\n";
        return 1;
    }

    if (sigaw::json_utils::u64_or(user, "id") != static_cast<uint64_t>(42)) {
        std::cerr << "u64_or string parse failed\n";
        return 1;
    }

    const json flags = {
        {"self_mute", nullptr},
        {"self_deaf", false},
        {"mute", 1},
        {"deaf", "true"},
    };

    if (sigaw::json_utils::bool_or(flags, "self_mute", true) != true) {
        std::cerr << "bool_or null default failed\n";
        return 1;
    }

    if (sigaw::json_utils::bool_or(flags, "self_deaf", true) != false) {
        std::cerr << "bool_or boolean parse failed\n";
        return 1;
    }

    if (sigaw::json_utils::bool_or(flags, "mute", false) != true) {
        std::cerr << "bool_or integer parse failed\n";
        return 1;
    }

    if (sigaw::json_utils::bool_or(flags, "deaf", false) != true) {
        std::cerr << "bool_or string parse failed\n";
        return 1;
    }

    const json numeric = {
        {"id", 77},
        {"user_id", nullptr},
    };

    if (sigaw::json_utils::u64_or(numeric, "id") != static_cast<uint64_t>(77)) {
        std::cerr << "u64_or integer parse failed\n";
        return 1;
    }

    if (sigaw::json_utils::u64_or(numeric, "user_id", 9) != static_cast<uint64_t>(9)) {
        std::cerr << "u64_or null default failed\n";
        return 1;
    }

    return 0;
}
