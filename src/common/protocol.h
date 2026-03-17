#ifndef SIGAW_PROTOCOL_H
#define SIGAW_PROTOCOL_H

#include <stdint.h>

#ifdef __cplusplus
#define SIGAW_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#else
#define SIGAW_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define SIGAW_MAGIC        0x53494741  /* "SIGA" */
#define SIGAW_VERSION      2
#define SIGAW_MAX_USERS    64
#define SIGAW_SHM_SIZE     sizeof(struct SigawState)

/* Socket path for daemon <-> ctl communication */
#define SIGAW_CTL_SOCKET   "/tmp/sigaw-ctl.sock"

/*
 * Header: 64 bytes, cache-line aligned.
 * The daemon writes the full state, then bumps `sequence` last (with a
 * release store). The layer reads `sequence` first (acquire load), reads
 * the state, then re-reads `sequence` -- if it changed, retry.
 * This gives us lock-free single-writer / single-reader consistency.
 */
struct __attribute__((aligned(64))) SigawHeader {
    uint32_t magic;
    uint32_t version;
    uint64_t sequence;            /* Monotonic, bumped on every state change */
    uint32_t channel_name_len;
    char     channel_name[40];    /* UTF-8, null-terminated */
    uint32_t _pad;
};

SIGAW_STATIC_ASSERT(sizeof(struct SigawHeader) == 64, "SigawHeader must be 64 bytes");

/*
 * Per-user voice state entry: 128 bytes each.
 * Packed to fit neatly in cache lines.
 */
struct __attribute__((aligned(128))) SigawUser {
    uint64_t user_id;
    char     username[48];        /* Display name / server nick, UTF-8, null-terminated */
    char     avatar_hash[44];     /* Discord CDN avatar hash (a_<hex>) */
    uint8_t  speaking;            /* 1 = currently speaking */
    uint8_t  self_mute;
    uint8_t  self_deaf;
    uint8_t  server_mute;
    uint8_t  server_deaf;
    uint8_t  suppress;            /* Stage channel suppress */
    uint8_t  _pad[22];
};

SIGAW_STATIC_ASSERT(sizeof(struct SigawUser) == 128, "SigawUser must be 128 bytes");

/*
 * Full voice channel state.
 * Total size: 64 + 8 + (128 * 64) = 8264 bytes -- fits in 3 pages.
 */
struct SigawState {
    struct SigawHeader header;
    uint32_t user_count;          /* Number of valid entries in users[] */
    uint32_t _pad;
    struct SigawUser users[SIGAW_MAX_USERS];
};

/* Control commands (daemon <-> ctl via Unix socket) */
enum SigawCtlCommand {
    SIGAW_CTL_STATUS    = 1,
    SIGAW_CTL_TOGGLE    = 2,
    SIGAW_CTL_RELOAD    = 3,
    SIGAW_CTL_QUIT      = 4,
};

struct SigawCtlRequest {
    uint32_t command;  /* enum SigawCtlCommand */
    uint32_t _pad;
};

struct SigawCtlResponse {
    uint32_t command;
    uint32_t status;   /* 0 = ok, nonzero = error */
    char     message[256];
};

#ifdef __cplusplus
}
#endif

#endif /* SIGAW_PROTOCOL_H */
