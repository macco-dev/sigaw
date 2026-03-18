#ifndef SIGAW_TRAY_ICON_H
#define SIGAW_TRAY_ICON_H

#include "tray_actions.h"

#include <cstddef>
#include <memory>
#include <string>

namespace sigaw::tray {

struct StatusSnapshot {
    bool        discord_connected = false;
    std::string channel_name;
    std::size_t user_count = 0;
    bool        overlay_visible = true;
    bool        show_voice_messages = false;
    bool        compact_mode = false;
};

class Icon {
public:
    Icon();
    ~Icon();

    Icon(const Icon&) = delete;
    Icon& operator=(const Icon&) = delete;

    bool open();
    void close();
    bool available() const;
    void pump_events();
    void update(const StatusSnapshot& snapshot);
    bool pop_action(Action& action);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} /* namespace sigaw::tray */

#endif /* SIGAW_TRAY_ICON_H */
