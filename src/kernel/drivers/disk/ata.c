#include "ata.h"
#include "../../core/gen/io.h"

extern void term_print(const char* str);

#define ATA_PRIMARY_DATA         0x1F0
#define ATA_PRIMARY_ERR          0x1F1
#define ATA_PRIMARY_SECCOUNT     0x1F2
#define ATA_PRIMARY_LBA_LOW      0x1F3
#define ATA_PRIMARY_LBA_MID      0x1F4
#define ATA_PRIMARY_LBA_HIGH     0x1F5
#define ATA_PRIMARY_DRIVE_SEL    0x1F6
#define ATA_PRIMARY_COMMAND      0x1F7
#define ATA_PRIMARY_STATUS       0x1F7

// Бит 6 (0x40) - LBA режим, Бит 4 (0x10) - Slave, Бит 7 и 5 - всегда 1
#define ATA_DRIVE_MASTER 0xE0 
#define ATA_DRIVE_SLAVE  0xF0

extern void term_print(const char* str);
int ata_drive_present = 0;

static void ata_400ns_delay() {
    for(int i = 0; i < 4; i++) inb(ATA_PRIMARY_STATUS);
}

static int ata_wait_bsy() {
    uint32_t timeout = 1000000;
    while ((inb(ATA_PRIMARY_STATUS) & 0x80) && --timeout);
    return timeout > 0;
}

static int ata_wait_drq() {
    uint32_t timeout = 1000000;
    while (!(inb(ATA_PRIMARY_STATUS) & 0x08) && --timeout);
    return timeout > 0;
}

void ata_identify() {
    outb(ATA_PRIMARY_DRIVE_SEL, 0xA0);
    outb(ATA_PRIMARY_SECCOUNT, 0);
    outb(ATA_PRIMARY_LBA_LOW, 0);
    outb(ATA_PRIMARY_LBA_MID, 0);
    outb(ATA_PRIMARY_LBA_HIGH, 0);
    outb(ATA_PRIMARY_COMMAND, 0xEC); // Identify command

    uint8_t status = inb(ATA_PRIMARY_STATUS);
    if (status == 0 || status == 0xFF) {
        term_print("ATA: No drive found on Primary Master!\n");
        ata_drive_present = 0; // Диска нет!
        return;
    }

    if (!ata_wait_bsy() || !ata_wait_drq()) {
        term_print("ATA: Drive IDENTIFY timeout or error.\n");
        return;
    }

    uint16_t info[256];
    for (int i = 0; i < 256; i++) {
        info[i] = inw(ATA_PRIMARY_DATA);
    }

    // Модель диска в словах 27-46
    char model[41];
    for (int i = 0; i < 20; i++) {
        model[i*2] = (info[27+i] >> 8) & 0xFF;
        model[i*2+1] = info[27+i] & 0xFF;
    }
    model[40] = '\0';
    ata_drive_present = 1;
    term_print("ATA: Drive found: ");
    term_print(model);
    term_print("\n");
}

static inline void insw(uint16_t port, void* addr, uint32_t count) {
    __asm__ volatile("rep insw" : "+D"(addr), "+c"(count) : "d"(port) : "memory");
}

/* Этап 8: ATA PIO — невытесняемая секция. Два одновременных процесса
 * (пайплайн `ls | grep`: два execve читают ELF с диска) перемежали свои
 * порт-команды и получали мусор («file not found»/«not a valid ELF» и
 * случайные панику/порчу памяти). На UP-ядре достаточно IRQ-off. */
static inline uint64_t ata_irq_save(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    return flags;
}
static inline void ata_irq_restore(uint64_t flags) {
    __asm__ volatile("push %0; popfq" :: "r"(flags) : "memory", "cc");
}

static void read_sectors_ata_pio_unlocked(uintptr_t target_address, uint64_t LBA, uint32_t sector_count) {
    if (sector_count == 0) return;

    while (sector_count > 0) {
        uint8_t chunk = (sector_count > 255) ? 255 : (uint8_t)sector_count;

        if (!ata_wait_bsy()) return;
        
        // Выбор диска и LBA
        outb(ATA_PRIMARY_DRIVE_SEL, 0xE0 | ((LBA >> 24) & 0x0F));
        ata_400ns_delay();
        outb(ATA_PRIMARY_SECCOUNT, chunk);
        outb(ATA_PRIMARY_LBA_LOW,  (uint8_t)LBA);
        outb(ATA_PRIMARY_LBA_MID,  (uint8_t)(LBA >> 8));
        outb(ATA_PRIMARY_LBA_HIGH, (uint8_t)(LBA >> 16));
        outb(ATA_PRIMARY_COMMAND,  0x20); // Read sectors

        for (int j = 0; j < chunk; j++) {
            if (!ata_wait_bsy() || !ata_wait_drq()) return;
            // ВМЕСТО ЦИКЛА FOR — ОДНА ИНСТРУКЦИЯ
            insw(ATA_PRIMARY_DATA, (void*)(target_address), 256);
            target_address += 512;
        }

        sector_count -= chunk;
        LBA += chunk;
    }
}

