#ifndef SIGAW_CHAT_RUNTIME_H
#define SIGAW_CHAT_RUNTIME_H

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "discord_ipc.h"
#include "json_utils.h"
#include "../common/protocol.h"

namespace sigaw::chat {

inline constexpr uint64_t hold_ms = 10000;
inline constexpr uint64_t fade_ms = 4000;
inline constexpr uint64_t total_lifetime_ms = hold_ms + fade_ms;
inline constexpr int repaint_interval_ms = 100;

inline uint64_t steady_clock_ms(std::chrono::steady_clock::time_point now) {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count()
    );
}

inline uint64_t steady_clock_now_ms() {
    return steady_clock_ms(std::chrono::steady_clock::now());
}

inline std::string collapse_whitespace(std::string_view input) {
    std::string out;
    out.reserve(input.size());

    bool pending_space = false;
    for (const unsigned char ch : input) {
        if (std::isspace(ch) != 0) {
            pending_space = !out.empty();
            continue;
        }
        if (pending_space) {
            out.push_back(' ');
            pending_space = false;
        }
        out.push_back(static_cast<char>(ch));
    }

    return out;
}

inline std::string message_author_name(const json& message, const std::string& def = "???") {
    if (const auto* member = json_utils::object_or_null(message, "member")) {
        const auto nick = json_utils::string_or(*member, "nick");
        if (!nick.empty()) {
            return nick;
        }
    }

    if (const auto* author = json_utils::object_or_null(message, "author")) {
        return json_utils::display_name(*author, def);
    }

    return def;
}

inline bool message_has_author_metadata(const json& message) {
    if (const auto* member = json_utils::object_or_null(message, "member")) {
        if (!json_utils::string_or(*member, "nick").empty()) {
            return true;
        }
    }
    return json_utils::object_or_null(message, "author") != nullptr;
}

inline std::string message_content(const json& message) {
    const auto content = collapse_whitespace(json_utils::string_or(message, "content"));
    if (!content.empty()) {
        return content;
    }

    const auto attachments = message.find("attachments");
    if (attachments != message.end() && attachments->is_array() && !attachments->empty()) {
        return "[attachment]";
    }

    return {};
}

inline bool parse_chat_message(const json& message,
                               uint64_t observed_at_ms,
                               VoiceChatMessage& out)
{
    const uint64_t message_id = json_utils::u64_or(message, "id");
    if (message_id == 0) {
        return false;
    }

    const auto normalized = message_content(message);
    if (normalized.empty()) {
        return false;
    }

    out = {};
    out.id = message_id;
    out.observed_at_ms = observed_at_ms;
    out.author_name = message_author_name(message);
    out.content = normalized;
    if (const auto* author = json_utils::object_or_null(message, "author")) {
        out.author_id = json_utils::u64_or(*author, "id");
    }
    return true;
}

inline VoiceChatMessage* find_chat_message(VoiceState& voice, uint64_t message_id) {
    const auto it = std::find_if(
        voice.chat_messages.begin(),
        voice.chat_messages.end(),
        [&](const VoiceChatMessage& message) { return message.id == message_id; }
    );
    return it == voice.chat_messages.end() ? nullptr : &*it;
}

inline bool upsert_chat_message(VoiceState& voice,
                                VoiceChatMessage message,
                                size_t max_messages = SIGAW_MAX_CHAT_MESSAGES)
{
    voice.chat_messages.erase(
        std::remove_if(
            voice.chat_messages.begin(),
            voice.chat_messages.end(),
            [&](const VoiceChatMessage& existing) { return existing.id == message.id; }
        ),
        voice.chat_messages.end()
    );

    voice.chat_messages.push_back(std::move(message));
    if (voice.chat_messages.size() > max_messages) {
        voice.chat_messages.erase(
            voice.chat_messages.begin(),
            voice.chat_messages.begin() +
                static_cast<std::ptrdiff_t>(voice.chat_messages.size() - max_messages)
        );
    }
    return true;
}

inline bool remove_chat_message(VoiceState& voice, uint64_t message_id) {
    const auto before = voice.chat_messages.size();
    voice.chat_messages.erase(
        std::remove_if(
            voice.chat_messages.begin(),
            voice.chat_messages.end(),
            [&](const VoiceChatMessage& message) { return message.id == message_id; }
        ),
        voice.chat_messages.end()
    );
    return before != voice.chat_messages.size();
}

