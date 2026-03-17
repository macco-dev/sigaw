#ifndef SIGAW_JSON_UTILS_H
#define SIGAW_JSON_UTILS_H

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace sigaw::json_utils {

inline const json* find_field(const json& object, const char* key) {
    if (!object.is_object()) {
        return nullptr;
    }

    const auto it = object.find(key);
    if (it == object.end() || it->is_null()) {
        return nullptr;
    }

    return &*it;
}

inline const json* object_or_null(const json& object, const char* key) {
    const auto* field = find_field(object, key);
    if (!field || !field->is_object()) {
        return nullptr;
    }

    return field;
}

inline std::string string_value(const json& value, const std::string& def = {}) {
    if (value.is_null()) {
        return def;
    }

    if (value.is_string()) {
        return value.get<std::string>();
    }

    if (value.is_number_unsigned()) {
        return std::to_string(value.get<uint64_t>());
    }

    if (value.is_number_integer()) {
        return std::to_string(value.get<int64_t>());
    }

    return def;
}

inline std::string string_or(const json& object, const char* key, const std::string& def = {}) {
    const auto* field = find_field(object, key);
    return field ? string_value(*field, def) : def;
}

inline uint64_t u64_value(const json& value, uint64_t def = 0) {
    try {
        if (value.is_number_unsigned()) {
            return value.get<uint64_t>();
        }

        if (value.is_number_integer()) {
            const auto parsed = value.get<int64_t>();
            return parsed >= 0 ? static_cast<uint64_t>(parsed) : def;
        }

        if (value.is_string()) {
            const auto parsed = value.get<std::string>();
            return parsed.empty() ? def : std::stoull(parsed);
        }
    } catch (...) {
    }

    return def;
}

inline uint64_t u64_or(const json& object, const char* key, uint64_t def = 0) {
    const auto* field = find_field(object, key);
    return field ? u64_value(*field, def) : def;
}

inline bool bool_value(const json& value, bool def = false) {
    if (value.is_boolean()) {
        return value.get<bool>();
    }

    if (value.is_number_unsigned()) {
        return value.get<uint64_t>() != 0;
    }

    if (value.is_number_integer()) {
        return value.get<int64_t>() != 0;
    }

    if (value.is_string()) {
        auto parsed = value.get<std::string>();
        std::transform(parsed.begin(), parsed.end(), parsed.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (parsed == "true" || parsed == "1" || parsed == "yes" || parsed == "on") {
            return true;
        }
        if (parsed == "false" || parsed == "0" || parsed == "no" || parsed == "off") {
            return false;
        }
    }

    return def;
}

inline bool bool_or(const json& object, const char* key, bool def = false) {
    const auto* field = find_field(object, key);
    return field ? bool_value(*field, def) : def;
}

inline std::string account_name(const json& user, const std::string& def = "???") {
    auto name = string_or(user, "global_name");
    if (!name.empty()) {
        return name;
    }

    name = string_or(user, "username");
    return name.empty() ? def : name;
}

inline std::string voice_display_name(const json& voice_state,
                                      const json* user = nullptr,
                                      const std::string& def = "???") {
    auto name = string_or(voice_state, "nick");
    if (!name.empty()) {
        return name;
    }

    if (user) {
        return account_name(*user, def);
    }

    if (const auto* embedded_user = object_or_null(voice_state, "user")) {
        return account_name(*embedded_user, def);
    }

    return def;
}

inline std::string display_name(const json& user, const std::string& def = "???") {
    return account_name(user, def);
}

} /* namespace sigaw::json_utils */

#endif /* SIGAW_JSON_UTILS_H */
