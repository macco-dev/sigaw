#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <getopt.h>
#include <algorithm>
#include <chrono>
#include <poll.h>
#include <thread>

#include <glib.h>

#include "avatar_cache.h"
#include "chat_runtime.h"
#include "control_server.h"
#include "discord_ipc.h"
#include "json_utils.h"
#include "shm_writer.h"
#include "tray_actions.h"
#include "tray_icon.h"
#include "voice_runtime.h"
#include "../common/config.h"
#include "../common/config_watcher.h"
#include "../common/protocol.h"

static volatile sig_atomic_t g_running = 1;
static constexpr int kDiscordAuthorizeTimeoutMs = 60000;

static void signal_handler(int) {
    g_running = 0;
}

struct TrayLoopResult {
    bool state_changed = false;
    bool reconnect_requested = false;
    bool reauthenticate_requested = false;
    bool quit_requested = false;
};

static void update_tray(sigaw::tray::Icon* tray,
                        const sigaw::Config& config,
                        sigaw::tray::DiscordConnectionState discord_state,
                        const sigaw::VoiceState& voice);
static bool pump_pre_auth_controls(sigaw::ControlServer& ctl,
                                   sigaw::Config& config,
                                   bool* profile_chat_requested,
                                   sigaw::ConfigWatcher* config_watcher,
                                   sigaw::tray::Icon* tray = nullptr,
                                   bool* reconnect_requested = nullptr,
                                   bool* reauthenticate_requested = nullptr,
                                   sigaw::tray::DiscordConnectionState discord_state =
                                       sigaw::tray::DiscordConnectionState::Disconnected);
static TrayLoopResult process_tray_actions(sigaw::tray::Icon* tray,
                                           sigaw::Config& config,
                                           bool profile_chat_requested,
                                           bool have_messages_read_scope,
                                           sigaw::VoiceRuntimeState* runtime = nullptr,
                                           sigaw::DiscordIpc* ipc = nullptr);

static bool daemon_chat_requested(const sigaw::Config& config,
                                  bool profile_chat_requested)
{
    return config.requests_voice_channel_chat() || profile_chat_requested;
}

static bool daemon_requires_reconnect(const sigaw::Config& current,
                                      const sigaw::Config& updated,
                                      bool current_chat_requested,
                                      bool updated_chat_requested,
                                      bool have_messages_read_scope = false)
{
    return current.client_id != updated.client_id ||
           current.client_secret != updated.client_secret ||
           (!current_chat_requested &&
            updated_chat_requested &&
            !have_messages_read_scope);
}

