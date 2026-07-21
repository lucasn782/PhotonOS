# Address Resolution Protocol (ARP)

This document describes the ARP packet structure, ARP cache table mapping, and request/reply routing implementation in PhotonOS.

---

## 1. ARP Packet Structure

The ARP packet structure is defined in `include/net.h`:

```c
struct __attribute__((packed)) arp_packet {
    uint16_t hw_type;         // Hardware type (1 for Ethernet)
    uint16_t proto_type;      // Protocol type (0x0800 for IPv4)
    uint8_t hw_len;           // Hardware address length (6 for MAC)
    uint8_t proto_len;        // Protocol address length (4 for IP)
    uint16_t opcode;          // Opcode: 1 for Request, 2 for Reply
    uint8_t src_mac[6];       // Sender MAC address
    uint32_t src_ip;          // Sender IP address
    uint8_t dest_mac[6];      // Target MAC address
    uint32_t dest_ip;         // Target IP address
};
```

---

## 2. ARP Cache Table

PhotonOS maintains a static lookup cache table to map IPv4 addresses to hardware MAC addresses:

- **Cache Size**: 16 entries (`ARP_CACHE_SIZE`).
- **Replacement Strategy**: First-In, First-Out (FIFO) when the table is full.
- **Primitivas**:
  - `arp_resolve(ip, out_mac)`: Scans the cache for a match. Returns `1` if resolved.
  - `arp_cache_update(ip, mac)`: Adds a new resolved entry or updates an existing one.

---

## 3. Interrupt Handling and Resolution Flow

### Request Flow
If `arp_resolve()` misses, the network stack schedules an ARP request:
1. `arp_send_request(ip)` constructs an Ethernet broadcast frame (dest MAC `FF:FF:FF:FF:FF:FF`).
2. Populates the ARP packet with opcode `1` (Request), local MAC/IP, and the target IP.
3. Transmits the frame via `e1000_send_packet()`.

### Handling Incoming ARP Packets (`net_handle_arp`)
When the background network thread receives an ARP packet:
- **Opcode 2 (Reply)**: Reads sender MAC and IP, updates `arp_cache` via `arp_cache_update()`, resolving any pending sockets waiting on this IP.
- **Opcode 1 (Request)**: If the target IP matches the local IP (`NET_LOCAL_IPV4`), the stack immediately constructs and transmits an ARP Reply packet (opcode `2`), swapping source and destination fields.
