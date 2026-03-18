#ifndef SIGAW_SHM_WRITER_H
#define SIGAW_SHM_WRITER_H

#include "../common/protocol.h"
#include "../common/config.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>

namespace sigaw {

class ShmWriter {
public:
    ShmWriter() = default;
    ~ShmWriter() { close(); }

    ShmWriter(const ShmWriter&) = delete;
    ShmWriter& operator=(const ShmWriter&) = delete;

    bool open() {
        if (mapped_) return true;

        const auto shm_name = Config::shared_memory_name();

        /* Create or open the shared memory segment */
        fd_ = shm_open(shm_name.c_str(), O_CREAT | O_RDWR,
                        S_IRUSR | S_IWUSR);
        if (fd_ < 0) {
            perror("[sigaw] shm_open");
            return false;
        }

        /* Set size */
        if (ftruncate(fd_, sizeof(SigawState)) < 0) {
            perror("[sigaw] ftruncate");
            ::close(fd_);
            fd_ = -1;
            return false;
        }

        void* ptr = mmap(nullptr, sizeof(SigawState),
                         PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (ptr == MAP_FAILED) {
            perror("[sigaw] mmap");
            ::close(fd_);
            fd_ = -1;
            return false;
        }

        state_ = static_cast<SigawState*>(ptr);
        mapped_ = true;

        /* Initialize with magic and version */
        memset(state_, 0, sizeof(SigawState));
        state_->header.magic   = SIGAW_MAGIC;
        state_->header.version = SIGAW_VERSION;
        state_->header.sequence = 0;

        fprintf(stderr, "[sigaw] Shared memory created: %s (%zu bytes)\n",
                shm_name.c_str(), sizeof(SigawState));
        return true;
    }

    void close() {
        if (mapped_ && state_) {
            munmap(state_, sizeof(SigawState));
            state_ = nullptr;
        }
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        if (mapped_) {
            /* Unlink the shm segment so it's cleaned up */
            const auto shm_name = Config::shared_memory_name();
            shm_unlink(shm_name.c_str());
        }
        mapped_ = false;
    }

    /*
     * Begin a write transaction. Bumps sequence to odd (signals readers
     * that a write is in progress).
     */
    void begin_write() {
        if (!mapped_) return;
        uint64_t seq = __atomic_load_n(&state_->header.sequence, __ATOMIC_RELAXED);
        __atomic_store_n(&state_->header.sequence, seq + 1, __ATOMIC_RELEASE);
        __atomic_thread_fence(__ATOMIC_RELEASE);
    }

    /*
     * End a write transaction. Bumps sequence to even (signals readers
     * that the write is complete and data is consistent).
     */
    void end_write() {
        if (!mapped_) return;
        __atomic_thread_fence(__ATOMIC_RELEASE);
        uint64_t seq = __atomic_load_n(&state_->header.sequence, __ATOMIC_RELAXED);
        __atomic_store_n(&state_->header.sequence, seq + 1, __ATOMIC_RELEASE);
    }

    /* Direct accessors for writing (must be between begin/end_write) */

    void set_channel(const char* name) {
        if (!state_) return;
        size_t len = strlen(name);
        if (len >= sizeof(state_->header.channel_name))
            len = sizeof(state_->header.channel_name) - 1;
        memcpy(state_->header.channel_name, name, len);
        state_->header.channel_name[len] = '\0';
        state_->header.channel_name_len = (uint32_t)len;
    }

    void clear_channel() {
        if (!state_) return;
        state_->header.channel_name[0] = '\0';
        state_->header.channel_name_len = 0;
        state_->user_count = 0;
        clear_chat();
    }

    void set_user_count(uint32_t count) {
        if (!state_) return;
        state_->user_count = (count > SIGAW_MAX_USERS) ? SIGAW_MAX_USERS : count;
    }

    void clear_chat() {
        if (!state_) return;
        state_->chat_count = 0;
        std::memset(state_->chat_messages, 0, sizeof(state_->chat_messages));
    }

    void set_chat_count(uint32_t count) {
        if (!state_) return;
        state_->chat_count =
            (count > SIGAW_MAX_CHAT_MESSAGES) ? SIGAW_MAX_CHAT_MESSAGES : count;
    }

    SigawUser* get_user(uint32_t index) {
        if (!state_ || index >= SIGAW_MAX_USERS) return nullptr;
        return &state_->users[index];
    }

    void set_user(uint32_t index, uint64_t id, const char* username,
                  const char* avatar, bool speaking,
                  bool self_mute, bool self_deaf,
                  bool server_mute, bool server_deaf)
    {
        if (!state_ || index >= SIGAW_MAX_USERS) return;

        SigawUser* u = &state_->users[index];
        u->user_id = id;

        strncpy(u->username, username, sizeof(u->username) - 1);
        u->username[sizeof(u->username) - 1] = '\0';

        strncpy(u->avatar_hash, avatar, sizeof(u->avatar_hash) - 1);
        u->avatar_hash[sizeof(u->avatar_hash) - 1] = '\0';

        u->speaking    = speaking ? 1 : 0;
        u->self_mute   = self_mute ? 1 : 0;
        u->self_deaf   = self_deaf ? 1 : 0;
        u->server_mute = server_mute ? 1 : 0;
        u->server_deaf = server_deaf ? 1 : 0;
        u->suppress    = 0;
    }

    void set_chat_message(uint32_t index,
                          uint64_t message_id,
                          uint64_t author_id,
                          uint64_t observed_at_ms,
                          const char* author_name,
                          const char* content)
    {
        if (!state_ || index >= SIGAW_MAX_CHAT_MESSAGES) return;

        SigawChatMessage* message = &state_->chat_messages[index];
        std::memset(message, 0, sizeof(*message));
        message->message_id = message_id;
        message->author_id = author_id;
        message->observed_at_ms = observed_at_ms;

        size_t author_len = std::strlen(author_name);
        if (author_len >= sizeof(message->author_name)) {
            author_len = sizeof(message->author_name) - 1;
        }
        std::memcpy(message->author_name, author_name, author_len);
        message->author_name[author_len] = '\0';
        message->author_name_len = static_cast<uint32_t>(author_len);

        size_t content_len = std::strlen(content);
        if (content_len >= sizeof(message->content)) {
            content_len = sizeof(message->content) - 1;
        }
        std::memcpy(message->content, content, content_len);
        message->content[content_len] = '\0';
        message->content_len = static_cast<uint32_t>(content_len);
    }

    bool is_open() const { return mapped_; }

private:
    int          fd_     = -1;
    SigawState*  state_  = nullptr;
    bool         mapped_ = false;
};

} /* namespace sigaw */

#endif /* SIGAW_SHM_WRITER_H */
