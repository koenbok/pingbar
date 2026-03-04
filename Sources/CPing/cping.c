#include "cping.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <net/if.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <resolv.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static uint16_t icmp_checksum(const void *data, size_t length) {
    // Fold 16-bit words into a 32-bit sum.
    const uint16_t *words = (const uint16_t *)data;
    uint32_t sum = 0;
    size_t remaining = length;

    while (remaining > 1) {
        sum += *words++;
        remaining -= 2;
    }

    // Include trailing odd byte if present.
    if (remaining == 1) {
        sum += *(const uint8_t *)words;
    }

    // Fold carries back into 16 bits.
    while ((sum >> 16) != 0) {
        sum = (sum & 0xffffu) + (sum >> 16);
    }

    return (uint16_t)(~sum);
}

static bool is_matching_echo_reply(
    const uint8_t *packet,
    ssize_t packet_len,
    uint16_t expected_id,
    uint16_t expected_seq
) {
    // Handle both packet formats: ICMP-only and IP+ICMP.
    const uint8_t *icmp_bytes = packet;
    ssize_t icmp_len = packet_len;

    if (packet_len >= (ssize_t)sizeof(struct ip)) {
        const struct ip *ip_header = (const struct ip *)packet;
        if (ip_header->ip_v == 4) {
            ssize_t ip_header_len = (ssize_t)(ip_header->ip_hl * 4);
            if (ip_header_len >= (ssize_t)sizeof(struct ip) &&
                packet_len >= ip_header_len + (ssize_t)sizeof(struct icmp)) {
                icmp_bytes = packet + ip_header_len;
                icmp_len = packet_len - ip_header_len;
            }
        }
    }

    if (icmp_len < (ssize_t)sizeof(struct icmp)) {
        return false;
    }

    const struct icmp *icmp_header = (const struct icmp *)icmp_bytes;
    if (icmp_header->icmp_type != ICMP_ECHOREPLY) {
        return false;
    }

    // Match identifier/sequence when provided by the socket mode.
    if (expected_id != 0 && icmp_header->icmp_id != expected_id) {
        return false;
    }
    if (expected_seq != 0 && icmp_header->icmp_seq != expected_seq) {
        return false;
    }

    return true;
}

double cp_ping_once_ms(const char *ipv4_address, int timeout_ms) {
    // Validate basic inputs up front.
    if (ipv4_address == NULL || timeout_ms <= 0) {
        return -1.0;
    }

    // Use ICMP datagram sockets so the call works without root on macOS.
    int sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
    if (sockfd < 0) {
        return -1.0;
    }

    // Enforce receive timeout to keep probe bounded.
    struct timeval recv_timeout;
    recv_timeout.tv_sec = timeout_ms / 1000;
    recv_timeout.tv_usec = (timeout_ms % 1000) * 1000;
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &recv_timeout, sizeof(recv_timeout)) != 0) {
        close(sockfd);
        return -1.0;
    }

    // Parse destination IPv4 address.
    struct sockaddr_in dest_addr;
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    if (inet_pton(AF_INET, ipv4_address, &dest_addr.sin_addr) != 1) {
        close(sockfd);
        return -1.0;
    }

    // Build a minimal echo request with deterministic payload.
    struct {
        struct icmp header;
        uint8_t payload[32];
    } request_packet;
    memset(&request_packet, 0, sizeof(request_packet));

    static uint16_t sequence_counter = 0;
    uint16_t identifier = htons((uint16_t)getpid());
    uint16_t sequence = htons(++sequence_counter);

    request_packet.header.icmp_type = ICMP_ECHO;
    request_packet.header.icmp_code = 0;
    request_packet.header.icmp_id = identifier;
    request_packet.header.icmp_seq = sequence;
    for (size_t i = 0; i < sizeof(request_packet.payload); i++) {
        request_packet.payload[i] = (uint8_t)(i & 0xffu);
    }
    request_packet.header.icmp_cksum = 0;
    request_packet.header.icmp_cksum = icmp_checksum(&request_packet, sizeof(request_packet));

    // Capture send timestamp for round-trip measurement.
    struct timespec start_time;
    if (clock_gettime(CLOCK_MONOTONIC, &start_time) != 0) {
        close(sockfd);
        return -1.0;
    }

    ssize_t bytes_sent = sendto(
        sockfd,
        &request_packet,
        sizeof(request_packet),
        0,
        (const struct sockaddr *)&dest_addr,
        sizeof(dest_addr)
    );
    if (bytes_sent < 0) {
        close(sockfd);
        return -1.0;
    }

    // Read replies until timeout and accept the matching echo reply.
    for (;;) {
        uint8_t recv_buffer[1024];
        ssize_t recv_len = recvfrom(sockfd, recv_buffer, sizeof(recv_buffer), 0, NULL, NULL);
        if (recv_len < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                close(sockfd);
                return -1.0;
            }
            continue;
        }

        if (!is_matching_echo_reply(recv_buffer, recv_len, identifier, sequence)) {
            continue;
        }

        struct timespec end_time;
        if (clock_gettime(CLOCK_MONOTONIC, &end_time) != 0) {
            close(sockfd);
            return -1.0;
        }

        double start_ms = (double)start_time.tv_sec * 1000.0 + (double)start_time.tv_nsec / 1000000.0;
        double end_ms = (double)end_time.tv_sec * 1000.0 + (double)end_time.tv_nsec / 1000000.0;
        close(sockfd);
        return end_ms - start_ms;
    }
}

