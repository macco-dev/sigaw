#ifndef SIGAW_WINE_POLICY_H
#define SIGAW_WINE_POLICY_H

#include <ctype.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum SigawWinePolicy {
    SIGAW_WINE_POLICY_AUTO = 0,
    SIGAW_WINE_POLICY_FORCE = 1,
    SIGAW_WINE_POLICY_DISABLE = 2,
} SigawWinePolicy;

static inline int sigaw_ascii_ieq(const char* lhs, const char* rhs) {
    if (!lhs || !rhs) {
        return 0;
    }

    while (*lhs && *rhs) {
        const int left = tolower((unsigned char)*lhs);
        const int right = tolower((unsigned char)*rhs);
        if (left != right) {
            return 0;
        }
        ++lhs;
        ++rhs;
    }

    return *lhs == '\0' && *rhs == '\0';
}

static inline int sigaw_is_wine_environment(void) {
    const char* steam_compat = getenv("STEAM_COMPAT_DATA_PATH");
    if (steam_compat && *steam_compat) {
        return 1;
    }

    const char* wineprefix = getenv("WINEPREFIX");
    if (wineprefix && *wineprefix) {
        return 1;
    }

    const char* winedllpath = getenv("WINEDLLPATH");
    if (winedllpath && *winedllpath) {
        return 1;
    }

    return 0;
}

static inline SigawWinePolicy sigaw_wine_policy_from_env(void) {
    const char* env = getenv("SIGAW_WINE_POLICY");
    if (!env || !*env || sigaw_ascii_ieq(env, "auto")) {
        return SIGAW_WINE_POLICY_AUTO;
    }
    if (sigaw_ascii_ieq(env, "force")) {
        return SIGAW_WINE_POLICY_FORCE;
    }
    if (sigaw_ascii_ieq(env, "disable")) {
        return SIGAW_WINE_POLICY_DISABLE;
    }
    return SIGAW_WINE_POLICY_AUTO;
}

#ifdef __cplusplus
}
#endif

#endif /* SIGAW_WINE_POLICY_H */
