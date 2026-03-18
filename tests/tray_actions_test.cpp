#include <iostream>

#include "daemon/tray_actions.h"

namespace {

bool test_toggle_overlay_requests_persist_and_refresh() {
    sigaw::Config cfg;
    cfg.visible = true;

    const auto planned = sigaw::tray::plan_action(
        sigaw::tray::Action::ToggleOverlay,
        cfg,
        false
    );

    if (!planned.updated_config || planned.updated_config->visible) {
        std::cerr << "toggle overlay should invert visible and request persistence\n";
        return false;
    }
    if (!planned.overlay_dirty || planned.reconnect_requested ||
        planned.clear_chat_state || planned.refresh_chat_state) {
        std::cerr << "toggle overlay should only request an overlay refresh\n";
        return false;
    }

    return true;
}

bool test_toggle_compact_requests_persist_and_refresh() {
    sigaw::Config cfg;
    cfg.compact = false;

    const auto planned = sigaw::tray::plan_action(
        sigaw::tray::Action::ToggleCompactMode,
        cfg,
        true
    );

    if (!planned.updated_config || !planned.updated_config->compact) {
        std::cerr << "toggle compact should enable compact mode in the planned config\n";
        return false;
    }
    if (!planned.overlay_dirty || planned.reconnect_requested) {
        std::cerr << "toggle compact should only request an overlay refresh\n";
        return false;
    }

    return true;
}

bool test_disable_voice_messages_clears_chat_immediately() {
    sigaw::Config cfg;
    cfg.show_voice_channel_chat = true;

    const auto planned = sigaw::tray::plan_action(
        sigaw::tray::Action::ToggleVoiceMessages,
        cfg,
        true
    );

    if (!planned.updated_config || planned.updated_config->show_voice_channel_chat) {
        std::cerr << "toggle voice messages should disable the flag in the planned config\n";
        return false;
    }
    if (!planned.overlay_dirty || !planned.clear_chat_state || planned.refresh_chat_state ||
        planned.reconnect_requested) {
        std::cerr << "disabling voice messages should clear chat state without reconnecting\n";
        return false;
    }

    return true;
}

bool test_enable_voice_messages_with_scope_refreshes_without_reconnect() {
    sigaw::Config cfg;
    cfg.show_voice_channel_chat = false;

    const auto planned = sigaw::tray::plan_action(
        sigaw::tray::Action::ToggleVoiceMessages,
        cfg,
        true
    );

    if (!planned.updated_config || !planned.updated_config->show_voice_channel_chat) {
        std::cerr << "toggle voice messages should enable the flag in the planned config\n";
        return false;
    }
    if (!planned.overlay_dirty || !planned.refresh_chat_state ||
        planned.clear_chat_state || planned.reconnect_requested) {
        std::cerr << "enabling voice messages with scope should refresh chat without reconnect\n";
        return false;
    }

    return true;
}

bool test_enable_voice_messages_without_scope_requests_reconnect() {
    sigaw::Config cfg;
    cfg.show_voice_channel_chat = false;

    const auto planned = sigaw::tray::plan_action(
        sigaw::tray::Action::ToggleVoiceMessages,
        cfg,
        false
    );

    if (!planned.updated_config || !planned.updated_config->show_voice_channel_chat) {
        std::cerr << "voice message toggle should still plan a persisted config change\n";
        return false;
    }
    if (!planned.overlay_dirty || !planned.reconnect_requested ||
        planned.refresh_chat_state || planned.clear_chat_state) {
        std::cerr << "enabling voice messages without scope should request reconnect\n";
        return false;
    }

    return true;
}

bool test_reload_and_stop_actions_map_to_daemon_controls() {
    sigaw::Config cfg;

    const auto reload = sigaw::tray::plan_action(
        sigaw::tray::Action::ReloadConfig,
        cfg,
        true
    );
    if (!reload.reload_requested || reload.updated_config || reload.stop_daemon) {
        std::cerr << "reload action should request daemon reload without config mutation\n";
        return false;
    }

    const auto stop = sigaw::tray::plan_action(
        sigaw::tray::Action::StopDaemon,
        cfg,
        true
    );
    if (!stop.stop_daemon || stop.updated_config || stop.reload_requested) {
        std::cerr << "stop action should map to daemon shutdown without config mutation\n";
        return false;
    }

    return true;
}

} // namespace

int main() {
    if (!test_toggle_overlay_requests_persist_and_refresh()) {
        return 1;
    }
    if (!test_toggle_compact_requests_persist_and_refresh()) {
        return 1;
    }
    if (!test_disable_voice_messages_clears_chat_immediately()) {
        return 1;
    }
    if (!test_enable_voice_messages_with_scope_refreshes_without_reconnect()) {
        return 1;
    }
    if (!test_enable_voice_messages_without_scope_requests_reconnect()) {
        return 1;
    }
    if (!test_reload_and_stop_actions_map_to_daemon_controls()) {
        return 1;
    }

    return 0;
}