int cp_primary_ipv4(char *buffer, size_t buffer_len) {
    // Validate destination buffer up front.
    if (buffer == NULL || buffer_len < INET_ADDRSTRLEN) {
        return -1;
    }

    struct ifaddrs *ifaddr = NULL;
    if (getifaddrs(&ifaddr) != 0 || ifaddr == NULL) {
        return -1;
    }

    int rc = -1;
    // Scan for an active non-loopback non-point-to-point IPv4 interface.
    for (struct ifaddrs *ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL || ifa->ifa_addr->sa_family != AF_INET) {
            continue;
        }

        unsigned int flags = ifa->ifa_flags;
        if ((flags & IFF_UP) == 0 || (flags & IFF_RUNNING) == 0) {
            continue;
        }
        if ((flags & IFF_LOOPBACK) != 0 || (flags & IFF_POINTOPOINT) != 0) {
            continue;
        }

        const struct sockaddr_in *addr = (const struct sockaddr_in *)ifa->ifa_addr;
        if (inet_ntop(AF_INET, &addr->sin_addr, buffer, (socklen_t)buffer_len) == NULL) {
            continue;
        }
        rc = 0;
        break;
    }

    freeifaddrs(ifaddr);
    return rc;
}

static int parse_public_ip_http_response(const char *response, size_t response_len, char *buffer, size_t buffer_len) {
    // Locate HTTP body after headers.
    const char *body = NULL;
    for (size_t i = 0; i + 3 < response_len; i++) {
        if (response[i] == '\r' && response[i + 1] == '\n' && response[i + 2] == '\r' && response[i + 3] == '\n') {
            body = response + i + 4;
            break;
        }
    }
    if (body == NULL) {
        return -1;
    }

    // Skip leading whitespace before the IP.
    while (*body != '\0' && isspace((unsigned char)*body)) {
        body++;
    }

    // Copy a candidate IPv4 token.
    char ip_candidate[INET_ADDRSTRLEN];
    size_t out = 0;
    while (*body != '\0' && out + 1 < sizeof(ip_candidate)) {
        char c = *body;
        if ((c >= '0' && c <= '9') || c == '.') {
            ip_candidate[out++] = c;
            body++;
            continue;
        }
        break;
    }
    ip_candidate[out] = '\0';
    if (out == 0) {
        return -1;
    }

    // Validate token is a real IPv4 literal.
    struct in_addr parsed;
    if (inet_pton(AF_INET, ip_candidate, &parsed) != 1) {
        return -1;
    }

    // Return validated IP string.
    if (snprintf(buffer, buffer_len, "%s", ip_candidate) <= 0) {
        return -1;
    }
    return 0;
}

int cp_public_ipv4(char *buffer, size_t buffer_len, int timeout_ms) {
    // Validate destination buffer and timeout.
    if (buffer == NULL || buffer_len < INET_ADDRSTRLEN || timeout_ms <= 0) {
        return -1;
    }

    const char *host = "api.ipify.org";
    const char *port = "80";
    const char *request =
        "GET /?format=text HTTP/1.1\r\n"
        "Host: api.ipify.org\r\n"
        "User-Agent: PingBar/1.0\r\n"
        "Connection: close\r\n\r\n";

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    struct addrinfo *results = NULL;
    if (getaddrinfo(host, port, &hints, &results) != 0 || results == NULL) {
        return -1;
    }

    int rc = -1;
    for (struct addrinfo *it = results; it != NULL; it = it->ai_next) {
        int sockfd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (sockfd < 0) {
            continue;
        }

        // Set IO timeouts so connect/send/recv do not block indefinitely.
        struct timeval timeout;
        timeout.tv_sec = timeout_ms / 1000;
        timeout.tv_usec = (timeout_ms % 1000) * 1000;
        setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

        if (connect(sockfd, it->ai_addr, (socklen_t)it->ai_addrlen) != 0) {
            close(sockfd);
            continue;
        }

        // Send request fully before reading response.
        size_t total_sent = 0;
        size_t request_len = strlen(request);
        while (total_sent < request_len) {
            ssize_t n = send(sockfd, request + total_sent, request_len - total_sent, 0);
            if (n <= 0) {
                break;
            }
            total_sent += (size_t)n;
        }
        if (total_sent != request_len) {
            close(sockfd);
            continue;
        }

        // Read a bounded response buffer and parse the first body token.
        char response[1024];
        size_t response_len = 0;
        memset(response, 0, sizeof(response));
        for (;;) {
            if (response_len + 1 >= sizeof(response)) {
                break;
            }
            ssize_t n = recv(sockfd, response + response_len, sizeof(response) - 1 - response_len, 0);
            if (n < 0) {
                response_len = 0;
                break;
            }
            if (n == 0) {
                break;
            }
            response_len += (size_t)n;
        }

        close(sockfd);
        if (response_len == 0) {
            continue;
        }

        if (parse_public_ip_http_response(response, response_len, buffer, buffer_len) == 0) {
            rc = 0;
            break;
        }
    }

    freeaddrinfo(results);
    return rc;
}

int cp_primary_dns_ipv4(char *buffer, size_t buffer_len) {
    // Validate destination buffer up front.
    if (buffer == NULL || buffer_len < INET_ADDRSTRLEN) {
        return -1;
    }

    struct __res_state state;
    memset(&state, 0, sizeof(state));
    if (res_ninit(&state) != 0) {
        return -1;
    }

    int rc = -1;
    // Use the first configured IPv4 resolver from libc resolver state.
    if (state.nscount > 0) {
        const struct sockaddr_in *dns = &state.nsaddr_list[0];
        if (dns->sin_family == AF_INET &&
            inet_ntop(AF_INET, &dns->sin_addr, buffer, (socklen_t)buffer_len) != NULL) {
            rc = 0;
        }
    }

    res_nclose(&state);
    return rc;
}
