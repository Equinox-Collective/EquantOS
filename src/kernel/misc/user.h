// src/kernel/misc/user.h - User Accounts, Authentication & Security Core
#ifndef USER_H
#define USER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define MAX_USERS        16
#define USERNAME_MAX_LEN 32
#define HASH_HEX_LEN     65
#define SALT_HEX_LEN     17

#define UID_ROOT         0
#define UID_FIRST_USER   1000

typedef struct {
    uint32_t uid;
    uint32_t gid;
    char username[USERNAME_MAX_LEN];
    char password_hash[HASH_HEX_LEN]; // Hex-encoded SHA-256
    char salt[SALT_HEX_LEN];          // Hex-encoded 8-byte salt
    char home[64];
    char shell[32];
    bool active;
} user_account_t;

void user_init(void);
user_account_t *user_get_current(void);
user_account_t *user_find_by_name(const char *username);
user_account_t *user_find_by_uid(uint32_t uid);

bool user_authenticate(const char *username, const char *password);
bool user_login(const char *username, const char *password);
bool user_su(const char *username, const char *password);
void user_logout(void);

bool user_add(const char *username, const char *password, uint32_t uid, uint32_t gid);
bool user_set_password(const char *username, const char *new_password);

// Low-Level SHA-256 Hashing API
void sha256_hash_string(const char *input, const char *salt, char *output_hex);

#endif // USER_H