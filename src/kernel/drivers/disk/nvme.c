// nvme.c - Production-grade NVMe Driver implementation
#include "nvme.h"
#include "../pci/pci.h"
#include "../serial/serial.h"
#include "../../core/mem/memory.h"
#include "../../core/mem/pmm.h"
#include "../../core/mem/vmm.h"
#include "../../core/gen/io.h"
#include "string.h"
#include "stdio.h"
#include "block.h"

static nvme_controller_t nvme_ctrl = {0};

static int nvme_bdev_read(uint64_t lba, uint32_t count, void *buffer) {
    return nvme_read_sectors(lba, count, buffer) == NVME_SUCCESS ? 0 : -1;
}

static int nvme_bdev_write(uint64_t lba, uint32_t count, void *buffer) {
    return nvme_write_sectors(lba, count, buffer) == NVME_SUCCESS ? 0 : -1;
}

block_device_t nvme_get_block_device(void) {
    block_device_t dev;
    dev.read = nvme_bdev_read;
    dev.write = nvme_bdev_write;
    dev.sector_size = nvme_ctrl.sector_size ? nvme_ctrl.sector_size : 512;
    return dev;
}

static inline uint32_t nvme_read32(nvme_controller_t *ctrl, uint32_t offset) {
    return *((volatile uint32_t *)(ctrl->bar0 + offset));
}

static inline void nvme_write32(nvme_controller_t *ctrl, uint32_t offset, uint32_t value) {
    *((volatile uint32_t *)(ctrl->bar0 + offset)) = value;
}

static inline uint64_t nvme_read64(nvme_controller_t *ctrl, uint32_t offset) {
    return *((volatile uint64_t *)(ctrl->bar0 + offset));
}

static inline void nvme_write64(nvme_controller_t *ctrl, uint32_t offset, uint64_t value) {
    *((volatile uint64_t *)(ctrl->bar0 + offset)) = value;
}

static uint64_t nvme_get_bar0(uint8_t bus, uint8_t slot, uint8_t func) {
    uint32_t bar0_low = pci_read_dword(bus, slot, func, 0x10);
    
    if (bar0_low & 1) { // Check if IO Space
        serial_puts(COM1, "[NVME ERROR] BAR0 is I/O space, expected MMIO!\n");
        return 0;
    }

    uint8_t type = (bar0_low >> 1) & 0x03;
    uint64_t bar0_phys = bar0_low & ~0xFULL;

    if (type == 0x02) { // 64-bit BAR
        uint32_t bar0_high = pci_read_dword(bus, slot, func, 0x14);
        bar0_phys |= ((uint64_t)bar0_high << 32);
    }

    return bar0_phys;
}

static int nvme_find_controller(uint8_t *out_bus, uint8_t *out_slot, uint8_t *out_func) {
    serial_puts(COM1, "[NVME] Scanning PCI bus topology for NVMe storage...\n");

    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint32_t vendor_device = pci_read_dword((uint8_t)bus, slot, func, 0x00);
                if ((vendor_device & 0xFFFF) == 0xFFFF) continue;

                uint32_t class_reg = pci_read_dword((uint8_t)bus, slot, func, 0x08);
                uint8_t class_code = (class_reg >> 24) & 0xFF;
                uint8_t subclass   = (class_reg >> 16) & 0xFF;
                uint8_t prog_if    = (class_reg >> 8)  & 0xFF;

                if (class_code == 0x01 && subclass == 0x08 && prog_if == 0x02) {
                    *out_bus = (uint8_t)bus;
                    *out_slot = slot;
                    *out_func = func;
                    serial_puts(COM1, "[NVME] MATCH FOUND! NVMe Controller located.\n");
                    return 1;
                }
            }
        }
    }
    serial_puts(COM1, "[NVME] WARNING: No NVMe controller found on PCIe.\n");
    return 0;
}

