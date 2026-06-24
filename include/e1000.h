#ifndef PHOTONOS_E1000_H
#define PHOTONOS_E1000_H

#include <stddef.h>
#include <stdint.h>

#define E1000_RX_DESC_COUNT 128U
#define E1000_TX_DESC_COUNT 128U
#define E1000_MAX_PACKET_SIZE 1518U

#define E1000_REG_CTRL 0x0000U
#define E1000_REG_STATUS 0x0008U
#define E1000_REG_IMC 0x00D8U
#define E1000_REG_RCTL 0x0100U
#define E1000_REG_TCTL 0x0400U
#define E1000_REG_TIPG 0x0410U
#define E1000_REG_RDBAL 0x2800U
#define E1000_REG_RDBAH 0x2804U
#define E1000_REG_RDLEN 0x2808U
#define E1000_REG_RDH 0x2810U
#define E1000_REG_RDT 0x2818U
#define E1000_REG_TDBAL 0x3800U
#define E1000_REG_TDBAH 0x3804U
#define E1000_REG_TDLEN 0x3808U
#define E1000_REG_TDH 0x3810U
#define E1000_REG_TDT 0x3818U

#define E1000_RCTL_EN (1U << 1)
#define E1000_RCTL_BAM (1U << 15)
#define E1000_RCTL_SECRC (1U << 26)

#define E1000_TCTL_EN (1U << 1)
#define E1000_TCTL_PSP (1U << 3)
#define E1000_TCTL_CT_SHIFT 4U
#define E1000_TCTL_COLD_SHIFT 12U

#define E1000_TX_CMD_EOP (1U << 0)
#define E1000_TX_CMD_IFCS (1U << 1)
#define E1000_TX_CMD_RS (1U << 3)
#define E1000_TX_STATUS_DD (1U << 0)
#define E1000_RX_STATUS_DD (1U << 0)

int e1000_init(uint64_t bar_address);
void e1000_reset(void);
void e1000_write_reg(uint32_t reg, uint32_t val);
uint32_t e1000_read_reg(uint32_t reg);
int e1000_send_packet(const void *packet, size_t length);
int e1000_receive_packet(void *buffer_out, size_t max_length);

#endif
