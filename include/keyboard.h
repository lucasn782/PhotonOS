#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>

/* Keyboard state flags */
extern int keyboard_shift;
extern int keyboard_ctrl;
extern int keyboard_extended;
extern int keyboard_altgr;

#define KEYBOARD_QUEUE_SIZE 256
#define KEYBOARD_STATUS_PORT 0x64
#define KEYBOARD_DATA_PORT 0x60
#define KEYBOARD_OUTPUT_FULL 0x01
#define IRQ_KEYBOARD_VECTOR 0x21

/* Public keyboard driver functions */
void keyboard_handle_scancode(uint8_t scancode);
char keyboard_char_from_scancode(uint8_t scancode);
void keyboard_queue_push(char ch);
void keyboard_init(void);
void keyboard_irq_handler(void);
void keyboard_flush(void);

#endif /* KEYBOARD_H */
