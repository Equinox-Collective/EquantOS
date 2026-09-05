// src/kernel/misc/user.c - User Authentication & Session Engine
#include "user.h"
#include "string.h"
#include "stdio.h"
#include "../proc/task.h"
#include "../drivers/serial/serial.h"
#include "../core/initcall.h"

static user_account_t user_table[MAX_USERS];
static user_account_t *active_user = NULL;

static const char *DEFAULT_SALT = "EquantOS_v0.1_SecuritySalt_2026";

// ============================================================================
// Built-in Cryptographic SHA-256 Engine (Self-contained, Zero Dependencies)
// ============================================================================

typedef struct {
    uint32_t state[8];
    uint64_t bitcount;
    uint8_t buffer[64];
} sha256_ctx_t;

#define ROTRIGHT(word, bits) (((word) >> (bits)) | ((word) << (32 - (bits))))
#define CH(x, y, z)          (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z)         (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x)               (ROTRIGHT(x, 2) ^ ROTRIGHT(x, 13) ^ ROTRIGHT(x, 22))
#define EP1(x)               (ROTRIGHT(x, 6) ^ ROTRIGHT(x, 11) ^ ROTRIGHT(x, 25))
#define SIG0(x)              (ROTRIGHT(x, 7) ^ ROTRIGHT(x, 18) ^ ((x) >> 3))
#define SIG1(x)              (ROTRIGHT(x, 17) ^ ROTRIGHT(x, 19) ^ ((x) >> 10))

