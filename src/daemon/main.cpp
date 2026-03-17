#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <cstring>
#include <string>
#include <unistd.h>
#include <getopt.h>
#include <algorithm>
#include <chrono>
#include <thread>

#include "avatar_cache.h"
#include "control_server.h"
#include "discord_ipc.h"
#include "json_utils.h"
#include "shm_writer.h"
#include "voice_runtime.h"
#include "../common/config.h"
#include "../common/config_watcher.h"
#include "../common/protocol.h"

static volatile sig_atomic_t g_running = 1;

static void signal_handler(int) {
    g_running = 0;
}

static bool daemon_requires_reconnect(const sigaw::Config& current,
                                      const sigaw::Config& updated)
{
    return current.client_id != updated.client_id ||
           current.client_secret != updated.client_secret;
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
        "Vulkan overlay layer to read.\n"
        "\n"
        "Setup:\n"
        "  1. Run: sigaw-daemon --init-config\n"
        "  2. Run: sigaw-daemon --foreground\n"
        "  3. Approve the authorization in Discord (one-time)\n"
        "  4. Launch games with: SIGAW=1 <game>\n",
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
    }

    shm.end_write();
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
            if (!ipc.authorize(config.client_secret)) {
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

            update_shm(shm, runtime.voice, avatar_cache);

            /* Event loop */
            fprintf(stderr, "[sigaw] Listening for voice events...\n");

            bool reconnect_requested = false;
            while (g_running && ipc.is_connected()) {
                json event = ipc.poll_event();
                bool state_changed = false;

                if (!event.is_null() && !event.empty()) {
                    if (event.contains("_closed")) break;

                    try {
                        const auto update = sigaw::process_voice_event(
                            event, runtime, ipc, std::chrono::steady_clock::now(), sync_config
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
                        state_changed = update.state_changed;
                    } catch (const std::exception& e) {
                        fprintf(stderr, "[sigaw] Ignoring malformed Discord event: %s\n",
                                e.what());
                    }
                }

                const auto sync_update = sigaw::sync_pending_channel(
                    runtime, ipc, std::chrono::steady_clock::now(), sync_config
                );
                if (sync_update.joined_channel) {
                    fprintf(stderr, "[sigaw] Joined: %s (%zu users)\n",
                            runtime.voice.channel_name.c_str(), runtime.voice.users.size());
                }
                state_changed = state_changed || sync_update.state_changed;

                if (state_changed) {
                    update_shm(shm, runtime.voice, avatar_cache);
                }

                if (config_watcher.consume_change()) {
                    const auto updated = sigaw::Config::load();
                    reconnect_requested = daemon_requires_reconnect(config, updated);
                    config = updated;
                    if (reconnect_requested) {
                        break;
                    }
                }

                const auto action = ctl.process_pending(config);
                if (action == sigaw::ControlServer::Action::Quit) {
                    g_running = 0;
                    break;
                }
                if (action == sigaw::ControlServer::Action::Reload) {
                    reconnect_requested = true;
                    break;
                }

                /* ~16ms poll interval (60Hz) -- low CPU, fast enough for
                 * speaking indicators to feel responsive */
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            }

            fprintf(stderr, "[sigaw] Disconnected, reconnecting...\n");

            /* Clear voice state on disconnect */
            runtime = {};
            update_shm(shm, runtime.voice, avatar_cache);

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