void read_sectors_ata_pio(uintptr_t target_address, uint64_t LBA, uint32_t sector_count) {
    if (!ata_drive_present) {
        // Если диска нет, не читаем мусор и сразу выходим!
        return; 
    }
    uint64_t f = ata_irq_save();
    read_sectors_ata_pio_unlocked(target_address, LBA, sector_count);
    ata_irq_restore(f);
}

// src/drivers/disk/ata.c
static void write_sectors_ata_pio_unlocked(uintptr_t source_address, uint64_t LBA,
                           uint32_t sector_count) {
  uint16_t *ptr =
      (uint16_t *)source_address; // Кастим адрес к указателю на слова

  while (sector_count > 0) {
    uint8_t chunk = (sector_count > 255) ? 255 : (uint8_t)sector_count;

    if (!ata_wait_bsy())
      return;
    outb(ATA_PRIMARY_DRIVE_SEL, 0xE0 | ((LBA >> 24) & 0x0F));
    ata_400ns_delay();
    outb(ATA_PRIMARY_SECCOUNT, chunk);
    outb(ATA_PRIMARY_LBA_LOW, (uint8_t)LBA);
    outb(ATA_PRIMARY_LBA_MID, (uint8_t)(LBA >> 8));
    outb(ATA_PRIMARY_LBA_HIGH, (uint8_t)(LBA >> 16));
    outb(ATA_PRIMARY_COMMAND, 0x30); // Command: Write sectors

    for (int j = 0; j < chunk; j++) {
      if (!ata_wait_bsy() || !ata_wait_drq())
        return;
      for (int i = 0; i < 256; i++) {
        outw(ATA_PRIMARY_DATA, ptr[i]);
      }
      ptr += 256;
    }

    outb(ATA_PRIMARY_COMMAND, 0xE7); // Cache Flush
    ata_wait_bsy();

    sector_count -= chunk;
    LBA += chunk;
  }
}
void write_sectors_ata_pio(uintptr_t source_address, uint64_t LBA,
                           uint32_t sector_count) {
    uint64_t f = ata_irq_save();
    write_sectors_ata_pio_unlocked(source_address, LBA, sector_count);
    ata_irq_restore(f);
}


// Extended ATA read supporting both Master (0) and Slave (1) on Primary channel with safety timeout
void read_sectors_ata_pio_drive(uint8_t drive, uintptr_t target_address, uint64_t LBA, uint32_t sector_count) {
    if (sector_count == 0) return;
    uint64_t f = ata_irq_save();

    while (sector_count > 0) {
        uint8_t chunk = (sector_count > 255) ? 255 : (uint8_t)sector_count;

        // Safety timeout to prevent infinite freezing if drive is missing or unresponsive
        uint32_t timeout = 1000000;
        while ((inb(0x1F7) & 0x80) && --timeout);
        if (timeout == 0) {
            // Drive not responding
            ata_irq_restore(f);
            return;
        }

        // Select drive (bit 4 = 0 for Master, 1 for Slave) and set LBA bits 24-27
        outb(0x1F6, 0xE0 | (drive << 4) | ((LBA >> 24) & 0x0F));
        outb(0x1F2, chunk);
        outb(0x1F3, (uint8_t)LBA);
        outb(0x1F4, (uint8_t)(LBA >> 8));
        outb(0x1F5, (uint8_t)(LBA >> 16));
        outb(0x1F7, 0x20); // Send READ SECTORS command

        for (int j = 0; j < chunk; j++) {
            timeout = 1000000;
            while (!(inb(0x1F7) & 0x08) && --timeout); // Wait for DRQ with timeout
            if (timeout == 0) {
                ata_irq_restore(f);
                return;
            }
            insw(ATA_PRIMARY_DATA, (void *)target_address, 256);
            target_address += 512;
        }

        sector_count -= chunk;
        LBA += chunk;
    }

    ata_irq_restore(f);
}

static inline void outsw(uint16_t port, const void *addr, uint32_t count) {
    __asm__ volatile("rep outsw" : "+S"(addr), "+c"(count) : "d"(port) : "memory");
}

// Write sectors to ATA drive using PIO mode (supports Master 0 and Slave 1)
void write_sectors_ata_pio_drive(uint8_t drive, uintptr_t src_address, uint64_t LBA, uint32_t sector_count) {
    if (sector_count == 0) return;
    uint64_t f = ata_irq_save();

    while (sector_count > 0) {
        uint8_t chunk = (sector_count > 255) ? 255 : (uint8_t)sector_count;

        while (inb(0x1F7) & 0x80); // Wait until BSY is clear

        outb(0x1F6, 0xE0 | (drive << 4) | ((LBA >> 24) & 0x0F));
        outb(0x1F2, chunk);
        outb(0x1F3, (uint8_t)LBA);
        outb(0x1F4, (uint8_t)(LBA >> 8));
        outb(0x1F5, (uint8_t)(LBA >> 16));
        outb(0x1F7, 0x30); // Send WRITE SECTORS command

        uint16_t *src = (uint16_t *)src_address;
        for (int j = 0; j < chunk; j++) {
            while (!(inb(0x1F7) & 0x08)); // Wait for DRQ
            outsw(0x1F0, src, 256);
            src += 256;
        }

        sector_count -= chunk;
        LBA += chunk;
    }

    ata_irq_restore(f);
}