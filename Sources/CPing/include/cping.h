#ifndef CPING_H
#define CPING_H

#include <stddef.h>

// Sends a single ICMP echo probe to an IPv4 address string.
// Returns round-trip time in milliseconds on success, or a negative value on failure/timeout.
double cp_ping_once_ms(const char *ipv4_address, int timeout_ms);

// Resolves a best-effort primary non-loopback IPv4 address.
// Returns 0 on success, non-zero on failure.
int cp_primary_ipv4(char *buffer, size_t buffer_len);

// Resolves public IPv4 via a minimal HTTP probe.
// Returns 0 on success, non-zero on failure.
int cp_public_ipv4(char *buffer, size_t buffer_len, int timeout_ms);

// Resolves the first configured IPv4 DNS resolver from system resolver state.
// Returns 0 on success, non-zero on failure.
int cp_primary_dns_ipv4(char *buffer, size_t buffer_len);

#endif
