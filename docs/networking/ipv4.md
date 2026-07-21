# IPv4 Protocol

This document details the IPv4 header parsing, subnet routing decisions, and checksum calculation in PhotonOS.

---

## 1. IPv4 Configuration and Header Format

PhotonOS utilizes a static IPv4 profile:
- **Local IP**: `10.0.2.15` (`NET_LOCAL_IPV4`)
- **Netmask**: `255.255.255.0` (`NET_IPV4_NETMASK`)
- **Default Gateway**: `10.0.2.2` (`NET_DEFAULT_GATEWAY_IPV4`)

The IPv4 header structure defined in `include/net.h`:

```c
struct __attribute__((packed)) ip_header {
    uint8_t ver_ihl;          // Version (4 bits) and Header Length (4 bits)
    uint8_t tos;              // Type of Service
    uint16_t total_length;    // Total packet length (including header)
    uint16_t id;              // Identification
    uint16_t flags_fragment;  // Fragment offset and flags
    uint8_t ttl;              // Time to Live
    uint8_t protocol;         // Protocol: 1 for ICMP, 17 for UDP, 6 for TCP
    uint16_t checksum;        // Header checksum
    uint32_t src_ip;          // Source IP
    uint32_t dest_ip;         // Destination IP
};
```

---

## 2. Checksum and Header Parsing

During packet reception:
1. The kernel parses the IP header from the Ethernet frame.
2. Checks version (must be 4) and header length (typically 20 bytes).
3. **Checksum Verification**: Uses `net_checksum(ip, header_len)`. The sum over the header must equal `0`. If invalid, the packet is discarded.
4. Checks destination IP: parses packet only if `dest_ip` matches `NET_LOCAL_IPV4` or is a broadcast address.

---

## 3. Subnet Routing Decisions

When sending a packet to a target destination IP:
- **On-link Check (`net_is_on_link_ipv4`)**: Determines if the target is on the same local subnet by checking:
  ```c
  (dest_ip & NET_IPV4_NETMASK) == (NET_LOCAL_IPV4 & NET_IPV4_NETMASK)
  ```
- **Next Hop Resolution**:
  - If on the same subnet, resolves the target IP directly using ARP.
  - If on a different subnet, resolves the default gateway IP (`10.0.2.2`) using ARP.
