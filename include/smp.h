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

#endif
