#include "mdns_resolve.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MDNS_PORT 5353
#define MDNS_GROUP "224.0.0.251"
#define MDNS_TYPE_A 1
#define MDNS_CLASS_IN 1
#define MDNS_QU_BIT 0x8000 /* "unicast response wanted" (RFC 6762 section 5.4) */
#define MDNS_NAME_MAX 255

bool mdns_is_local_name(const char * host) {
    if (!host) return false;
    size_t len = strlen(host);
    if (len && host[len - 1] == '.') len--;
    return len > 6 && strncasecmp(host + len - 6, ".local", 6) == 0;
}

/* host without a trailing dot, into out; false when it is too long. */
static bool mdns_plain_name(const char * host, char * out, size_t size) {
    size_t len = strlen(host);
    if (len && host[len - 1] == '.') len--;
    if (!len || len >= size || len > MDNS_NAME_MAX - 2) return false;
    memcpy(out, host, len);
    out[len] = '\0';
    return true;
}

size_t mdns_build_query(const char * name, uint16_t id, uint8_t * buf, size_t size) {
    char plain[MDNS_NAME_MAX + 1];
    if (!name || !mdns_plain_name(name, plain, sizeof(plain)) || size < 12) return 0;
    memset(buf, 0, 12);
    buf[0] = (uint8_t) (id >> 8);
    buf[1] = (uint8_t) id;
    buf[5] = 1; /* one question */
    size_t at = 12;
    const char * label = plain;
    while (*label) {
        const char * dot = strchr(label, '.');
        size_t n = dot ? (size_t) (dot - label) : strlen(label);
        if (n == 0 || n > 63 || at + 1 + n + 5 > size) return 0;
        buf[at++] = (uint8_t) n;
        memcpy(buf + at, label, n);
        at += n;
        label += n;
        if (*label == '.') label++;
    }
    buf[at++] = 0;
    uint16_t qtype = MDNS_TYPE_A, qclass = MDNS_CLASS_IN | MDNS_QU_BIT;
    buf[at++] = (uint8_t) (qtype >> 8);
    buf[at++] = (uint8_t) qtype;
    buf[at++] = (uint8_t) (qclass >> 8);
    buf[at++] = (uint8_t) qclass;
    return at;
}

/* Reads the (possibly compressed) name at *at into out as dotted text and
 * moves *at past it. False on a malformed or looping name. */
static bool mdns_read_name(const uint8_t * p, size_t len, size_t * at, char * out, size_t size) {
    size_t pos = *at, used = 0;
    bool jumped = false;
    int jumps = 0;
    for (;;) {
        if (pos >= len) return false;
        uint8_t n = p[pos];
        if ((n & 0xC0) == 0xC0) {
            if (pos + 1 >= len || ++jumps > 16) return false;
            size_t target = ((size_t) (n & 0x3F) << 8) | p[pos + 1];
            if (!jumped) *at = pos + 2;
            jumped = true;
            pos = target;
            continue;
        }
        if (n & 0xC0) return false;
        pos++;
        if (n == 0) break;
        if (pos + n > len || used + n + 2 > size) return false;
        if (used) out[used++] = '.';
        memcpy(out + used, p + pos, n);
        used += n;
        pos += n;
    }
    out[used] = '\0';
    if (!jumped) *at = pos;
    return true;
}

static uint16_t mdns_u16(const uint8_t * p) {
    return (uint16_t) ((p[0] << 8) | p[1]);
}

bool mdns_parse_a_response(const uint8_t * p, size_t len, const char * name, uint32_t * out_addr) {
    char want[MDNS_NAME_MAX + 1], got[MDNS_NAME_MAX + 1];
    if (!p || len < 12 || !name || !mdns_plain_name(name, want, sizeof(want))) return false;
    if (!(p[2] & 0x80)) return false; /* a query, not a response */
    unsigned questions = mdns_u16(p + 4);
    unsigned records = (unsigned) mdns_u16(p + 6) + mdns_u16(p + 8) + mdns_u16(p + 10);
    size_t at = 12;
    for (unsigned i = 0; i < questions; i++) {
        if (!mdns_read_name(p, len, &at, got, sizeof(got)) || at + 4 > len) return false;
        at += 4;
    }
    for (unsigned i = 0; i < records; i++) {
        if (!mdns_read_name(p, len, &at, got, sizeof(got)) || at + 10 > len) return false;
        uint16_t type = mdns_u16(p + at), klass = mdns_u16(p + at + 2) & 0x7FFF; /* top bit: cache flush */
        uint16_t rdlen = mdns_u16(p + at + 8);
        at += 10;
        if (at + rdlen > len) return false;
        if (type == MDNS_TYPE_A && klass == MDNS_CLASS_IN && rdlen == 4 && strcasecmp(got, want) == 0) {
            memcpy(out_addr, p + at, 4);
            return true;
        }
        at += rdlen;
    }
    return false;
}

static uint64_t mdns_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000ULL + (uint64_t) ts.tv_nsec / 1000000ULL;
}

bool mdns_resolve_ipv4(const char * host, uint32_t timeout_ms, char * out, size_t out_size) {
    uint8_t query[300];
    uint16_t id = (uint16_t) (mdns_now_ms() & 0xFFFF);
    size_t query_len = mdns_build_query(host, id, query, sizeof(query));
    if (!query_len || !out || out_size < INET_ADDRSTRLEN) return false;
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    unsigned char ttl = 255;
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    struct sockaddr_in group = { .sin_family = AF_INET, .sin_port = htons(MDNS_PORT) };
    inet_pton(AF_INET, MDNS_GROUP, &group.sin_addr);

    bool found = false;
    uint64_t start = mdns_now_ms(), next_send = start;
    int sends = 0;
    while (!found) {
        uint64_t now = mdns_now_ms();
        if (now - start >= timeout_ms) break;
        /* Ask again after 250 ms and 1 s: a dropped multicast packet is common on Wi-Fi. */
        if (now >= next_send && sends < 3) {
            sendto(fd, query, query_len, 0, (struct sockaddr *) &group, sizeof(group));
            sends++;
            next_send = now + (sends == 1 ? 250 : 750);
        }
        uint64_t wait_until = start + timeout_ms;
        if (sends < 3 && next_send < wait_until) wait_until = next_send;
        int wait_ms = wait_until > now ? (int) (wait_until - now) : 1;
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int ready = poll(&pfd, 1, wait_ms);
        if (ready < 0 && errno != EINTR) break;
        if (ready <= 0) continue;
        uint8_t reply[1500];
        ssize_t n = recv(fd, reply, sizeof(reply), 0);
        uint32_t addr;
        if (n > 0 && mdns_parse_a_response(reply, (size_t) n, host, &addr)) {
            struct in_addr in = { .s_addr = addr };
            found = inet_ntop(AF_INET, &in, out, (socklen_t) out_size) != NULL;
        }
    }
    close(fd);
    return found;
}
