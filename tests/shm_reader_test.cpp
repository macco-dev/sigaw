#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <unistd.h>

#include "common/config.h"
#include "daemon/shm_writer.h"
#include "layer/shm_reader.h"

namespace {

struct ShmEnvGuard {
    explicit ShmEnvGuard(std::string value) : value_(std::move(value)) {
        setenv("SIGAW_SHM_NAME", value_.c_str(), 1);
        shm_unlink(value_.c_str());
    }

    ~ShmEnvGuard() {
        shm_unlink(value_.c_str());
        unsetenv("SIGAW_SHM_NAME");
    }

    std::string value_;
};

std::string unique_shm_name() {
    return "/sigaw-test-voice-" +
           std::to_string(static_cast<unsigned long long>(::getpid()));
}

void write_voice(sigaw::ShmWriter& writer,
                 const char* channel_name,
                 uint64_t user_id,
                 const char* username)
{
    writer.begin_write();
    writer.set_channel(channel_name);
    writer.set_user_count(1);
    writer.set_user(0, user_id, username, "", false, false, false, false, false);
    writer.set_chat_count(1);
    writer.set_chat_message(0, 55, 77, 9'999, "Bravo", "Need backup");
    writer.end_write();
}

bool test_default_shared_memory_name_uses_uid() {
    unsetenv("SIGAW_SHM_NAME");
    const auto expected = "/sigaw-voice-" +
                          std::to_string(static_cast<unsigned long long>(::getuid()));
    if (sigaw::Config::shared_memory_name() != expected) {
        std::cerr << "default SHM name should include the current uid\n";
        return false;
    }
    return true;
}

bool test_shared_memory_name_override_is_honored() {
    ShmEnvGuard guard(unique_shm_name());
    if (sigaw::Config::shared_memory_name() != guard.value_) {
        std::cerr << "SIGAW_SHM_NAME override was not honored\n";
        return false;
    }
    return true;
}

bool test_reader_disconnects_and_remaps_after_writer_restart() {
    ShmEnvGuard guard(unique_shm_name());

    sigaw::ShmWriter writer;
    if (!writer.open()) {
        std::cerr << "failed to open initial SHM writer\n";
        return false;
    }
    write_voice(writer, "Lobby", 7, "Alpha");

    sigaw::ShmReader reader(std::chrono::milliseconds(0));
    SigawState initial = {};
    if (!reader.read(initial)) {
        std::cerr << "reader failed to read initial voice state\n";
        return false;
    }
    if (std::string(initial.header.channel_name) != "Lobby" ||
        initial.user_count != 1 ||
        std::string(initial.users[0].username) != "Alpha" ||
        initial.chat_count != 1 ||
        std::string(initial.chat_messages[0].author_name) != "Bravo" ||
        std::string(initial.chat_messages[0].content) != "Need backup") {
        std::cerr << "initial SHM snapshot mismatch\n";
        return false;
    }

    writer.close();

    SigawState after_stop = {};
    if (reader.read(after_stop)) {
        std::cerr << "reader should stop reading once the named SHM object disappears\n";
        return false;
    }
    if (reader.is_connected()) {
        std::cerr << "reader should drop the orphaned mapping after daemon stop\n";
        return false;
    }

    sigaw::ShmWriter restarted_writer;
    if (!restarted_writer.open()) {
        std::cerr << "failed to open restarted SHM writer\n";
        return false;
    }
    write_voice(restarted_writer, "General", 9, "Bravo");

    SigawState restarted = {};
    if (!reader.read(restarted)) {
        std::cerr << "reader failed to remap after writer restart\n";
        return false;
    }
    if (std::string(restarted.header.channel_name) != "General" ||
        restarted.user_count != 1 ||
        std::string(restarted.users[0].username) != "Bravo" ||
        restarted.chat_count != 1 ||
        std::string(restarted.chat_messages[0].content) != "Need backup") {
        std::cerr << "reader did not pick up restarted SHM contents\n";
        return false;
    }

    restarted_writer.close();
    return true;
}

} // namespace

int main() {
    if (!test_default_shared_memory_name_uses_uid()) {
        return 1;
    }

    if (!test_shared_memory_name_override_is_honored()) {
        return 1;
    }

    if (!test_reader_disconnects_and_remaps_after_writer_restart()) {
        return 1;
    }

    return 0;
}
