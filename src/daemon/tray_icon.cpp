#include "tray_icon.h"

#include <cstdio>
#include <deque>
#include <filesystem>
#include <string>
#include <utility>

#include <gio/gio.h>
#include <gtk/gtk.h>

#if SIGAW_USE_AYATANA_APPINDICATOR
#include <libayatana-appindicator/app-indicator.h>
#else
#include <libappindicator/app-indicator.h>
#endif

namespace sigaw::tray {

namespace {

constexpr const char* kActionKey = "sigaw-action";

std::filesystem::path icon_theme_dir() {
    const std::string file_name = "sigaw-logo.svg";
    const std::filesystem::path installed_dir = std::filesystem::path(SIGAW_DATA_DIR) / "icons";
    if (std::filesystem::exists(installed_dir / file_name)) {
        return installed_dir;
    }

#ifdef SIGAW_SOURCE_DIR
    const auto source_dir = std::filesystem::path(SIGAW_SOURCE_DIR) / "assets";
    if (std::filesystem::exists(source_dir / file_name)) {
        return source_dir;
    }
#endif

    return installed_dir;
}

std::string connection_label(DiscordConnectionState state) {
    switch (state) {
        case DiscordConnectionState::Connected:
            return "Discord: connected";
        case DiscordConnectionState::AuthorizationPending:
            return "Discord: waiting for approval";
        case DiscordConnectionState::AuthorizationRequired:
            return "Discord: authorization required";
        case DiscordConnectionState::Disconnected:
        default:
            return "Discord: disconnected";
    }
}

std::string channel_label(const StatusSnapshot& snapshot) {
    if (snapshot.channel_name.empty()) {
        return "Channel: none";
    }

    const char* noun = snapshot.user_count == 1 ? "user" : "users";
    return "Channel: " + snapshot.channel_name + " (" +
           std::to_string(static_cast<unsigned long long>(snapshot.user_count)) +
           " " + noun + ")";
}

} // namespace

struct Icon::Impl {
    AppIndicator*      indicator = nullptr;
    GtkWidget*         menu = nullptr;
    GtkWidget*         connection_item = nullptr;
    GtkWidget*         channel_item = nullptr;
    GtkCheckMenuItem*  overlay_item = nullptr;
    GtkCheckMenuItem*  voice_messages_item = nullptr;
    GtkCheckMenuItem*  compact_item = nullptr;
    GtkWidget*         reauthenticate_item = nullptr;
    std::deque<Action> actions;
    std::string        icon_path;
    bool               syncing_menu = false;

    static void on_menu_signal(GtkWidget* widget, gpointer user_data) {
        auto* self = static_cast<Impl*>(user_data);
        if (!self || self->syncing_menu) {
            return;
        }

        const auto action = static_cast<Action>(
            GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), kActionKey))
        );
        self->actions.push_back(action);
    }

    GtkWidget* make_status_item(const char* label) {
        GtkWidget* item = gtk_menu_item_new_with_label(label);
        gtk_widget_set_sensitive(item, FALSE);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
        return item;
    }

    GtkWidget* make_menu_item(const char* label,
                              const char* signal_name,
                              Action action)
    {
        GtkWidget* item = gtk_menu_item_new_with_label(label);
        g_object_set_data(G_OBJECT(item), kActionKey, GINT_TO_POINTER(static_cast<int>(action)));
        g_signal_connect(item, signal_name, G_CALLBACK(on_menu_signal), this);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
        return item;
    }

    GtkCheckMenuItem* make_check_item(const char* label, Action action) {
        GtkWidget* item = gtk_check_menu_item_new_with_label(label);
        g_object_set_data(G_OBJECT(item), kActionKey, GINT_TO_POINTER(static_cast<int>(action)));
        g_signal_connect(item, "toggled", G_CALLBACK(on_menu_signal), this);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
        return GTK_CHECK_MENU_ITEM(item);
    }

    void set_checked(GtkCheckMenuItem* item, bool active) {
        if (!item) {
            return;
        }
        if (gtk_check_menu_item_get_active(item) != static_cast<gboolean>(active)) {
            gtk_check_menu_item_set_active(item, active ? TRUE : FALSE);
        }
    }

    void update(const StatusSnapshot& snapshot) {
        gtk_menu_item_set_label(GTK_MENU_ITEM(connection_item),
                                connection_label(snapshot.discord_state).c_str());
        gtk_menu_item_set_label(GTK_MENU_ITEM(channel_item),
                                channel_label(snapshot).c_str());

        syncing_menu = true;
        set_checked(overlay_item, snapshot.overlay_visible);
        set_checked(voice_messages_item, snapshot.show_voice_messages);
        set_checked(compact_item, snapshot.compact_mode);
        syncing_menu = false;
    }
};