static const uint32_t sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static void sha256_transform(sha256_ctx_t *ctx, const uint8_t data[64]) {
    uint32_t a, b, c, d, e, f, g, h, t1, t2, m[64];
    for (int i = 0, j = 0; i < 16; ++i, j += 4) {
        m[i] = ((uint32_t)data[j] << 24) | ((uint32_t)data[j + 1] << 16) |
               ((uint32_t)data[j + 2] << 8) | ((uint32_t)data[j + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        m[i] = SIG1(m[i - 2]) + m[i - 7] + SIG0(m[i - 15]) + m[i - 16];
    }
    a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; d = ctx->state[3];
    e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];

    for (int i = 0; i < 64; ++i) {
        t1 = h + EP1(e) + CH(e, f, g) + sha256_k[i] + m[i];
        t2 = EP0(a) + MAJ(a, b, c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

static void sha256_init(sha256_ctx_t *ctx) {
    ctx->bitcount = 0;
    ctx->state[0] = 0x6a09e667; ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372; ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f; ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab; ctx->state[7] = 0x5be0cd19;
}

static void sha256_update(sha256_ctx_t *ctx, const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        ctx->buffer[(ctx->bitcount >> 3) & 63] = data[i];
        ctx->bitcount += 8;
        if ((ctx->bitcount & 511) == 0) {
            sha256_transform(ctx, ctx->buffer);
        }
    }
}

static void sha256_final(sha256_ctx_t *ctx, uint8_t hash[32]) {
    size_t i = (ctx->bitcount >> 3) & 63;
    ctx->buffer[i++] = 0x80;
    if (i > 56) {
        while (i < 64) ctx->buffer[i++] = 0x00;
        sha256_transform(ctx, ctx->buffer);
        i = 0;
    }
    while (i < 56) ctx->buffer[i++] = 0x00;
    for (int j = 7; j >= 0; --j) {
        ctx->buffer[56 + (7 - j)] = (uint8_t)(ctx->bitcount >> (j * 8));
    }
    sha256_transform(ctx, ctx->buffer);
    for (int j = 0; j < 4; ++j) {
        for (int k = 0; k < 8; ++k) {
            hash[k * 4 + j] = (uint8_t)((ctx->state[k] >> (24 - j * 8)) & 0x000000ff);
        }
    }
}

void user_hash_password(const char *password, const char *salt, char *output_hash) {
    if (!password || !output_hash) return;
    const char *active_salt = salt ? salt : DEFAULT_SALT;

    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, (const uint8_t *)active_salt, strlen(active_salt));
    sha256_update(&ctx, (const uint8_t *)password, strlen(password));

    uint8_t raw_hash[32];
    sha256_final(&ctx, raw_hash);

    static const char hex_digits[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        output_hash[i * 2]     = hex_digits[(raw_hash[i] >> 4) & 0x0F];
        output_hash[i * 2 + 1] = hex_digits[raw_hash[i] & 0x0F];
    }
    output_hash[64] = '\0';
}

// ============================================================================
// User Accounts Implementation
// ============================================================================

void user_subsys_init(void) {
    memset(user_table, 0, sizeof(user_table));

    // 1. Root Superuser (Default password: "root")
    user_add("root", "root", UID_ROOT, GID_ROOT);
    user_account_t *root = user_find_by_uid(UID_ROOT);
    if (root) {
        strncpy(root->home_dir, "/root", sizeof(root->home_dir) - 1);
        strncpy(root->shell_path, "/bin/sh", sizeof(root->shell_path) - 1);
    }

    // 2. Default Standard User (Default password: "default")
    user_add("default", "default", UID_DEFAULT_USER, GID_DEFAULT_USER);
    user_account_t *def = user_find_by_uid(UID_DEFAULT_USER);
    if (def) {
        strncpy(def->home_dir, "/home/default", sizeof(def->home_dir) - 1);
        strncpy(def->shell_path, "/bin/sh", sizeof(def->shell_path) - 1);
    }

    // Initial session starts as root on Live/Bootstrap
    active_user = root;

    if (current_task && current_task->process) {
        current_task->process->uid = active_user->uid;
        current_task->process->gid = active_user->gid;
        current_task->process->euid = active_user->uid;
        current_task->process->egid = active_user->gid;
    }

    serial_puts(COM1, "[USER] User and security subsystem initialized (root, default).\n");
}

user_account_t *user_get_current(void) {
    return active_user;
}

bool user_is_root(void) {
    return (active_user && active_user->uid == UID_ROOT);
}

user_account_t *user_find_by_name(const char *username) {
    if (!username) return NULL;
    for (int i = 0; i < MAX_USERS; i++) {
        if (user_table[i].active && strcmp(user_table[i].username, username) == 0) {
            return &user_table[i];
        }
    }
    return NULL;
}

user_account_t *user_find_by_uid(uint32_t uid) {
    for (int i = 0; i < MAX_USERS; i++) {
        if (user_table[i].active && user_table[i].uid == uid) {
            return &user_table[i];
        }
    }
    return NULL;
}

bool user_add(const char *username, const char *password, uint32_t uid, uint32_t gid) {
    if (!username || !password || strlen(username) == 0) return false;
    if (user_find_by_name(username) || user_find_by_uid(uid)) return false;

    for (int i = 0; i < MAX_USERS; i++) {
        if (!user_table[i].active) {
            user_table[i].uid = uid;
            user_table[i].gid = gid;
            strncpy(user_table[i].username, username, USER_NAME_MAX - 1);
            user_hash_password(password, DEFAULT_SALT, user_table[i].password_hash);
            snprintf(user_table[i].home_dir, sizeof(user_table[i].home_dir), "/home/%s", username);
            strncpy(user_table[i].shell_path, "/bin/sh", sizeof(user_table[i].shell_path) - 1);
            user_table[i].active = true;
            return true;
        }
    }
    return false;
}

bool user_set_password(const char *username, const char *new_password) {
    user_account_t *user = user_find_by_name(username);
    if (!user) return false;

    user_hash_password(new_password, DEFAULT_SALT, user->password_hash);
    return true;
}

bool user_authenticate(const char *username, const char *password) {
    user_account_t *user = user_find_by_name(username);
    if (!user) return false;

    char test_hash[USER_HASH_LEN];
    user_hash_password(password, DEFAULT_SALT, test_hash);

    return (strcmp(user->password_hash, test_hash) == 0);
}

static void sync_current_task_credentials(void) {
    if (current_task && current_task->process && active_user) {
        current_task->process->uid = active_user->uid;
        current_task->process->gid = active_user->gid;
        current_task->process->euid = active_user->uid;
        current_task->process->egid = active_user->gid;
    }
}

bool user_switch(const char *username, const char *password) {
    if (!username || !password) return false;

    // Root can switch to anyone without password verification
    if (user_is_root()) {
        user_account_t *target = user_find_by_name(username);
        if (!target) return false;
        active_user = target;
        sync_current_task_credentials();
        return true;
    }

    if (!user_authenticate(username, password)) {
        return false;
    }

    active_user = user_find_by_name(username);
    sync_current_task_credentials();
    return true;
}

bool user_switch_force(uint32_t uid) {
    user_account_t *target = user_find_by_uid(uid);
    if (!target) return false;
    active_user = target;
    sync_current_task_credentials();
    return true;
}

static int __init user_initcall(void) {
    user_subsys_init();
    return 0;
}
subsys_initcall(user_initcall);