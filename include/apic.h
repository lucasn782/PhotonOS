#ifndef PHOTONOS_APIC_H
#define PHOTONOS_APIC_H

#include <stdint.h>

#define APIC_REG_ID         0x20
#define APIC_REG_VER        0x30
#define APIC_REG_TPR        0x80
#define APIC_REG_EOI        0xB0
#define APIC_REG_LDR        0xD0
#define APIC_REG_DFR        0xE0
#define APIC_REG_SIVR       0xF0
#define APIC_REG_ESR        0x280
#define APIC_REG_ICR_LOW    0x300
#define APIC_REG_ICR_HIGH   0x310
#define APIC_REG_LVT_TMR    0x320
#define APIC_REG_LVT_PERF   0x340
#define APIC_REG_LVT_LINT0  0x350
#define APIC_REG_LVT_LINT1  0x360
#define APIC_REG_LVT_ERR    0x370

void apic_init(void);
void apic_init_ap(void);
int apic_is_enabled(void);
void apic_eoi(void);
uint32_t apic_read(uint32_t reg);
void apic_write(uint32_t reg, uint32_t value);
static inline uint32_t lapic_read(uint32_t reg) {
    return apic_read(reg);
}

static inline void lapic_write(uint32_t reg, uint32_t value) {
    apic_write(reg, value);
}

#endif
