#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <cstring>
#include <string>
#include <unistd.h>
#include <getopt.h>
#include <algorithm>
#include <chrono>
#include <poll.h>
#include <thread>
#include <unordered_map>

#include "avatar_cache.h"
#include "chat_runtime.h"
#include "control_server.h"
#include "discord_ipc.h"
#include "json_utils.h"
#include "shm_writer.h"
#include "voice_runtime.h"
#include "../common/config.h"
#include "../common/config_watcher.h"
#include "../common/overlay_frame_shm.h"
#include "../common/protocol.h"
#include "../layer/overlay_animation.h"
#include "../layer/overlay_layout.h"
#include "../layer/overlay_preview.h"

static volatile sig_atomic_t g_running = 1;

static void signal_handler(int) {
    g_running = 0;
}

static bool daemon_requires_reconnect(const sigaw::Config& current,
                                      const sigaw::Config& updated,
                                      bool have_messages_read_scope = false)
{
    return current.client_id != updated.client_id ||
           current.client_secret != updated.client_secret ||
           (!current.show_voice_channel_chat &&
            updated.show_voice_channel_chat &&
            !have_messages_read_scope);
}

static bool wait_with_control(sigaw::ControlServer& ctl, sigaw::Config& config,
                              sigaw::ConfigWatcher* config_watcher,
                              int total_ms, bool* reconnect_requested = nullptr)
{
    for (int elapsed = 0; elapsed < total_ms && g_running; elapsed += 100) {
        if (config_watcher && config_watcher->consume_change()) {
            const auto updated = sigaw::Config::load();
            const bool reconnect = daemon_requires_reconnect(config, updated);
            config = updated;
            if (reconnect) {
                if (reconnect_requested) {
                    *reconnect_requested = true;
                }
                return false;
            }
        }

        const auto action = ctl.process_pending(config);
        if (action == sigaw::ControlServer::Action::Quit) {
            g_running = 0;
            return false;
        }
        if (action == sigaw::ControlServer::Action::Reload) {
            if (reconnect_requested) {
                *reconnect_requested = true;
            }
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

static SigawState make_overlay_state(const sigaw::VoiceState& voice) {
    SigawState state = {};
    state.header.magic = SIGAW_MAGIC;
    state.header.version = SIGAW_VERSION;
    if (voice.channel_id.empty()) {
        return state;
    }

    const size_t channel_len = std::min(
        voice.channel_name.size(),
        sizeof(state.header.channel_name) - 1
    );
    std::memcpy(state.header.channel_name, voice.channel_name.data(), channel_len);
    state.header.channel_name[channel_len] = '\0';
    state.header.channel_name_len = static_cast<uint32_t>(channel_len);

    const uint32_t count = std::min(
        static_cast<uint32_t>(voice.users.size()),
        static_cast<uint32_t>(SIGAW_MAX_USERS)
    );
    state.user_count = count;

    for (uint32_t i = 0; i < count; ++i) {
        const auto& src = voice.users[i];
        auto& dst = state.users[i];
        dst.user_id = src.id;
        std::strncpy(dst.username, src.username.c_str(), sizeof(dst.username) - 1);
        dst.username[sizeof(dst.username) - 1] = '\0';
        std::strncpy(dst.avatar_hash, src.avatar.c_str(), sizeof(dst.avatar_hash) - 1);
        dst.avatar_hash[sizeof(dst.avatar_hash) - 1] = '\0';
        dst.speaking = src.speaking ? 1u : 0u;
        dst.self_mute = src.self_mute ? 1u : 0u;
        dst.self_deaf = src.self_deaf ? 1u : 0u;
        dst.server_mute = src.server_mute ? 1u : 0u;
        dst.server_deaf = src.server_deaf ? 1u : 0u;
        dst.suppress = 0u;
    }

    const uint32_t chat_count = std::min(
        static_cast<uint32_t>(voice.chat_messages.size()),
        static_cast<uint32_t>(SIGAW_MAX_CHAT_MESSAGES)
    );
    state.chat_count = chat_count;
    for (uint32_t i = 0; i < chat_count; ++i) {
        const auto& src = voice.chat_messages[i];
        auto& dst = state.chat_messages[i];
        dst.message_id = src.id;
        dst.author_id = src.author_id;
        dst.observed_at_ms = src.observed_at_ms;

        const size_t author_len = std::min(src.author_name.size(), sizeof(dst.author_name) - 1);
        std::memcpy(dst.author_name, src.author_name.data(), author_len);
        dst.author_name[author_len] = '\0';
        dst.author_name_len = static_cast<uint32_t>(author_len);

        const size_t content_len = std::min(src.content.size(), sizeof(dst.content) - 1);
        std::memcpy(dst.content, src.content.data(), content_len);
        dst.content[content_len] = '\0';
        dst.content_len = static_cast<uint32_t>(content_len);
    }

    return state;
}

static std::unordered_map<uint64_t, float>
speaking_overlay_times(const sigaw::VoiceState& voice)
{
    std::unordered_map<uint64_t, float> times;
    for (const auto& user : voice.users) {
        if (user.speaking) {
            times[user.id] = sigaw::overlay::speaking_fade_ms;
        }
    }
    return times;
}

static sigaw::OverlayFrameSnapshot
build_overlay_frame(const sigaw::VoiceState& voice, const sigaw::Config& config)
{
    sigaw::OverlayFrameSnapshot frame;
    frame.position = config.position;
    frame.margin_px = static_cast<uint32_t>(sigaw::overlay::scaled_px(config.scale, 16));
    if (!config.visible || voice.channel_id.empty()) {
        return frame;
    }

    const SigawState state = make_overlay_state(voice);
    sigaw::preview::Image image;
    if (!sigaw::preview::render_panel_rgba(
            state, config, speaking_overlay_times(voice), image
        )) {
        return frame;
    }

    frame.visible = image.width != 0 && image.height != 0 && !image.rgba.empty();
    frame.width = image.width;
    frame.height = image.height;
    frame.stride = image.width * 4u;
    frame.rgba = std::move(image.rgba);
    return frame;
}

static void publish_overlay_frame(sigaw::OverlayFrameWriter& writer,
                                  const sigaw::VoiceState& voice,
                                  const sigaw::Config& config)
{
    if (!writer.publish(build_overlay_frame(voice, config))) {
        fprintf(stderr, "[sigaw] Failed to publish overlay frame\n");
    }
}

static bool chat_requested(const sigaw::Config& config) {
    return config.show_voice_channel_chat && config.max_visible_chat_messages > 0;
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
                               const sigaw::Config& config)
{
    const bool want_chat = chat_requested(config) && ipc.has_scope("messages.read");
    if (!want_chat || runtime.voice.channel_id.empty()) {
        return clear_chat_state(runtime, &ipc);
    }

    if (runtime.active_chat_channel_id == runtime.voice.channel_id) {
        return false;
    }

    const bool had_chat = clear_chat_state(runtime, &ipc);
    runtime.active_chat_channel_id = runtime.voice.channel_id;

    json history = json::array();
    bool loaded_history = false;
    if (!ipc.get_channel_messages(runtime.voice.channel_id, SIGAW_MAX_CHAT_MESSAGES, history)) {
        fprintf(stderr,
                "[sigaw] Voice channel chat history unavailable for %s; continuing with live updates only\n",
                runtime.voice.channel_name.c_str());
    } else {
        const uint64_t now_ms = sigaw::chat::steady_clock_now_ms();
        loaded_history = sigaw::chat::load_chat_history(
            runtime.voice, history, now_ms, SIGAW_MAX_CHAT_MESSAGES
        );
    }

    if (!ipc.subscribe_channel_messages(runtime.voice.channel_id)) {
        fprintf(stderr,
                "[sigaw] Voice channel chat live updates unavailable for %s\n",
                runtime.voice.channel_name.c_str());
        return had_chat || loaded_history;
    }

    runtime.subscribed_chat_channel_id = runtime.voice.channel_id;
    runtime.live_chat_events = true;
    return had_chat || loaded_history;
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
        sigaw::OverlayFrameWriter overlay_frame;
        const sigaw::VoiceSyncConfig sync_config{};

        /* Main reconnection loop */
        while (g_running) {
            fprintf(stderr, "[sigaw] Connecting to Discord...\n");

            sigaw::DiscordIpc ipc(config.client_id);

            if (!ipc.connect()) {
                fprintf(stderr, "[sigaw] Discord not found, retrying in 5s...\n");
                wait_with_control(ctl, config, &config_watcher, 5000);
                continue;
            }

            if (!ipc.handshake()) {
                fprintf(stderr, "[sigaw] Handshake failed, retrying in 5s...\n");
                wait_with_control(ctl, config, &config_watcher, 5000);
                continue;
            }

            /* Authenticate */
            bool authorized = ipc.authorize(config.client_secret, chat_requested(config));
            if (!authorized && chat_requested(config)) {
                fprintf(stderr,
                        "[sigaw] Chat authorization failed; continuing with voice-only overlay\n");
                authorized = ipc.authorize(config.client_secret, false);
            }
            if (!authorized) {
                fprintf(stderr, "[sigaw] Auth failed (check client_id/secret)\n");
                fprintf(stderr, "[sigaw] Retrying in 10s...\n");
                wait_with_control(ctl, config, &config_watcher, 10000);
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

            (void)refresh_chat_state(runtime, ipc, config);
            update_shm(shm, runtime.voice, avatar_cache);
            publish_overlay_frame(overlay_frame, runtime.voice, config);

            /* Event loop */
            fprintf(stderr, "[sigaw] Listening for voice events...\n");

            bool reconnect_requested = false;
            while (g_running && ipc.is_connected()) {
                bool state_changed = false;
                bool overlay_dirty = false;
                bool closed = false;

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
                                overlay_dirty = true;
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
                const bool chat_state_changed = refresh_chat_state(runtime, ipc, config);
                state_changed = state_changed || chat_state_changed;
                overlay_dirty = overlay_dirty || chat_state_changed;

                const uint64_t now_ms = sigaw::chat::steady_clock_now_ms();
                if (sigaw::chat::prune_expired_messages(runtime.voice, now_ms)) {
                    state_changed = true;
                    overlay_dirty = true;
                }
                if (sigaw::chat::repaint_due(runtime.voice, now_ms)) {
                    overlay_dirty = true;
                }

                if (state_changed) {
                    update_shm(shm, runtime.voice, avatar_cache);
                    overlay_dirty = true;
                }

                if (config_watcher.consume_change()) {
                    const auto current = config;
                    const auto updated = sigaw::Config::load();
                    reconnect_requested = daemon_requires_reconnect(
                        current, updated, ipc.has_scope("messages.read")
                    );
                    config = updated;
                    if (reconnect_requested) {
                        break;
                    }
                    overlay_dirty = true;
                }

                const auto action = ctl.process_pending(config);
                if (action == sigaw::ControlServer::Action::Quit) {
                    g_running = 0;
                    break;
                }
                if (action == sigaw::ControlServer::Action::Refresh) {
                    overlay_dirty = true;
                }
                if (action == sigaw::ControlServer::Action::Reload) {
                    reconnect_requested = true;
                    break;
                }

                if (avatar_cache.consume_dirty()) {
                    overlay_dirty = true;
                }

                if (overlay_dirty) {
                    publish_overlay_frame(overlay_frame, runtime.voice, config);
                }
            }

            fprintf(stderr, "[sigaw] Disconnected, reconnecting...\n");

            /* Clear voice state on disconnect */
            runtime = {};
            update_shm(shm, runtime.voice, avatar_cache);
            publish_overlay_frame(overlay_frame, runtime.voice, config);

            if (!g_running) {
                break;
            }

            if (!reconnect_requested) {
                wait_with_control(ctl, config, &config_watcher, 2000);
            }
        }
    }

    ctl.close();
    curl_global_cleanup();
    fprintf(stderr, "[sigaw] Shutting down\n");
    return 0;
}
