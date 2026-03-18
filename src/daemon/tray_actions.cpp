#include "tray_actions.h"

namespace sigaw::tray {

PlannedAction plan_action(Action action,
                          const Config& current,
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

            if (next.show_voice_channel_chat) {
                planned.refresh_chat_state = have_messages_read_scope;
                planned.reconnect_requested = !have_messages_read_scope;
            } else {
                planned.clear_chat_state = true;
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

        case Action::StopDaemon:
            planned.stop_daemon = true;
            break;
    }

    return planned;
}

} /* namespace sigaw::tray */
