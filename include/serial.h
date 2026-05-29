#ifndef PHOTONOS_SERIAL_H
#define PHOTONOS_SERIAL_H

void serial_init(void);
void serial_putc(char c);
void serial_print(char *str);
void klog(char *msg);

#endif
