#include "apic.h"
#include "vmm.h"
#include "memory.h"

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline void io_wait(void) {
    outb(0x80, 0);
}

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t low;
    uint32_t high;
    __asm__ volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

static uintptr_t lapic_base_addr = 0xFEE00000ULL;
static int apic_enabled_flag = 0;

uint32_t apic_read(uint32_t reg) {
    volatile uint32_t *apic = (volatile uint32_t *)(lapic_base_addr + reg);
    return *apic;
}

void apic_write(uint32_t reg, uint32_t value) {
    volatile uint32_t *apic = (volatile uint32_t *)(lapic_base_addr + reg);
    *apic = value;
}

void apic_init(void) {
    // 1. Disable legacy PIC by masking all interrupts (sending 0xFF to 0x21 and 0xA1)
    outb(0x21, 0xFF);
    io_wait();
    outb(0xA1, 0xFF);
    io_wait();

    // 2. Get Local APIC Base Address from MSR 0x1B
    uint64_t apic_base_msr = rdmsr(0x1B);
    lapic_base_addr = apic_base_msr & 0xFFFFF000ULL;

    // 3. Map LAPIC Base region as PAGE_PRESENT | PAGE_WRITABLE | PAGE_CACHE_DISABLE
    vmm_map(lapic_base_addr, lapic_base_addr, PAGE_PRESENT | PAGE_WRITABLE | PAGE_CACHE_DISABLE);

    // 4. Enable APIC by setting bit 8 in SIVR (offset 0xF0) and mapping vector 0xFF
    uint32_t sivr = apic_read(APIC_REG_SIVR);
    apic_write(APIC_REG_SIVR, sivr | 0x100 | 0xFF);

    apic_enabled_flag = 1;
}

void apic_init_ap(void) {
    // Enable APIC on secondary cores
    uint32_t sivr = apic_read(APIC_REG_SIVR);
    apic_write(APIC_REG_SIVR, sivr | 0x100 | 0xFF);
}

int apic_is_enabled(void) {
    return apic_enabled_flag;
}

void apic_eoi(void) {
    apic_write(APIC_REG_EOI, 0);
}