static bool wait_with_control(sigaw::ControlServer& ctl, sigaw::Config& config,
                              bool* profile_chat_requested,
                              sigaw::ConfigWatcher* config_watcher,
                              int total_ms,
                              sigaw::tray::Icon* tray = nullptr,
                              bool* reconnect_requested = nullptr,
                              bool* reauthenticate_requested = nullptr,
                              sigaw::tray::DiscordConnectionState discord_state =
                                  sigaw::tray::DiscordConnectionState::Disconnected)
{
    for (int elapsed = 0; g_running && (total_ms < 0 || elapsed < total_ms); elapsed += 100) {
        if (!pump_pre_auth_controls(
                ctl,
                config,
                profile_chat_requested,
                config_watcher,
                tray,
                reconnect_requested,
                reauthenticate_requested,
                discord_state
            )) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return g_running != 0;
}

static void print_usage(const char* prog) {
    const auto shm_name = sigaw::Config::shared_memory_name();
    fprintf(stderr,
        "sigaw-daemon - Discord voice overlay daemon\n"
        "\n"
        "Usage: %s [OPTIONS]\n"
        "\n"
        "Options:\n"
        "  --config PATH    Path to config file (default: ~/.config/sigaw/sigaw.conf)\n"
        "  --init-config    Write default config file and exit\n"
        "  --foreground     Run in foreground (default)\n"
        "  --help           Show this help\n"
        "\n"
        "The daemon connects to Discord's local IPC socket and publishes\n"
        "voice channel state to shared memory (%s) for the\n"
        "Vulkan/OpenGL overlay hooks to read.\n"
        "\n"
        "Setup:\n"
        "  1. Run: sigaw-daemon --init-config\n"
        "  2. Run: sigaw-daemon --foreground\n"
        "  3. Approve the authorization in Discord (one-time)\n"
        "  4. Launch games with: sigaw-run <game>\n",
        prog, shm_name.c_str()
    );
}

/*
 * Update the shared memory state from the current voice channel.
 * Called after any voice event.
 */
static void update_shm(sigaw::ShmWriter& shm,
                        const sigaw::VoiceState& voice,
                        sigaw::AvatarCache& avatar_cache)
{
    shm.begin_write();

    if (voice.channel_id.empty()) {
        shm.clear_channel();
    } else {
        shm.set_channel(voice.channel_name.c_str());
        uint32_t count = std::min((uint32_t)voice.users.size(),
                                   (uint32_t)SIGAW_MAX_USERS);
        shm.set_user_count(count);

        for (uint32_t i = 0; i < count; i++) {
            auto& u = voice.users[i];
            avatar_cache.request(u.id, u.avatar);
            shm.set_user(i, u.id, u.username.c_str(), u.avatar.c_str(),
                         u.speaking, u.self_mute, u.self_deaf,
                         u.server_mute, u.server_deaf);
        }

        shm.clear_chat();
        count = std::min((uint32_t)voice.chat_messages.size(),
                         (uint32_t)SIGAW_MAX_CHAT_MESSAGES);
        shm.set_chat_count(count);
        for (uint32_t i = 0; i < count; ++i) {
            const auto& message = voice.chat_messages[i];
            shm.set_chat_message(
                i,
                message.id,
                message.author_id,
                message.observed_at_ms,
                message.author_name.c_str(),
                message.content.c_str()
            );
        }
    }

    shm.end_write();
}

static void unsubscribe_chat_events(sigaw::VoiceRuntimeState& runtime,
                                    sigaw::DiscordIpc& ipc)
{
    if (!runtime.subscribed_chat_channel_id.empty()) {
        (void)ipc.unsubscribe_channel_messages(runtime.subscribed_chat_channel_id);
        runtime.subscribed_chat_channel_id.clear();
    }
    runtime.live_chat_events = false;
}

static bool clear_chat_state(sigaw::VoiceRuntimeState& runtime,
                             sigaw::DiscordIpc* ipc = nullptr)
{
    const bool had_chat =
        !runtime.voice.chat_messages.empty() ||
        !runtime.active_chat_channel_id.empty() ||
        !runtime.subscribed_chat_channel_id.empty();

    if (ipc) {
        unsubscribe_chat_events(runtime, *ipc);
    } else {
        runtime.subscribed_chat_channel_id.clear();
        runtime.live_chat_events = false;
    }
    runtime.active_chat_channel_id.clear();
    runtime.voice.chat_messages.clear();
    return had_chat;
}

static bool refresh_chat_state(sigaw::VoiceRuntimeState& runtime,
                               sigaw::DiscordIpc& ipc,
                               bool want_chat)
{
    if (!want_chat || !ipc.has_scope("messages.read") || runtime.voice.channel_id.empty()) {
        return clear_chat_state(runtime, &ipc);
    }

    if (runtime.active_chat_channel_id == runtime.voice.channel_id) {
        return false;
    }

    const bool had_chat = clear_chat_state(runtime, &ipc);
    runtime.active_chat_channel_id = runtime.voice.channel_id;

    if (!ipc.subscribe_channel_messages(runtime.voice.channel_id)) {
        fprintf(stderr,
                "[sigaw] Voice channel chat live updates unavailable for %s\n",
                runtime.voice.channel_name.c_str());
        return had_chat;
    }

    runtime.subscribed_chat_channel_id = runtime.voice.channel_id;
    runtime.live_chat_events = true;
    return had_chat;
}

static bool open_config_in_editor(const sigaw::Config& config) {
    const auto path = sigaw::Config::config_path();
    if (!std::filesystem::exists(path) && !config.save()) {
        fprintf(stderr, "[sigaw] Failed to create config file before opening it\n");
        return false;
    }

    std::string path_string = path.string();
    char open_cmd[] = "xdg-open";
    char* argv[] = {open_cmd, path_string.data(), nullptr};
    GError* error = nullptr;
    if (!g_spawn_async(nullptr,
                       argv,
                       nullptr,
                       G_SPAWN_SEARCH_PATH,
                       nullptr,
                       nullptr,
                       nullptr,
                       &error)) {
        fprintf(stderr, "[sigaw] Failed to launch xdg-open for %s: %s\n",
                path_string.c_str(),
                error ? error->message : "unknown error");
        if (error) {
            g_error_free(error);
        }
        return false;
    }

    return true;
}

static void update_tray(sigaw::tray::Icon* tray,
                        const sigaw::Config& config,
                        sigaw::tray::DiscordConnectionState discord_state,
                        const sigaw::VoiceState& voice)
{
    if (!tray || !tray->available()) {
        return;
    }

    sigaw::tray::StatusSnapshot snapshot;
    snapshot.discord_state = discord_state;
    if (!voice.channel_id.empty()) {
        snapshot.channel_name = voice.channel_name;
        snapshot.user_count = voice.users.size();
    }
    snapshot.overlay_visible = config.visible;
    snapshot.show_voice_messages = config.show_voice_channel_chat;
    snapshot.compact_mode = config.compact;
    tray->update(snapshot);
}

static bool pump_pre_auth_controls(sigaw::ControlServer& ctl,
                                   sigaw::Config& config,
                                   bool* profile_chat_requested,
                                   sigaw::ConfigWatcher* config_watcher,
                                   sigaw::tray::Icon* tray,
                                   bool* reconnect_requested,
                                   bool* reauthenticate_requested,
                                   sigaw::tray::DiscordConnectionState discord_state)
{
    const auto tray_result = process_tray_actions(
        tray,
        config,
        profile_chat_requested ? *profile_chat_requested : false,
        false
    );
    if (tray_result.quit_requested) {
        g_running = 0;
        return false;
    }
    if (tray_result.reconnect_requested) {
        if (reconnect_requested) {
            *reconnect_requested = true;
        }
        if (tray_result.reauthenticate_requested && reauthenticate_requested) {
            *reauthenticate_requested = true;
        }
        return false;
    }

    if (config_watcher && config_watcher->consume_change()) {
        const auto current = config;
        const auto updated = sigaw::Config::load();
        const bool updated_profile_chat_requested =
            sigaw::Config::any_profile_requests_chat();
        const bool reconnect = daemon_requires_reconnect(
            current,
            updated,
            daemon_chat_requested(current, profile_chat_requested ? *profile_chat_requested : false),
            daemon_chat_requested(updated, updated_profile_chat_requested)
        );
        config = updated;
        if (profile_chat_requested) {
            *profile_chat_requested = updated_profile_chat_requested;
        }
        if (reconnect && reconnect_requested) {
            *reconnect_requested = true;
        }
        if (reconnect) {
            return false;
        }
    }

    const auto action = ctl.process_pending(config);
    if (action == sigaw::ControlServer::Action::Quit) {
        g_running = 0;
        return false;
    }
    if (action == sigaw::ControlServer::Action::Reload) {
        if (profile_chat_requested) {
            *profile_chat_requested = sigaw::Config::any_profile_requests_chat();
        }
        if (reconnect_requested) {
            *reconnect_requested = true;
        }
        return false;
    }

    const sigaw::VoiceState no_voice;
    update_tray(tray, config, discord_state, no_voice);
    return g_running != 0;
}

static TrayLoopResult process_tray_actions(sigaw::tray::Icon* tray,
                                           sigaw::Config& config,
                                           bool profile_chat_requested,
                                           bool have_messages_read_scope,
                                           sigaw::VoiceRuntimeState* runtime,
                                           sigaw::DiscordIpc* ipc)
{
    TrayLoopResult result;
    if (!tray || !tray->available()) {
        return result;
    }

    tray->pump_events();

    sigaw::tray::Action action;
    while (tray->pop_action(action)) {
        const auto planned = sigaw::tray::plan_action(
            action,
            config,
            profile_chat_requested,
            have_messages_read_scope
        );

        if (planned.updated_config) {
            auto next = *planned.updated_config;
            if (!next.save()) {
                fprintf(stderr, "[sigaw] Failed to persist config after tray action\n");
                continue;
            }
            config = std::move(next);
        }

        if (planned.open_config_requested) {
            (void)open_config_in_editor(config);
        }
        if (planned.clear_chat_state && runtime) {
            if (clear_chat_state(*runtime, ipc)) {
                result.state_changed = true;
            }
        }
        if (planned.refresh_chat_state && runtime && ipc) {
            const bool chat_changed = refresh_chat_state(
                *runtime,
                *ipc,
                daemon_chat_requested(config, profile_chat_requested)
            );
            result.state_changed = result.state_changed || chat_changed;
        }
        if (planned.reauthenticate_requested) {
            result.reauthenticate_requested = true;
        }
        if (planned.reload_requested || planned.reconnect_requested) {
            result.reconnect_requested = true;
            break;
        }
        if (planned.stop_daemon) {
            result.quit_requested = true;
            break;
        }
    }

    return result;
}

int main(int argc, char** argv) {
    bool foreground = true;
    bool init_config = false;
    std::string config_path;

    static struct option long_opts[] = {
        {"config",      required_argument, nullptr, 'c'},
        {"init-config", no_argument,       nullptr, 'I'},
        {"foreground",  no_argument,       nullptr, 'f'},
        {"help",        no_argument,       nullptr, 'h'},
        {nullptr, 0, nullptr, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "c:fhI", long_opts, nullptr)) != -1) {
        switch (opt) {
            case 'c':
                config_path = optarg;
                break;
            case 'f': foreground = true; break;
            case 'I': init_config = true; break;
            case 'h':
                print_usage(argv[0]);
                return 0;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }

    if (!config_path.empty()) {
        sigaw::Config::set_override_path(config_path);
    }

    /* Load config */
    sigaw::Config config = sigaw::Config::load();
    bool profile_chat_requested = sigaw::Config::any_profile_requests_chat();
    sigaw::ConfigWatcher config_watcher;
    config_watcher.sync();

    if (init_config) {
        config.write_default();
        fprintf(stderr, "[sigaw] Config written to: %s\n",
                sigaw::Config::config_path().c_str());
        return 0;
    }

    if (config.client_id.empty() || config.client_secret.empty()) {
        fprintf(stderr,
            "[sigaw] Error: client_id/client_secret not set in config.\n"
            "[sigaw] Run: sigaw-daemon --init-config\n"
            "[sigaw] Or restore the default public credentials in the config file.\n");
        return 1;
    }

    if (!foreground) {
        fprintf(stderr, "[sigaw] Background mode is not supported; use a user service.\n");
        return 1;
    }

    /* Signal handling */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* Initialize shared memory */
    sigaw::ShmWriter shm;
    if (!shm.open()) {
        fprintf(stderr, "[sigaw] Failed to create shared memory\n");
        return 1;
    }

    sigaw::ControlServer ctl;
    if (!ctl.open()) {
        fprintf(stderr, "[sigaw] Failed to open control socket\n");
        return 1;
    }

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        fprintf(stderr, "[sigaw] Failed to initialize libcurl\n");
        return 1;
    }

    {
        sigaw::AvatarCache avatar_cache;
        const sigaw::VoiceSyncConfig sync_config{};
        sigaw::tray::Icon tray;
        (void)tray.open();

        const sigaw::VoiceState no_voice;
        update_tray(
            &tray,
            config,
            sigaw::tray::DiscordConnectionState::Disconnected,
            no_voice
        );

        bool waiting_for_discord = false;

        /* Main reconnection loop */
        while (g_running) {
            sigaw::DiscordIpc ipc(config.client_id);

            if (!ipc.connect()) {
                if (!waiting_for_discord) {
                    fprintf(stderr,
                            "[sigaw] Discord IPC socket not found; retrying every 5s until Discord starts\n");
                    waiting_for_discord = true;
                }
                bool force_reauthenticate = false;
                wait_with_control(
                    ctl,
                    config,
                    &profile_chat_requested,
                    &config_watcher,
                    5000,
                    &tray,
                    nullptr,
                    &force_reauthenticate,
                    sigaw::tray::DiscordConnectionState::Disconnected
                );
                if (force_reauthenticate) {
                    sigaw::DiscordIpc::clear_saved_token_cache();
                }
                continue;
            }

            waiting_for_discord = false;

            if (!ipc.handshake()) {
                fprintf(stderr, "[sigaw] Handshake failed, retrying in 5s...\n");
                bool force_reauthenticate = false;
                wait_with_control(
                    ctl,
                    config,
                    &profile_chat_requested,
                    &config_watcher,
                    5000,
                    &tray,
                    nullptr,
                    &force_reauthenticate,
                    sigaw::tray::DiscordConnectionState::Disconnected
                );
                if (force_reauthenticate) {
                    sigaw::DiscordIpc::clear_saved_token_cache();
                }
                continue;
            }

            /* Authenticate */
            bool auth_reconnect_requested = false;
            bool auth_reauthenticate_requested = false;
            const bool want_daemon_chat = daemon_chat_requested(config, profile_chat_requested);
            update_tray(
                &tray,
                config,
                sigaw::tray::DiscordConnectionState::AuthorizationPending,
                no_voice
            );
            const auto auth_result = ipc.authorize(
                config.client_secret,
                want_daemon_chat,
                kDiscordAuthorizeTimeoutMs,
                [&]() {
                    return pump_pre_auth_controls(
                        ctl,
                        config,
                        &profile_chat_requested,
                        &config_watcher,
                        &tray,
                        &auth_reconnect_requested,
                        &auth_reauthenticate_requested,
                        sigaw::tray::DiscordConnectionState::AuthorizationPending
                    );
                }
            );
            if (!g_running) {
                break;
            }
            if (auth_reconnect_requested ||
                auth_result == sigaw::DiscordIpc::AuthorizeResult::Aborted) {
                if (auth_reauthenticate_requested) {
                    sigaw::DiscordIpc::clear_saved_token_cache();
                }
                continue;
            }
            if (auth_result == sigaw::DiscordIpc::AuthorizeResult::TimedOut) {
                fprintf(stderr,
                        "[sigaw] Discord authorization timed out; use the tray menu to reauthenticate\n");
            } else if (auth_result == sigaw::DiscordIpc::AuthorizeResult::Failed) {
                fprintf(stderr,
                        "[sigaw] Auth failed; use the tray menu to reauthenticate\n");
            }
            if (auth_result == sigaw::DiscordIpc::AuthorizeResult::TimedOut ||
                auth_result == sigaw::DiscordIpc::AuthorizeResult::Failed) {
                ipc.disconnect();
                bool reconnect_requested = false;
                bool reauthenticate_requested = false;
                update_tray(
                    &tray,
                    config,
                    sigaw::tray::DiscordConnectionState::AuthorizationRequired,
                    no_voice
                );
                wait_with_control(
                    ctl,
                    config,
                    &profile_chat_requested,
                    &config_watcher,
                    -1,
                    &tray,
                    &reconnect_requested,
                    &reauthenticate_requested,
                    sigaw::tray::DiscordConnectionState::AuthorizationRequired
                );
                if (!g_running) {
                    break;
                }
                if (reauthenticate_requested) {
                    sigaw::DiscordIpc::clear_saved_token_cache();
                }
                continue;
            }

            /* Subscribe to voice channel selection */
            ipc.subscribe_voice();

            /* Check if we're already in a voice channel */
            sigaw::VoiceRuntimeState runtime;
            runtime.voice = ipc.get_selected_voice_channel();
            if (!runtime.voice.channel_id.empty()) {
                ipc.subscribe_channel(runtime.voice.channel_id);
                runtime.subscribed_channel_id = runtime.voice.channel_id;
            }

            if (!runtime.voice.channel_id.empty() && runtime.voice.users.empty()) {
                sigaw::schedule_channel_sync(runtime, ipc, runtime.voice.channel_id,
                                             std::chrono::steady_clock::now(),
                                             sync_config, false);
                const auto sync_result = sigaw::sync_pending_channel(
                    runtime, ipc, std::chrono::steady_clock::now(), sync_config
                );
                (void)sync_result;
            }

            if (!runtime.voice.channel_id.empty()) {
                fprintf(stderr, "[sigaw] Already in: %s (%zu users)\n",
                        runtime.voice.channel_name.c_str(), runtime.voice.users.size());
            }

            (void)refresh_chat_state(
                runtime, ipc, daemon_chat_requested(config, profile_chat_requested)
            );
            update_shm(shm, runtime.voice, avatar_cache);
            update_tray(
                &tray,
                config,
                sigaw::tray::DiscordConnectionState::Connected,
                runtime.voice
            );

            /* Event loop */
            fprintf(stderr, "[sigaw] Listening for voice events...\n");

            bool reconnect_requested = false;
            bool force_reauthenticate = false;
            while (g_running && ipc.is_connected()) {
                bool state_changed = false;
                bool closed = false;

                const auto tray_result = process_tray_actions(
                    &tray,
                    config,
                    profile_chat_requested,
                    ipc.has_scope("messages.read"),
                    &runtime,
                    &ipc
                );
                if (tray_result.quit_requested) {
                    g_running = 0;
                    break;
                }
                if (tray_result.reconnect_requested) {
                    reconnect_requested = true;
                    force_reauthenticate = tray_result.reauthenticate_requested;
                    break;
                }
                state_changed = state_changed || tray_result.state_changed;

                int timeout_ms = 250;
                const auto timeout_now = std::chrono::steady_clock::now();
                if (runtime.pending_sync.active()) {
                    const auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(
                        runtime.pending_sync.next_attempt - timeout_now
                    ).count();
                    timeout_ms = std::clamp(static_cast<int>(delay), 0, timeout_ms);
                }
                timeout_ms = std::min(
                    timeout_ms,
                    sigaw::chat::next_timeout_ms(
                        runtime.voice,
                        sigaw::chat::steady_clock_ms(timeout_now),
                        timeout_ms
                    )
                );

                struct pollfd fds[2] = {};
                nfds_t fd_count = 0;
                if (ipc.fd() >= 0) {
                    fds[fd_count++] = {ipc.fd(), POLLIN, 0};
                }
                if (ctl.fd() >= 0) {
                    fds[fd_count++] = {ctl.fd(), POLLIN, 0};
                }
                if (fd_count > 0) {
                    (void)::poll(fds, fd_count, timeout_ms);
                }

                while (true) {
                    json event = ipc.poll_event(0);
                    if (event.is_null() || event.empty()) {
                        break;
                    }
                    if (event.contains("_closed")) {
                        closed = true;
                        break;
                    }

                    try {
                        const auto now = std::chrono::steady_clock::now();
                        const auto evt_name = sigaw::json_utils::string_or(event, "evt");
                        if (evt_name == "MESSAGE_CREATE" ||
                            evt_name == "MESSAGE_UPDATE" ||
                            evt_name == "MESSAGE_DELETE") {
                            if (!runtime.voice.channel_id.empty() &&
                                runtime.active_chat_channel_id == runtime.voice.channel_id &&
                                sigaw::chat::process_chat_event(
                                    event,
                                    runtime.voice,
                                    sigaw::chat::steady_clock_ms(now),
                                    SIGAW_MAX_CHAT_MESSAGES
                                )) {
                                state_changed = true;
                            }
                        } else {
                            const auto update = sigaw::process_voice_event(
                                event, runtime, ipc, now, sync_config
                            );
                            if (update.left_channel) {
                                fprintf(stderr, "[sigaw] Left voice channel\n");
                            }
                            if (!update.user_joined.empty()) {
                                fprintf(stderr, "[sigaw] + %s\n", update.user_joined.c_str());
                            }
                            if (!update.user_left.empty()) {
                                fprintf(stderr, "[sigaw] - %s\n", update.user_left.c_str());
                            }
                            state_changed = state_changed || update.state_changed;
                        }
                    } catch (const std::exception& e) {
                        fprintf(stderr, "[sigaw] Ignoring malformed Discord event: %s\n",
                                e.what());
                    }
                }

                if (closed) {
                    break;
                }

                const auto sync_update = sigaw::sync_pending_channel(
                    runtime, ipc, std::chrono::steady_clock::now(), sync_config
                );
                if (sync_update.joined_channel) {
                    fprintf(stderr, "[sigaw] Joined: %s (%zu users)\n",
                            runtime.voice.channel_name.c_str(), runtime.voice.users.size());
                }
                state_changed = state_changed || sync_update.state_changed;
                const bool chat_state_changed = refresh_chat_state(
                    runtime,
                    ipc,
                    daemon_chat_requested(config, profile_chat_requested)
                );
                state_changed = state_changed || chat_state_changed;

                const uint64_t now_ms = sigaw::chat::steady_clock_now_ms();
                if (sigaw::chat::prune_expired_messages(runtime.voice, now_ms)) {
                    state_changed = true;
                }

                if (state_changed) {
                    update_shm(shm, runtime.voice, avatar_cache);
                }

                if (config_watcher.consume_change()) {
                    const auto current = config;
                    const auto updated = sigaw::Config::load();
                    const bool updated_profile_chat_requested =
                        sigaw::Config::any_profile_requests_chat();
                    reconnect_requested = daemon_requires_reconnect(
                        current,
                        updated,
                        daemon_chat_requested(current, profile_chat_requested),
                        daemon_chat_requested(updated, updated_profile_chat_requested),
                        ipc.has_scope("messages.read")
                    );
                    config = updated;
                    profile_chat_requested = updated_profile_chat_requested;
                    if (reconnect_requested) {
                        break;
                    }
                    const bool chat_changed = refresh_chat_state(
                        runtime,
                        ipc,
                        daemon_chat_requested(config, profile_chat_requested)
                    );
                    state_changed = state_changed || chat_changed;
                }

                const auto action = ctl.process_pending(config);
                if (action == sigaw::ControlServer::Action::Quit) {
                    g_running = 0;
                    break;
                }
                if (action == sigaw::ControlServer::Action::Reload) {
                    profile_chat_requested = sigaw::Config::any_profile_requests_chat();
                    reconnect_requested = true;
                    break;
                }

                (void)avatar_cache.consume_dirty();

                update_tray(
                    &tray,
                    config,
                    sigaw::tray::DiscordConnectionState::Connected,
                    runtime.voice
                );
            }

            fprintf(stderr, "[sigaw] Disconnected, reconnecting...\n");

            /* Clear voice state on disconnect */
            runtime = {};
            update_shm(shm, runtime.voice, avatar_cache);
            update_tray(
                &tray,
                config,
                sigaw::tray::DiscordConnectionState::Disconnected,
                runtime.voice
            );

            if (!g_running) {
                break;
            }

            if (force_reauthenticate) {
                sigaw::DiscordIpc::clear_saved_token_cache();
            }

            if (!reconnect_requested) {
                bool wait_force_reauthenticate = false;
                wait_with_control(
                    ctl,
                    config,
                    &profile_chat_requested,
                    &config_watcher,
                    2000,
                    &tray,
                    nullptr,
                    &wait_force_reauthenticate,
                    sigaw::tray::DiscordConnectionState::Disconnected
                );
                if (wait_force_reauthenticate) {
                    sigaw::DiscordIpc::clear_saved_token_cache();
                }
            }
        }
    }

    ctl.close();
    curl_global_cleanup();
    fprintf(stderr, "[sigaw] Shutting down\n");
    return 0;
}
