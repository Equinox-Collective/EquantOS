// src/kernel/drivers/net/rtl8139.c - Realtek RTL8139 PCI Network Driver for EquantOS
#include "rtl8139.h"
#include "../../net/net.h"
#include "../pci/pci.h"
#include "../../core/gen/io.h"
#include "../../core/mem/pmm.h"
#include "../../core/mem/vmm.h"
#include "../../core/initcall.h"
#include "../../drivers/serial/serial.h"
#include "string.h"
#include "stdio.h"

#define REG_MAC         0x00
#define REG_TSD0        0x10
#define REG_TSAD0       0x20
#define REG_RBSTART     0x30
#define REG_COMMAND     0x37
#define REG_CAPR        0x38
#define REG_IMR         0x3C
#define REG_ISR         0x3E
#define REG_TCR         0x40
#define REG_RCR         0x44
#define REG_CONFIG1     0x52

#define RX_BUFFER_SIZE  16384
#define RX_WRAP_PAD     1500
#define RCR_RBLEN_16K   (1u << 11)

static uint32_t rtl_io_base = 0;
static int tx_cur_desc = 0;
static uint64_t rx_phys_addr = 0;
static uint64_t tx_phys_addr = 0;
static uint16_t rx_offset = 0;
static bool rtl_initialized = false;

static net_interface_t rtl_iface;

void rtl8139_send_packet(void *data, uint32_t len) {
    if (!rtl_initialized) return;

    uint32_t send_len = (len < 60) ? 60 : len;
    uint64_t current_tx_phys = tx_phys_addr + (tx_cur_desc * 512);
    uint8_t *current_tx_virt = (uint8_t *)VIRT(current_tx_phys);

    memcpy(current_tx_virt, data, len);
    if (len < 60) {
        memset(current_tx_virt + len, 0, 60 - len);
    }

    __asm__ volatile("" : : : "memory");

    uint32_t tsad_reg = REG_TSAD0 + (tx_cur_desc * 4);
    uint32_t tsd_reg  = REG_TSD0  + (tx_cur_desc * 4);

    outl(rtl_io_base + tsad_reg, (uint32_t)current_tx_phys);
    outl(rtl_io_base + tsd_reg, send_len & 0x00001FFF);

    tx_cur_desc = (tx_cur_desc + 1) % 4;
}

void rtl8139_poll(void) {
    if (!rtl_initialized) return;

    uint16_t isr_status = inw(rtl_io_base + REG_ISR);
    if (!(isr_status & 0x01)) return;

    outw(rtl_io_base + REG_ISR, 0x05);

    uint8_t *rx_virt = (uint8_t *)VIRT(rx_phys_addr);

    while (!(inb(rtl_io_base + REG_COMMAND) & 0x01)) {
        uint16_t *header = (uint16_t *)(rx_virt + rx_offset);
        uint16_t status  = header[0];
        uint16_t length  = header[1];

        if (!(status & 0x01) || length < 20 || length > 1518 + 4) {
            serial_puts(COM1, "[RTL8139] Packet desync, resetting RX ring...\n");
            outb(rtl_io_base + REG_COMMAND, 0x10);
            while ((inb(rtl_io_base + REG_COMMAND) & 0x10) != 0);
            outl(rtl_io_base + REG_RBSTART, (uint32_t)rx_phys_addr);
            outl(rtl_io_base + REG_RCR, 0x0F | (1 << 7) | RCR_RBLEN_16K);
            outb(rtl_io_base + REG_COMMAND, 0x0C);
            rx_offset = 0;
            return;
        }

        uint8_t *packet = rx_virt + rx_offset + 4;
        net_handle_packet(&rtl_iface, packet, length - 4);

        rx_offset = (rx_offset + length + 4 + 3) & ~3;
        if (rx_offset >= RX_BUFFER_SIZE) {
            rx_offset -= RX_BUFFER_SIZE;
        }

        uint16_t capr = (uint16_t)((rx_offset + RX_BUFFER_SIZE - 16) % RX_BUFFER_SIZE);
        outw(rtl_io_base + REG_CAPR, capr);
    }
}

