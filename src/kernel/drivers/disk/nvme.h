#ifndef NVME_H
#define NVME_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "block.h"

#define NVME_SUCCESS        0
#define NVME_ERR_NOMEM      -1
#define NVME_ERR_TIMEOUT    -2
#define NVME_ERR_HARDWARE   -3
#define NVME_ERR_NOTFOUND   -4

#define NVME_REG_CAP    0x00
#define NVME_REG_VS     0x08
#define NVME_REG_CC     0x14
#define NVME_REG_CSTS   0x1C
#define NVME_REG_AQA    0x24
#define NVME_REG_ASQ    0x28
#define NVME_REG_ACQ    0x30

#define NVME_CC_ENABLE     (1 << 0)
#define NVME_CC_CSS_NVM    (0 << 4)
#define NVME_CC_AMS_RR     (0 << 11)
#define NVME_CC_SHN_NONE   (0 << 14)
#define NVME_CC_IOSQES     (6 << 16)
#define NVME_CC_IOCQES     (4 << 20)

#define NVME_CSTS_RDY      (1 << 0)

#define NVME_ADMIN_QUEUE_SIZE 64
#define NVME_IO_QUEUE_SIZE    1024

#define NVME_ADMIN_CREATE_SQ    0x01
#define NVME_ADMIN_CREATE_CQ    0x05
#define NVME_ADMIN_IDENTIFY     0x06

#define NVME_IDENTIFY_CONTROLLER 0x01
#define NVME_NVM_CMD_READ    0x02
#define NVME_NVM_CMD_WRITE   0x01

typedef struct {
    uint32_t cdw0;
    uint32_t nsid;
    uint64_t reserved;
    uint64_t mptr;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
} __attribute__((packed)) nvme_sq_entry_t;

typedef struct {
    uint32_t dw0;
    uint32_t dw1;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t cid;
    uint16_t status;
} __attribute__((packed)) nvme_cq_entry_t;

typedef struct {
    nvme_sq_entry_t *sq;
    nvme_cq_entry_t *cq;
    uint64_t sq_phys;
    uint64_t cq_phys;
    uint16_t sq_tail;
    uint16_t cq_head;
    uint16_t sq_size;
    uint16_t cq_size;
    uint8_t  cq_phase;
} nvme_queue_pair_t;

typedef struct {
    volatile uint8_t *bar0;
    nvme_queue_pair_t admin_queue;
    nvme_queue_pair_t io_queue;
    uint32_t num_namespaces;
    uint16_t command_id;
    uint32_t db_stride;
    bool initialized; // FIX: Added initialization state tracking for idempotency
} nvme_controller_t;

int nvme_init(void);
int nvme_read_sectors(uint64_t lba, uint32_t sector_count, void *buffer);
int nvme_write_sectors(uint64_t lba, uint32_t sector_count, void *buffer);
block_device_t nvme_get_block_device(void);

#endif // NVME_H