inline bool load_chat_history(VoiceState& voice,
                              const json& messages,
                              uint64_t observed_at_ms,
                              size_t max_messages = SIGAW_MAX_CHAT_MESSAGES)
{
    voice.chat_messages.clear();
    if (!messages.is_array()) {
        return false;
    }

    std::vector<VoiceChatMessage> parsed;
    parsed.reserve(std::min(messages.size(), max_messages));
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        VoiceChatMessage message;
        if (!parse_chat_message(*it, observed_at_ms, message)) {
            continue;
        }
        parsed.push_back(std::move(message));
    }

    if (parsed.size() > max_messages) {
        parsed.erase(
            parsed.begin(),
            parsed.begin() + static_cast<std::ptrdiff_t>(parsed.size() - max_messages)
        );
    }

    voice.chat_messages = std::move(parsed);
    return !voice.chat_messages.empty();
}

inline const json* event_message_object(const json& event) {
    const auto* data = json_utils::object_or_null(event, "data");
    if (!data) {
        return nullptr;
    }

    if (const auto* message = json_utils::object_or_null(*data, "message")) {
        return message;
    }

    return data;
}

inline bool process_chat_event(const json& event,
                               VoiceState& voice,
                               uint64_t observed_at_ms,
                               size_t max_messages = SIGAW_MAX_CHAT_MESSAGES)
{
    if (json_utils::string_or(event, "cmd") != "DISPATCH") {
        return false;
    }

    const auto evt = json_utils::string_or(event, "evt");
    const auto* payload_message = event_message_object(event);
    if (!payload_message) {
        return false;
    }

    if (evt == "MESSAGE_DELETE") {
        return remove_chat_message(voice, json_utils::u64_or(*payload_message, "id"));
    }

    if (evt != "MESSAGE_CREATE" && evt != "MESSAGE_UPDATE") {
        return false;
    }

    if (evt == "MESSAGE_CREATE") {
        VoiceChatMessage message;
        if (!parse_chat_message(*payload_message, observed_at_ms, message)) {
            return false;
        }
        return upsert_chat_message(voice, std::move(message), max_messages);
    }

    const uint64_t message_id = json_utils::u64_or(*payload_message, "id");
    if (message_id == 0) {
        return false;
    }

    VoiceChatMessage updated;
    if (auto* existing = find_chat_message(voice, message_id)) {
        updated = *existing;
    } else {
        updated.id = message_id;
    }
    updated.observed_at_ms = observed_at_ms;

    if (message_has_author_metadata(*payload_message)) {
        updated.author_name = message_author_name(
            *payload_message, updated.author_name.empty() ? "???" : updated.author_name
        );
        if (const auto* author = json_utils::object_or_null(*payload_message, "author")) {
            updated.author_id = json_utils::u64_or(*author, "id", updated.author_id);
        }
    }

    if (payload_message->contains("content") || payload_message->contains("attachments")) {
        updated.content = message_content(*payload_message);
        if (updated.content.empty()) {
            return remove_chat_message(voice, message_id);
        }
    }

    if (updated.content.empty()) {
        VoiceChatMessage parsed;
        if (!parse_chat_message(*payload_message, observed_at_ms, parsed)) {
            return false;
        }
        updated = std::move(parsed);
    }

    return upsert_chat_message(voice, std::move(updated), max_messages);
}

inline bool prune_expired_messages(VoiceState& voice, uint64_t now_ms) {
    const auto before = voice.chat_messages.size();
    voice.chat_messages.erase(
        std::remove_if(
            voice.chat_messages.begin(),
            voice.chat_messages.end(),
            [&](const VoiceChatMessage& message) {
                const uint64_t age = now_ms > message.observed_at_ms
                    ? now_ms - message.observed_at_ms
                    : 0u;
                return age >= total_lifetime_ms;
            }
        ),
        voice.chat_messages.end()
    );
    return before != voice.chat_messages.size();
}

inline bool repaint_due(const VoiceState& voice, uint64_t now_ms) {
    return std::any_of(
        voice.chat_messages.begin(),
        voice.chat_messages.end(),
        [&](const VoiceChatMessage& message) {
            const uint64_t age = now_ms > message.observed_at_ms
                ? now_ms - message.observed_at_ms
                : 0u;
            return age >= hold_ms && age < total_lifetime_ms;
        }
    );
}

inline int next_timeout_ms(const VoiceState& voice,
                           uint64_t now_ms,
                           int default_timeout_ms = 250)
{
    int timeout_ms = default_timeout_ms;
    for (const auto& message : voice.chat_messages) {
        const uint64_t age = now_ms > message.observed_at_ms
            ? now_ms - message.observed_at_ms
            : 0u;
        if (age >= total_lifetime_ms) {
            return 0;
        }
        if (age < hold_ms) {
            timeout_ms = std::min<int>(
                timeout_ms,
                static_cast<int>(hold_ms - age)
            );
        } else {
            timeout_ms = std::min(timeout_ms, repaint_interval_ms);
        }
    }
    return timeout_ms;
}

} /* namespace sigaw::chat */

#endif /* SIGAW_CHAT_RUNTIME_H */