static int rtl8139_probe(pci_device_t *dev) {
    serial_puts(COM1, "[RTL8139] Matching device detected on PCI bus.\n");

    // Enable PCI Bus Mastering & I/O Space (Command Register at offset 0x04)
    uint32_t pci_cmd = pci_read_dword(dev->bus, dev->slot, dev->func, 0x04);
    pci_cmd |= (1 << 0) | (1 << 2); // IO Space Enable + Bus Master Enable
    pci_write_word(dev->bus, dev->slot, dev->func, 0x04, (uint16_t)pci_cmd);

    // Read BAR0 (I/O Base Address)
    uint32_t bar0 = pci_read_dword(dev->bus, dev->slot, dev->func, 0x10);
    if (!(bar0 & 0x01)) {
        serial_puts(COM1, "[RTL8139] Error: BAR0 is not I/O space!\n");
        return -1;
    }
    rtl_io_base = bar0 & ~0x03;

    // Allocate continuous physical DMA memory via Buddy Allocator
    // RX requires 16KB + 1.5KB wrap pad = ~18KB -> 5 pages (20KB, order 3)
    void *rx_page = pmm_alloc_continuous(5);
    if (!rx_page) {
        serial_puts(COM1, "[RTL8139] Failed to allocate DMA RX buffer!\n");
        return -1;
    }
    rx_phys_addr = (uint64_t)rx_page;
    memset((void *)VIRT(rx_phys_addr), 0, RX_BUFFER_SIZE + RX_WRAP_PAD);

    // TX requires 4 * 512B = 2KB -> 1 page (order 0)
    void *tx_page = pmm_alloc();
    if (!tx_page) {
        serial_puts(COM1, "[RTL8139] Failed to allocate DMA TX buffer!\n");
        return -1;
    }
    tx_phys_addr = (uint64_t)tx_page;
    memset((void *)VIRT(tx_phys_addr), 0, 4096);

    // Hardware Reset
    outb(rtl_io_base + REG_CONFIG1, 0x00);
    outb(rtl_io_base + REG_COMMAND, 0x10); // Reset bit
    while ((inb(rtl_io_base + REG_COMMAND) & 0x10) != 0);

    // Set Receive Buffer Start
    outl(rtl_io_base + REG_RBSTART, (uint32_t)rx_phys_addr);

    // RCR: Accept Broadcast, Multicast, My-Physical, Wrap mode, 16K Buffer
    outl(rtl_io_base + REG_RCR, 0x0000000F | (1 << 7) | RCR_RBLEN_16K);

    // Enable Transmitter and Receiver
    outb(rtl_io_base + REG_COMMAND, 0x0C);

    // Standard TCR
    outl(rtl_io_base + REG_TCR, 0x03000000);

    // Enable ROK (Receive OK) and TOK (Transmit OK) interrupts
    outw(rtl_io_base + REG_IMR, 0x0005);

    // Register interface in Network Subsystem
    memset(&rtl_iface, 0, sizeof(net_interface_t));
    strncpy(rtl_iface.name, "eth0", sizeof(rtl_iface.name) - 1);
    for (int i = 0; i < 6; i++) {
        rtl_iface.mac[i] = inb(rtl_io_base + REG_MAC + i);
    }
    rtl_iface.ip = 0x0A00020F;          // 10.0.2.15 (QEMU default IP)
    rtl_iface.subnet_mask = 0xFFFFFF00; // 255.255.255.0
    rtl_iface.gateway_ip = 0x0A000202;  // 10.0.2.2 (QEMU gateway)
    rtl_iface.send = rtl8139_send_packet;

    net_register_interface(&rtl_iface);
    rtl_initialized = true;

    char mac_str[64];
    snprintf(mac_str, sizeof(mac_str),
             "[RTL8139] Device active (eth0) MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
             rtl_iface.mac[0], rtl_iface.mac[1], rtl_iface.mac[2],
             rtl_iface.mac[3], rtl_iface.mac[4], rtl_iface.mac[5]);
    serial_puts(COM1, mac_str);

    return 0;
}

static pci_device_id_t rtl8139_pci_ids[] = {
    { 0x10EC, 0x8139, 0, 0 }, // Realtek RTL-8139/8139C/8139C+
    { 0, 0, 0, 0 }
};

static pci_driver_t rtl8139_driver = {
    .name = "rtl8139",
    .id_table = rtl8139_pci_ids,
    .probe = rtl8139_probe,
    .remove = NULL,
    .next = NULL
};

static int __init rtl8139_initcall(void) {
    pci_register_driver(&rtl8139_driver);
    return 0;
}
device_initcall(rtl8139_initcall);