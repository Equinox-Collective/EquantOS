// src/kernel/misc/user.h - Modular User & Authentication Management
#ifndef USER_H
#define USER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define MAX_USERS         16
#define USER_NAME_MAX     32
#define USER_PASS_MAX     64
#define USER_HASH_LEN     65 // 64 hex chars + null terminator

#define UID_ROOT          0
#define GID_ROOT          0
#define UID_DEFAULT_USER  1000
#define GID_DEFAULT_USER  1000

typedef struct {
    uint32_t uid;
    uint32_t gid;
    char username[USER_NAME_MAX];
    char password_hash[USER_HASH_LEN];
    char home_dir[64];
    char shell_path[32];
    bool active;
} user_account_t;

// Initialization & Core API
void user_subsys_init(void);
user_account_t *user_get_current(void);
user_account_t *user_find_by_name(const char *username);
user_account_t *user_find_by_uid(uint32_t uid);

bool user_authenticate(const char *username, const char *password);
bool user_switch(const char *username, const char *password);
bool user_switch_force(uint32_t uid); // For root/kernel privilege transitions

bool user_add(const char *username, const char *password, uint32_t uid, uint32_t gid);
bool user_set_password(const char *username, const char *new_password);
bool user_is_root(void);

// Password Hashing (Salted SHA-256)
void user_hash_password(const char *password, const char *salt, char *output_hash);

#endif // USER_H