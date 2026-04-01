#include "tray_actions.h"

namespace sigaw::tray {

namespace {

bool daemon_chat_requested(const Config& config, bool profile_chat_requested) {
    return config.requests_voice_channel_chat() || profile_chat_requested;
}

} // namespace

PlannedAction plan_action(Action action,
                          const Config& current,
                          bool profile_chat_requested,
                          bool have_messages_read_scope)
{
    PlannedAction planned;

    switch (action) {
        case Action::ToggleOverlay: {
            auto next = current;
            next.visible = !next.visible;
            planned.updated_config = next;
            planned.overlay_dirty = true;
            break;
        }

        case Action::ToggleVoiceMessages: {
            auto next = current;
            next.show_voice_channel_chat = !next.show_voice_channel_chat;
            planned.updated_config = next;
            planned.overlay_dirty = true;
            const bool current_chat_requested = daemon_chat_requested(current, profile_chat_requested);
            const bool next_chat_requested = daemon_chat_requested(next, profile_chat_requested);

            if (!next_chat_requested) {
                planned.clear_chat_state = true;
            } else if (!current_chat_requested && have_messages_read_scope) {
                planned.refresh_chat_state = true;
            } else if (!current_chat_requested) {
                planned.reconnect_requested = true;
            }
            break;
        }

        case Action::ToggleCompactMode: {
            auto next = current;
            next.compact = !next.compact;
            planned.updated_config = next;
            planned.overlay_dirty = true;
            break;
        }

        case Action::OpenConfig:
            planned.open_config_requested = true;
            break;

        case Action::ReloadConfig:
            planned.reload_requested = true;
            break;

        case Action::Reauthenticate:
            planned.reconnect_requested = true;
            planned.reauthenticate_requested = true;
            break;

        case Action::StopDaemon:
            planned.stop_daemon = true;
            break;
    }

    return planned;
}

} /* namespace sigaw::tray */
