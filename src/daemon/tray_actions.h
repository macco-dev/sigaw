#ifndef SIGAW_TRAY_ACTIONS_H
#define SIGAW_TRAY_ACTIONS_H

#include "../common/config.h"

#include <optional>

namespace sigaw::tray {

enum class Action {
    ToggleOverlay,
    ToggleVoiceMessages,
    ToggleCompactMode,
    OpenConfig,
    ReloadConfig,
    StopDaemon,
};

struct PlannedAction {
    std::optional<Config> updated_config;
    bool overlay_dirty = false;
    bool clear_chat_state = false;
    bool refresh_chat_state = false;
    bool reconnect_requested = false;
    bool reload_requested = false;
    bool open_config_requested = false;
    bool stop_daemon = false;
};

PlannedAction plan_action(Action action,
                          const Config& current,
                          bool have_messages_read_scope);

} /* namespace sigaw::tray */

#endif /* SIGAW_TRAY_ACTIONS_H */
