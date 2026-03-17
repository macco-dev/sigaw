#include <chrono>
#include <deque>
#include <iostream>
#include <string>
#include <vector>

#include "daemon/voice_runtime.h"

namespace {

struct FakeIpc {
    std::deque<sigaw::VoiceState> snapshots;
    std::vector<std::string> subscribed;
    std::vector<std::string> unsubscribed;
    sigaw::DiscordUserIdentity self{42, "Skynet", "avatarhash"};

    bool subscribe_channel(const std::string& channel_id) {
        subscribed.push_back(channel_id);
        return true;
    }

    bool unsubscribe_channel(const std::string& channel_id) {
        unsubscribed.push_back(channel_id);
        return true;
    }

    sigaw::VoiceState get_selected_voice_channel() {
        if (snapshots.empty()) {
            return {};
        }

        auto current = snapshots.front();
        snapshots.pop_front();
        return current;
    }

    const sigaw::DiscordUserIdentity& local_user() const {
        return self;
    }
};

sigaw::VoiceState make_voice(const std::string& channel_id,
                             const std::string& channel_name,
                             std::initializer_list<uint64_t> users)
{
    sigaw::VoiceState state;
    state.channel_id = channel_id;
    state.channel_name = channel_name;
    for (const auto user_id : users) {
        sigaw::VoiceUser user;
        user.id = user_id;
        user.username = "user-" + std::to_string(static_cast<unsigned long long>(user_id));
        state.users.push_back(std::move(user));
    }
    return state;
}

bool test_inbox_queues_dispatch_until_matching_response() {
    sigaw::DiscordMessageInbox inbox;
    json matched;

    const json dispatch = {
        {"cmd", "DISPATCH"},
        {"evt", "VOICE_CHANNEL_SELECT"},
        {"data", {
            {"channel_id", "general"},
            {"user", {
                {"id", "42"},
                {"username", "Skynet"},
                {"avatar", "avatarhash"},
            }},
        }},
    };

    if (inbox.store_or_match(dispatch, "s-1", "SUBSCRIBE", matched)) {
        std::cerr << "dispatch should not satisfy subscribe response\n";
        return false;
    }

    const json ack = {
        {"cmd", "SUBSCRIBE"},
        {"evt", nullptr},
        {"nonce", "s-1"},
        {"data", json::object()},
    };

    if (!inbox.store_or_match(ack, "s-1", "SUBSCRIBE", matched)) {
        std::cerr << "subscribe response was not matched after queued dispatch\n";
        return false;
    }

    if (sigaw::json_utils::string_or(matched, "cmd") != "SUBSCRIBE") {
        std::cerr << "matched response command mismatch\n";
        return false;
    }

    json queued;
    if (!inbox.take_dispatch(queued) ||
        sigaw::json_utils::string_or(queued, "evt") != "VOICE_CHANNEL_SELECT") {
        std::cerr << "queued dispatch was lost while waiting for response\n";
        return false;
    }

    if (inbox.local_user().id != 42 || inbox.local_user().username != "Skynet") {
        std::cerr << "local user identity was not captured from queued dispatch\n";
        return false;
    }

    return true;
}

bool test_rejoin_retries_until_populated_snapshot() {
    FakeIpc ipc;
    ipc.snapshots.push_back(make_voice("new", "General", {}));
    ipc.snapshots.push_back(make_voice("new", "General", {42, 77}));

    sigaw::VoiceRuntimeState state;
    state.voice = make_voice("old", "Lobby", {12});
    state.subscribed_channel_id = "old";

    const sigaw::VoiceSyncConfig config{};
    const auto now = std::chrono::steady_clock::time_point{};
    const json event = {
        {"cmd", "DISPATCH"},
        {"evt", "VOICE_CHANNEL_SELECT"},
        {"data", {{"channel_id", "new"}}},
    };

    const auto begin = sigaw::process_voice_event(event, state, ipc, now, config);
    if (!begin.state_changed || !state.voice.channel_id.empty()) {
        std::cerr << "join event should clear published voice state before resync\n";
        return false;
    }

    if (ipc.unsubscribed != std::vector<std::string>{"old"} ||
        ipc.subscribed != std::vector<std::string>{"new"}) {
        std::cerr << "channel subscription did not switch during rejoin\n";
        return false;
    }

    const auto first_sync = sigaw::sync_pending_channel(state, ipc, now, config);
    if (first_sync.state_changed || !state.pending_sync.active()) {
        std::cerr << "empty first snapshot should keep retry pending\n";
        return false;
    }

    const auto second_sync = sigaw::sync_pending_channel(
        state, ipc, now + std::chrono::milliseconds(config.retry_interval_ms), config
    );
    if (!second_sync.state_changed || !second_sync.joined_channel) {
        std::cerr << "populated retry should publish joined voice state\n";
        return false;
    }

    if (state.voice.channel_id != "new" || state.voice.users.size() != 2) {
        std::cerr << "rejoin sync did not publish the populated member list\n";
        return false;
    }

    return true;
}

bool test_solo_join_falls_back_to_local_user() {
    FakeIpc ipc;
    for (int i = 0; i < 5; ++i) {
        ipc.snapshots.push_back(make_voice("solo", "General", {}));
    }

    sigaw::VoiceRuntimeState state;
    const sigaw::VoiceSyncConfig config{};
    const auto now = std::chrono::steady_clock::time_point{};
    const json event = {
        {"cmd", "DISPATCH"},
        {"evt", "VOICE_CHANNEL_SELECT"},
        {"data", {{"channel_id", "solo"}}},
    };

    sigaw::process_voice_event(event, state, ipc, now, config);

    sigaw::VoiceUpdateResult sync_result;
    for (int attempt = 0; attempt < config.max_attempts; ++attempt) {
        sync_result = sigaw::sync_pending_channel(
            state, ipc, now + std::chrono::milliseconds(attempt * config.retry_interval_ms), config
        );
    }

    if (!sync_result.state_changed || !sync_result.joined_channel) {
        std::cerr << "solo join should resolve after bounded retries\n";
        return false;
    }

    if (state.voice.channel_id != "solo" || state.voice.users.size() != 1) {
        std::cerr << "solo join should publish exactly one local user\n";
        return false;
    }

    if (state.voice.users.front().id != ipc.self.id ||
        state.voice.users.front().username != ipc.self.username) {
        std::cerr << "solo fallback did not use the cached local identity\n";
        return false;
    }

    return true;
}

bool test_voice_state_update_reads_nested_voice_state_flags() {
    FakeIpc ipc;
    sigaw::VoiceRuntimeState state;
    state.voice = make_voice("room", "Room", {42});

    const sigaw::VoiceSyncConfig config{};
    const auto now = std::chrono::steady_clock::time_point{};
    const json event = {
        {"cmd", "DISPATCH"},
        {"evt", "VOICE_STATE_UPDATE"},
        {"data", {
            {"user", {
                {"id", "42"},
                {"username", "Skynet"},
            }},
            {"voice_state", {
                {"self_mute", true},
                {"self_deaf", true},
                {"mute", false},
                {"deaf", false},
            }},
        }},
    };

    const auto update = sigaw::process_voice_event(event, state, ipc, now, config);
    if (!update.state_changed) {
        std::cerr << "nested voice_state update should mark state as changed\n";
        return false;
    }

    const auto& user = state.voice.users.front();
    if (!user.self_mute || !user.self_deaf) {
        std::cerr << "nested voice_state flags were not applied live\n";
        return false;
    }

    return true;
}

} // namespace

int main() {
    if (!test_inbox_queues_dispatch_until_matching_response()) {
        return 1;
    }

    if (!test_rejoin_retries_until_populated_snapshot()) {
        return 1;
    }

    if (!test_solo_join_falls_back_to_local_user()) {
        return 1;
    }

    if (!test_voice_state_update_reads_nested_voice_state_flags()) {
        return 1;
    }

    return 0;
}
