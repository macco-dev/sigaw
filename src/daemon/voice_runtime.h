#ifndef SIGAW_VOICE_RUNTIME_H
#define SIGAW_VOICE_RUNTIME_H

#include <algorithm>
#include <chrono>
#include <string>

#include "discord_ipc.h"
#include "json_utils.h"

namespace sigaw {

struct VoiceSyncConfig {
    int retry_interval_ms = 100;
    int max_attempts = 5;
};

struct PendingVoiceSync {
    std::string channel_id;
    int attempts_remaining = 0;
    std::chrono::steady_clock::time_point next_attempt{};
    bool announce_join = false;

    bool active() const {
        return !channel_id.empty() && attempts_remaining > 0;
    }

    void clear() {
        channel_id.clear();
        attempts_remaining = 0;
        next_attempt = {};
        announce_join = false;
    }
};

struct VoiceRuntimeState {
    VoiceState voice;
    std::string subscribed_channel_id;
    std::string active_chat_channel_id;
    std::string subscribed_chat_channel_id;
    bool live_chat_events = false;
    PendingVoiceSync pending_sync;
};

struct VoiceUpdateResult {
    bool state_changed = false;
    bool left_channel = false;
    bool joined_channel = false;
    std::string user_joined;
    std::string user_left;
};

inline void update_voice_identity(VoiceUser& voice_user,
                                  const json& voice_state,
                                  const json* user)
{
    voice_user.username = json_utils::voice_display_name(
        voice_state, user, voice_user.username.empty() ? "???" : voice_user.username
    );

    if (!user) {
        return;
    }

    if (user->contains("avatar")) {
        voice_user.avatar = json_utils::string_or(*user, "avatar");
    }
}

inline const json& voice_flags_source(const json& voice_state) {
    const auto* nested = json_utils::object_or_null(voice_state, "voice_state");
    return nested ? *nested : voice_state;
}

inline void apply_voice_flags(VoiceUser& user, const json& voice_state) {
    const auto& flags = voice_flags_source(voice_state);
    user.self_mute = json_utils::bool_or(flags, "self_mute", user.self_mute);
    user.self_deaf = json_utils::bool_or(flags, "self_deaf", user.self_deaf);
    user.server_mute = json_utils::bool_or(flags, "mute", user.server_mute);
    user.server_deaf = json_utils::bool_or(flags, "deaf", user.server_deaf);
}

template <typename Ipc>
inline void add_local_user_fallback(VoiceState& voice, const Ipc& ipc) {
    if (!voice.users.empty()) {
        return;
    }

    const auto& self = ipc.local_user();
    if (self.empty()) {
        return;
    }

    VoiceUser user;
    user.id = self.id;
    user.username = self.username.empty() ? "???" : self.username;
    user.avatar = self.avatar;
    voice.users.push_back(std::move(user));
}

template <typename Ipc>
inline void schedule_channel_sync(VoiceRuntimeState& state,
                                  Ipc& ipc,
                                  const std::string& channel_id,
                                  std::chrono::steady_clock::time_point now,
                                  const VoiceSyncConfig& config,
                                  bool announce_join)
{
    if (channel_id.empty()) {
        return;
    }

    if (!state.subscribed_channel_id.empty() && state.subscribed_channel_id != channel_id) {
        ipc.unsubscribe_channel(state.subscribed_channel_id);
        state.subscribed_channel_id.clear();
    }

    if (state.subscribed_channel_id != channel_id) {
        ipc.subscribe_channel(channel_id);
        state.subscribed_channel_id = channel_id;
    }

    state.voice = {};
    state.pending_sync.channel_id = channel_id;
    state.pending_sync.attempts_remaining = std::max(1, config.max_attempts);
    state.pending_sync.next_attempt = now;
    state.pending_sync.announce_join = announce_join;
}

template <typename Ipc>
inline VoiceUpdateResult sync_pending_channel(VoiceRuntimeState& state,
                                              Ipc& ipc,
                                              std::chrono::steady_clock::time_point now,
                                              const VoiceSyncConfig& config)
{
    VoiceUpdateResult result;
    if (!state.pending_sync.active() || now < state.pending_sync.next_attempt) {
        return result;
    }

    VoiceState selected = ipc.get_selected_voice_channel();
    if (selected.channel_id.empty()) {
        if (!state.subscribed_channel_id.empty()) {
            ipc.unsubscribe_channel(state.subscribed_channel_id);
            state.subscribed_channel_id.clear();
        }
        state.pending_sync.clear();
        return result;
    }

    if (selected.channel_id != state.pending_sync.channel_id) {
        schedule_channel_sync(state, ipc, selected.channel_id, now, config,
                              state.pending_sync.announce_join);
        selected = ipc.get_selected_voice_channel();
        if (selected.channel_id.empty()) {
            return result;
        }
    }

    if (selected.channel_id == state.pending_sync.channel_id && !selected.users.empty()) {
        result.joined_channel = state.pending_sync.announce_join;
        state.pending_sync.clear();
        state.voice = std::move(selected);
        result.state_changed = true;
        return result;
    }

    state.pending_sync.attempts_remaining--;
    if (state.pending_sync.attempts_remaining > 0) {
        state.pending_sync.next_attempt = now + std::chrono::milliseconds(config.retry_interval_ms);
        return result;
    }

    add_local_user_fallback(selected, ipc);
    result.joined_channel = state.pending_sync.announce_join;
    state.pending_sync.clear();
    state.voice = std::move(selected);
    result.state_changed = true;
    return result;
}

template <typename Ipc>
inline VoiceUpdateResult process_voice_event(const json& event,
                                             VoiceRuntimeState& state,
                                             Ipc& ipc,
                                             std::chrono::steady_clock::time_point now,
                                             const VoiceSyncConfig& config)
{
    VoiceUpdateResult result;
    const std::string cmd = json_utils::string_or(event, "cmd");
    const std::string evt = json_utils::string_or(event, "evt");

    if (cmd != "DISPATCH") {
        return result;
    }

    if (evt == "VOICE_CHANNEL_SELECT") {
        const auto& data = event["data"];
        if (data.is_null() || json_utils::string_or(data, "channel_id").empty()) {
            if (!state.subscribed_channel_id.empty()) {
                ipc.unsubscribe_channel(state.subscribed_channel_id);
                state.subscribed_channel_id.clear();
            }
            state.pending_sync.clear();
            const bool had_voice = !state.voice.channel_id.empty();
            state.voice = {};
            result.state_changed = had_voice;
            result.left_channel = had_voice;
            return result;
        }

        const std::string new_channel = json_utils::string_or(data, "channel_id");
        if (new_channel == state.subscribed_channel_id &&
            state.pending_sync.channel_id.empty() &&
            state.voice.channel_id == new_channel &&
            !state.voice.users.empty()) {
            return result;
        }

        schedule_channel_sync(state, ipc, new_channel, now, config, true);
        result.state_changed = true;
        return result;
    }

    if (state.voice.channel_id.empty()) {
        return result;
    }

    if (evt == "VOICE_STATE_CREATE") {
        const auto& data = event["data"];
        const auto* user_data = json_utils::object_or_null(data, "user");
        if (!user_data) {
            return result;
        }

        VoiceUser user;
        user.id = json_utils::u64_or(*user_data, "id");
        if (user.id == 0) {
            return result;
        }

        auto it = std::find_if(state.voice.users.begin(), state.voice.users.end(),
            [&](const VoiceUser& existing) { return existing.id == user.id; });
        if (it != state.voice.users.end()) {
            return result;
        }

        update_voice_identity(user, data, user_data);
        apply_voice_flags(user, data);
        result.user_joined = user.username;
        state.voice.users.push_back(std::move(user));
        result.state_changed = true;
        return result;
    }

    if (evt == "VOICE_STATE_UPDATE") {
        const auto& data = event["data"];
        const auto* user_data = json_utils::object_or_null(data, "user");
        if (!user_data) {
            return result;
        }

        const uint64_t uid = json_utils::u64_or(*user_data, "id");
        if (uid == 0) {
            return result;
        }

        auto it = std::find_if(state.voice.users.begin(), state.voice.users.end(),
            [&](const VoiceUser& existing) { return existing.id == uid; });
        if (it == state.voice.users.end()) {
            VoiceUser user;
            user.id = uid;
            update_voice_identity(user, data, user_data);
            apply_voice_flags(user, data);
            result.user_joined = user.username;
            state.voice.users.push_back(std::move(user));
            result.state_changed = true;
            return result;
        }

        update_voice_identity(*it, data, user_data);
        apply_voice_flags(*it, data);
        result.state_changed = true;
        return result;
    }

    if (evt == "VOICE_STATE_DELETE") {
        const auto& data = event["data"];
        const auto* user_data = json_utils::object_or_null(data, "user");
        if (!user_data) {
            return result;
        }

        const uint64_t uid = json_utils::u64_or(*user_data, "id");
        const auto it = std::find_if(state.voice.users.begin(), state.voice.users.end(),
            [&](const VoiceUser& existing) { return existing.id == uid; });
        if (it == state.voice.users.end()) {
            return result;
        }

        result.user_left = it->username;
        state.voice.users.erase(it);
        result.state_changed = true;
        return result;
    }

    if (evt == "SPEAKING_START" || evt == "SPEAKING_STOP") {
        const auto& data = event["data"];
        const uint64_t uid = json_utils::u64_or(data, "user_id");
        if (uid == 0) {
            return result;
        }

        const bool speaking = evt == "SPEAKING_START";
        for (auto& user : state.voice.users) {
            if (user.id == uid) {
                if (user.speaking != speaking) {
                    user.speaking = speaking;
                    result.state_changed = true;
                }
                break;
            }
        }
    }

    return result;
}

} /* namespace sigaw */

#endif /* SIGAW_VOICE_RUNTIME_H */
