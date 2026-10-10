// src/kernel/ipc/shm.h - System V Shared Memory Subsystem for X11 MIT-SHM
#ifndef SHM_H
#define SHM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define IPC_PRIVATE 0
#define IPC_CREAT   01000
#define IPC_EXCL    02000
#define IPC_RMID    0
#define IPC_STAT    2

#define SHM_RDONLY  010000
#define SHM_MAX_SEGMENTS 32
#define SHM_MAX_SIZE     (64 * 1024 * 1024) // 64MB max per segment

#define IPC_SET     1
#define IPC_INFO    3
#define IPC_64      0x100   // glibc/musl OR this into every *ctl command

// Linux x86-64 struct shmid_ds (struct ipc64_perm + segment info), 112 bytes
struct shmid_ds {
    int32_t  shm_perm_key;
    uint32_t shm_perm_uid;
    uint32_t shm_perm_gid;
    uint32_t shm_perm_cuid;
    uint32_t shm_perm_cgid;
    uint32_t shm_perm_mode;
    uint16_t shm_perm_seq;
    uint16_t shm_perm_pad;
    uint64_t shm_perm_unused1;
    uint64_t shm_perm_unused2;
    uint64_t shm_segsz;
    int64_t  shm_atime;
    int64_t  shm_dtime;
    int64_t  shm_ctime;
    int32_t  shm_cpid;
    int32_t  shm_lpid;
    uint64_t shm_nattch;
    uint64_t shm_unused4;
    uint64_t shm_unused5;
};

void shm_init(void);

int64_t sys_shmget_handler(uint64_t key, size_t size, int shmflg);
int64_t sys_shmat_handler(int shmid, uint64_t shmaddr, int shmflg);
int64_t sys_shmdt_handler(uint64_t shmaddr);
int64_t sys_shmctl_handler(int shmid, int cmd, void *buf);

#endif // SHM_H