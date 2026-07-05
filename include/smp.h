#ifndef PHOTONOS_SMP_H
#define PHOTONOS_SMP_H

#include <stdint.h>

typedef struct spinlock {
    volatile int locked;
} spinlock_t;

void spin_init(spinlock_t *lock);
void spin_lock(spinlock_t *lock);
void spin_unlock(spinlock_t *lock);

void smp_init(void);
void smp_boot_ap(uint8_t ap_id);
void ap_kmain(uint64_t ap_id);
int smp_ap_booted_count(void);
void smp_tlb_shootdown_handler(void);

extern volatile uint64_t tlb_acknowledge_count;
extern volatile uintptr_t tlb_shootdown_addr;

#endif