Icon::Icon() = default;

Icon::~Icon() {
    close();
}

bool Icon::open() {
    if (impl_) {
        return true;
    }

    int gtk_argc = 0;
    char** gtk_argv = nullptr;
    if (!gtk_init_check(&gtk_argc, &gtk_argv)) {
        fprintf(stderr, "[sigaw] Tray unavailable: no graphical GTK session\n");
        return false;
    }

    GError* error = nullptr;
    GDBusConnection* session_bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (!session_bus) {
        fprintf(stderr, "[sigaw] Tray unavailable: session bus not reachable: %s\n",
                error ? error->message : "unknown error");
        if (error) {
            g_error_free(error);
        }
        return false;
    }
    g_object_unref(session_bus);

    auto impl = std::make_unique<Impl>();
    impl->icon_path = icon_theme_dir().string();

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
    impl->indicator = app_indicator_new(
        "sigaw-daemon",
        "sigaw-logo",
        APP_INDICATOR_CATEGORY_COMMUNICATIONS
    );
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
    if (!impl->indicator) {
        fprintf(stderr, "[sigaw] Tray unavailable: failed to create AppIndicator\n");
        return false;
    }

    app_indicator_set_title(impl->indicator, "Sigaw");
    if (!impl->icon_path.empty()) {
        app_indicator_set_icon_theme_path(impl->indicator, impl->icon_path.c_str());
    }
    app_indicator_set_icon_full(impl->indicator, "sigaw-logo", "Sigaw");

    impl->menu = gtk_menu_new();
    g_object_ref_sink(impl->menu);

    impl->connection_item = impl->make_status_item("Discord: disconnected");
    impl->channel_item = impl->make_status_item("Channel: none");

    gtk_menu_shell_append(GTK_MENU_SHELL(impl->menu), gtk_separator_menu_item_new());
    impl->overlay_item = impl->make_check_item("Show overlay", Action::ToggleOverlay);
    impl->voice_messages_item = impl->make_check_item(
        "Show voice messages",
        Action::ToggleVoiceMessages
    );
    impl->compact_item = impl->make_check_item("Compact mode", Action::ToggleCompactMode);

    gtk_menu_shell_append(GTK_MENU_SHELL(impl->menu), gtk_separator_menu_item_new());
    (void)impl->make_menu_item("Open config", "activate", Action::OpenConfig);
    (void)impl->make_menu_item("Reload config", "activate", Action::ReloadConfig);
    impl->reauthenticate_item = impl->make_menu_item(
        "Reauthenticate with Discord",
        "activate",
        Action::Reauthenticate
    );
    (void)impl->make_menu_item("Stop daemon", "activate", Action::StopDaemon);

    gtk_widget_show_all(impl->menu);
    app_indicator_set_menu(impl->indicator, GTK_MENU(impl->menu));
    app_indicator_set_status(impl->indicator, APP_INDICATOR_STATUS_ACTIVE);

    impl_ = std::move(impl);
    return true;
}

void Icon::close() {
    if (!impl_) {
        return;
    }

    if (impl_->indicator) {
        app_indicator_set_status(impl_->indicator, APP_INDICATOR_STATUS_PASSIVE);
        g_object_unref(impl_->indicator);
    }
    if (impl_->menu) {
        gtk_widget_destroy(impl_->menu);
        g_object_unref(impl_->menu);
    }

    impl_.reset();
}

bool Icon::available() const {
    return impl_ != nullptr;
}

void Icon::pump_events() {
    if (!impl_) {
        return;
    }

    while (g_main_context_iteration(nullptr, FALSE)) {
    }
}

void Icon::update(const StatusSnapshot& snapshot) {
    if (!impl_) {
        return;
    }
    impl_->update(snapshot);
}

bool Icon::pop_action(Action& action) {
    if (!impl_ || impl_->actions.empty()) {
        return false;
    }

    action = impl_->actions.front();
    impl_->actions.pop_front();
    return true;
}

} /* namespace sigaw::tray */