static int nvme_init_queue_pair(nvme_queue_pair_t *qp, uint16_t sq_size, uint16_t cq_size) {
    size_t sq_bytes = sq_size * sizeof(nvme_sq_entry_t);
    size_t cq_bytes = cq_size * sizeof(nvme_cq_entry_t);

    size_t sq_pages = (sq_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    size_t cq_pages = (cq_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    void *sq_virt = pmm_alloc_continuous(sq_pages);
    void *cq_virt = pmm_alloc_continuous(cq_pages);

    if (!sq_virt || !cq_virt) {
        serial_puts(COM1, "[NVME] Out of physical memory for queue pair allocation!\n");
        return NVME_ERR_NOMEM;
    }

    qp->sq = (nvme_sq_entry_t *)VIRT((uint64_t)sq_virt);
    qp->cq = (nvme_cq_entry_t *)VIRT((uint64_t)cq_virt);
    
    memset(qp->sq, 0, sq_bytes);
    memset(qp->cq, 0, cq_bytes);

    qp->sq_phys = (uint64_t)sq_virt;
    qp->cq_phys = (uint64_t)cq_virt;

    qp->sq_size = sq_size;
    qp->cq_size = cq_size;
    qp->sq_tail = 0;
    qp->cq_head = 0;
    qp->cq_phase = 1;

    return NVME_SUCCESS;
}

static inline uint32_t nvme_sq_doorbell_offset(nvme_controller_t *ctrl, uint16_t qid) {
    return 0x1000 + (2 * qid) * ctrl->db_stride;
}

static inline uint32_t nvme_cq_doorbell_offset(nvme_controller_t *ctrl, uint16_t qid) {
    return 0x1000 + (2 * qid + 1) * ctrl->db_stride;
}

static void nvme_submit_command(nvme_controller_t *ctrl, nvme_queue_pair_t *qp, nvme_sq_entry_t *cmd, uint16_t qid) {
    if (!ctrl->bar0) return;

    uint16_t tail = qp->sq_tail;
    
    qp->sq[tail] = *cmd;

    __asm__ volatile("clflush (%0)" : : "r"(&qp->sq[tail]) : "memory");
    __asm__ volatile("mfence" ::: "memory");

    qp->sq_tail = (qp->sq_tail + 1) % qp->sq_size;

    nvme_write32(ctrl, nvme_sq_doorbell_offset(ctrl, qid), qp->sq_tail);
}

static int nvme_wait_completion(nvme_controller_t *ctrl, nvme_queue_pair_t *qp, uint16_t cid, uint16_t qid) {
    if (!ctrl->bar0) return NVME_ERR_HARDWARE;

    uint32_t timeout = 10000000;

    while (timeout--) {
        volatile nvme_cq_entry_t *cqe = &qp->cq[qp->cq_head];

        // FIX FOR REAL HARDWARE: Invalidate L1 CPU cache line for CQE to fetch DMA updates from RAM
        __asm__ volatile("clflush (%0)" : : "r"((void *)cqe) : "memory");
        __asm__ volatile("mfence" ::: "memory");

        uint16_t cqe_status = cqe->status;
        uint8_t phase = (uint8_t)(cqe_status & 1);

        if (phase == qp->cq_phase) {
            if (cqe->cid == cid) {
                uint16_t status = (uint16_t)((cqe_status >> 1) & 0x7FF);

                qp->cq_head = (qp->cq_head + 1) % qp->cq_size;
                if (qp->cq_head == 0) {
                    qp->cq_phase = !qp->cq_phase;
                }

                nvme_write32(ctrl, nvme_cq_doorbell_offset(ctrl, qid), qp->cq_head);

                if (status != 0) {
                    serial_puts(COM1, "[NVME] Command completed with status error code!\n");
                    return NVME_ERR_HARDWARE;
                }

                return NVME_SUCCESS;
            }
        }
    }
    
    serial_puts(COM1, "[NVME] Command completion timeout!\n");
    return NVME_ERR_TIMEOUT;
}

static int nvme_setup_prp(nvme_sq_entry_t *cmd, uint64_t dma_phys, size_t bytes) {
    size_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    cmd->prp1 = dma_phys;

    if (pages == 1) {
        cmd->prp2 = 0;
    } else if (pages == 2) {
        cmd->prp2 = dma_phys + PAGE_SIZE;
    } else {
        void *prp_list_virt = pmm_alloc_continuous(1);
        if (!prp_list_virt) return NVME_ERR_NOMEM;
        uint64_t prp_list_phys = (uint64_t)prp_list_virt;

        uint64_t *prp_entries = (uint64_t *)VIRT(prp_list_phys);
        for (size_t i = 1; i < pages; i++) {
            prp_entries[i - 1] = dma_phys + (i * PAGE_SIZE);
        }

        cmd->prp2 = prp_list_phys;
    }
    return NVME_SUCCESS;
}

static int nvme_identify_controller(nvme_controller_t *ctrl) {
    void *buf_virt = pmm_alloc_continuous(1);
    if (!buf_virt) return NVME_ERR_NOMEM;
    uint64_t buf_phys = (uint64_t)buf_virt;
    memset((void *)VIRT(buf_phys), 0, 4096);

    nvme_sq_entry_t cmd;
    memset(&cmd, 0, sizeof(cmd));

    cmd.cdw0 = NVME_ADMIN_IDENTIFY | (ctrl->command_id++ << 16);
    cmd.nsid = 0;
    cmd.prp1 = buf_phys;
    cmd.cdw10 = NVME_IDENTIFY_CONTROLLER;

    nvme_submit_command(ctrl, &ctrl->admin_queue, &cmd, 0);
    return nvme_wait_completion(ctrl, &ctrl->admin_queue, (uint16_t)(cmd.cdw0 >> 16), 0);
}

static int nvme_identify_namespace(nvme_controller_t *ctrl) {
    void *buf_virt = pmm_alloc_continuous(1);
    if (!buf_virt) return NVME_ERR_NOMEM;
    uint64_t buf_phys = (uint64_t)buf_virt;
    memset((void *)VIRT(buf_phys), 0, 4096);

    nvme_sq_entry_t cmd;
    memset(&cmd, 0, sizeof(cmd));

    cmd.cdw0 = NVME_ADMIN_IDENTIFY | (ctrl->command_id++ << 16);
    cmd.nsid = 1; // Primary Namespace
    cmd.prp1 = buf_phys;
    cmd.cdw10 = 0x00; // Identify Namespace struct

    nvme_submit_command(ctrl, &ctrl->admin_queue, &cmd, 0);
    int status = nvme_wait_completion(ctrl, &ctrl->admin_queue, (uint16_t)(cmd.cdw0 >> 16), 0);

    if (status == NVME_SUCCESS) {
        uint8_t *ns_data = (uint8_t *)VIRT(buf_phys);
        uint8_t flbas = ns_data[26];
        uint8_t lba_format_idx = flbas & 0x0F;
        
        uint32_t lba_format = *(uint32_t *)&ns_data[128 + (lba_format_idx * 4)];
        uint8_t lbads = (lba_format >> 16) & 0xFF;

        if (lbads >= 9) {
            ctrl->sector_size = (1U << lbads);
        } else {
            ctrl->sector_size = 512;
        }
    } else {
        ctrl->sector_size = 512;
    }

    return status;
}

static int nvme_create_io_cq(nvme_controller_t *ctrl) {
    if (nvme_init_queue_pair(&ctrl->io_queue, NVME_IO_QUEUE_SIZE, NVME_IO_QUEUE_SIZE) != NVME_SUCCESS) {
        return NVME_ERR_NOMEM;
    }

    nvme_sq_entry_t cmd;
    memset(&cmd, 0, sizeof(cmd));

    cmd.cdw0 = NVME_ADMIN_CREATE_CQ | (ctrl->command_id++ << 16);
    cmd.prp1 = ctrl->io_queue.cq_phys;
    cmd.cdw10 = (1 | ((NVME_IO_QUEUE_SIZE - 1) << 16));
    cmd.cdw11 = 1; 

    nvme_submit_command(ctrl, &ctrl->admin_queue, &cmd, 0);
    return nvme_wait_completion(ctrl, &ctrl->admin_queue, (uint16_t)(cmd.cdw0 >> 16), 0);
}

static int nvme_create_io_sq(nvme_controller_t *ctrl) {
    nvme_sq_entry_t cmd;
    memset(&cmd, 0, sizeof(cmd));

    cmd.cdw0 = NVME_ADMIN_CREATE_SQ | (ctrl->command_id++ << 16);
    cmd.prp1 = ctrl->io_queue.sq_phys;
    cmd.cdw10 = (1 | ((NVME_IO_QUEUE_SIZE - 1) << 16));
    cmd.cdw11 = (1 | (1 << 16));

    nvme_submit_command(ctrl, &ctrl->admin_queue, &cmd, 0);
    return nvme_wait_completion(ctrl, &ctrl->admin_queue, (uint16_t)(cmd.cdw0 >> 16), 0);
}

int nvme_read_sectors(uint64_t lba, uint32_t sector_count, void *buffer) {
    if (!nvme_ctrl.bar0) return NVME_ERR_HARDWARE;
    if (sector_count == 0) return NVME_SUCCESS;

    uint32_t sec_size = nvme_ctrl.sector_size ? nvme_ctrl.sector_size : 512;
    size_t transfer_bytes = (size_t)sector_count * sec_size;
    size_t pages_needed = (transfer_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    void *dma_virt = pmm_alloc_continuous(pages_needed);
    if (!dma_virt) return NVME_ERR_NOMEM;
    uint64_t dma_phys = (uint64_t)dma_virt;

    nvme_sq_entry_t cmd;
    memset(&cmd, 0, sizeof(cmd));

    uint16_t cid = nvme_ctrl.command_id++;
    cmd.cdw0 = NVME_NVM_CMD_READ | (cid << 16);
    cmd.nsid = 1;

    void *prp_list_phys = NULL;
    if (pages_needed == 1) {
        cmd.prp1 = dma_phys;
        cmd.prp2 = 0;
    } else if (pages_needed == 2) {
        cmd.prp1 = dma_phys;
        cmd.prp2 = dma_phys + PAGE_SIZE;
    } else {
        void *prp_list_virt = pmm_alloc_continuous(1);
        if (!prp_list_virt) {
            size_t order = 0;
            while ((1ULL << order) < pages_needed) order++;
            pmm_free_pages(dma_virt, order);
            return NVME_ERR_NOMEM;
        }
        prp_list_phys = prp_list_virt;
        uint64_t *prp_entries = (uint64_t *)VIRT((uint64_t)prp_list_virt);
        for (size_t i = 1; i < pages_needed; i++) {
            prp_entries[i - 1] = dma_phys + (i * PAGE_SIZE);
        }
        cmd.prp1 = dma_phys;
        cmd.prp2 = (uint64_t)prp_list_virt;
    }

    cmd.cdw10 = (uint32_t)lba;
    cmd.cdw11 = (uint32_t)(lba >> 32);
    cmd.cdw12 = (sector_count - 1) & 0xFFFF;

    nvme_submit_command(&nvme_ctrl, &nvme_ctrl.io_queue, &cmd, 1);
    int err = nvme_wait_completion(&nvme_ctrl, &nvme_ctrl.io_queue, cid, 1);

    if (err == NVME_SUCCESS) {
        memcpy(buffer, (void *)VIRT(dma_phys), transfer_bytes);
    }

    // FIX: Free allocated DMA and PRP physical pages
    size_t order = 0;
    while ((1ULL << order) < pages_needed) {
        order++;
    }
    pmm_free_pages(dma_virt, order);

    if (prp_list_phys) {
        pmm_free_pages(prp_list_phys, 0);
    }

    return err;
}

int nvme_write_sectors(uint64_t lba, uint32_t sector_count, void *buffer) {
    if (!nvme_ctrl.bar0) return NVME_ERR_HARDWARE;
    if (sector_count == 0) return NVME_SUCCESS;

    uint32_t sec_size = nvme_ctrl.sector_size ? nvme_ctrl.sector_size : 512;
    size_t transfer_bytes = (size_t)sector_count * sec_size;
    size_t pages_needed = (transfer_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    void *dma_virt = pmm_alloc_continuous(pages_needed);
    if (!dma_virt) return NVME_ERR_NOMEM;
    uint64_t dma_phys = (uint64_t)dma_virt;

    memcpy((void *)VIRT(dma_phys), buffer, transfer_bytes);

    nvme_sq_entry_t cmd;
    memset(&cmd, 0, sizeof(cmd));

    uint16_t cid = nvme_ctrl.command_id++;
    cmd.cdw0 = NVME_NVM_CMD_WRITE | (cid << 16);
    cmd.nsid = 1;

    void *prp_list_phys = NULL;
    if (pages_needed == 1) {
        cmd.prp1 = dma_phys;
        cmd.prp2 = 0;
    } else if (pages_needed == 2) {
        cmd.prp1 = dma_phys;
        cmd.prp2 = dma_phys + PAGE_SIZE;
    } else {
        void *prp_list_virt = pmm_alloc_continuous(1);
        if (!prp_list_virt) {
            size_t order = 0;
            while ((1ULL << order) < pages_needed) order++;
            pmm_free_pages(dma_virt, order);
            return NVME_ERR_NOMEM;
        }
        prp_list_phys = prp_list_virt;
        uint64_t *prp_entries = (uint64_t *)VIRT((uint64_t)prp_list_virt);
        for (size_t i = 1; i < pages_needed; i++) {
            prp_entries[i - 1] = dma_phys + (i * PAGE_SIZE);
        }
        cmd.prp1 = dma_phys;
        cmd.prp2 = (uint64_t)prp_list_virt;
    }

    cmd.cdw10 = (uint32_t)lba;
    cmd.cdw11 = (uint32_t)(lba >> 32);
    cmd.cdw12 = (sector_count - 1) & 0xFFFF;

    nvme_submit_command(&nvme_ctrl, &nvme_ctrl.io_queue, &cmd, 1);
    int err = nvme_wait_completion(&nvme_ctrl, &nvme_ctrl.io_queue, cid, 1);

    // FIX: Free allocated DMA and PRP physical pages
    size_t order = 0;
    while ((1ULL << order) < pages_needed) {
        order++;
    }
    pmm_free_pages(dma_virt, order);

    if (prp_list_phys) {
        pmm_free_pages(prp_list_phys, 0);
    }

    return err;
}

int nvme_init(void) {
    if (nvme_ctrl.initialized) {
        return NVME_SUCCESS;
    }

    uint8_t bus, slot, func;
    if (!nvme_find_controller(&bus, &slot, &func)) {
        return NVME_ERR_NOTFOUND;
    }

    uint32_t pci_cmd = pci_read_dword(bus, slot, func, 0x04);
    pci_cmd |= (1 << 1) | (1 << 2);
    pci_write_word(bus, slot, func, 0x04, (uint16_t)pci_cmd);

    uint64_t bar0_phys = nvme_get_bar0(bus, slot, func);
    if (!bar0_phys) {
        return NVME_ERR_HARDWARE;
    }

    nvme_ctrl.bar0 = (volatile uint8_t *)pci_map_mmio(bar0_phys, 0x4000);
    if (!nvme_ctrl.bar0) {
        return NVME_ERR_HARDWARE;
    }

    uint64_t cap = nvme_read64(&nvme_ctrl, NVME_REG_CAP);
    uint32_t dstrd = (uint32_t)((cap >> 32) & 0xF);
    nvme_ctrl.db_stride = 4 << dstrd;

    uint32_t cc = nvme_read32(&nvme_ctrl, NVME_REG_CC);
    cc &= ~NVME_CC_ENABLE;
    nvme_write32(&nvme_ctrl, NVME_REG_CC, cc);

    uint32_t timeout = 5000000;
    while ((nvme_read32(&nvme_ctrl, NVME_REG_CSTS) & NVME_CSTS_RDY) && --timeout);
    if (timeout == 0) return NVME_ERR_TIMEOUT;

    if (nvme_init_queue_pair(&nvme_ctrl.admin_queue, NVME_ADMIN_QUEUE_SIZE, NVME_ADMIN_QUEUE_SIZE) != NVME_SUCCESS) {
        return NVME_ERR_NOMEM;
    }

    nvme_write64(&nvme_ctrl, NVME_REG_ASQ, nvme_ctrl.admin_queue.sq_phys);
    nvme_write64(&nvme_ctrl, NVME_REG_ACQ, nvme_ctrl.admin_queue.cq_phys);

    uint32_t aqa = ((NVME_ADMIN_QUEUE_SIZE - 1) << 16) | (NVME_ADMIN_QUEUE_SIZE - 1);
    nvme_write32(&nvme_ctrl, NVME_REG_AQA, aqa);

    cc = NVME_CC_ENABLE | NVME_CC_CSS_NVM | NVME_CC_AMS_RR | NVME_CC_SHN_NONE | NVME_CC_IOSQES | NVME_CC_IOCQES;
    nvme_write32(&nvme_ctrl, NVME_REG_CC, cc);

    timeout = 5000000;
    while (!(nvme_read32(&nvme_ctrl, NVME_REG_CSTS) & NVME_CSTS_RDY) && --timeout);
    if (timeout == 0) return NVME_ERR_TIMEOUT;

    nvme_ctrl.command_id = 0;

    if (nvme_identify_controller(&nvme_ctrl) != NVME_SUCCESS) {
        return NVME_ERR_HARDWARE;
    }

    nvme_identify_namespace(&nvme_ctrl);

    if (nvme_create_io_cq(&nvme_ctrl) != NVME_SUCCESS) return NVME_ERR_HARDWARE;
    if (nvme_create_io_sq(&nvme_ctrl) != NVME_SUCCESS) return NVME_ERR_HARDWARE;

    nvme_ctrl.initialized = true;
    return NVME_SUCCESS;
}