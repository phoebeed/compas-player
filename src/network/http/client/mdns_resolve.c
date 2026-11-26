#include "mdns_resolve.h"
#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
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
#define MDNS_TYPE_SRV 33
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

static size_t mdns_build_query_type(const char * name, uint16_t qtype, uint16_t id, uint8_t * buf, size_t size) {
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
    uint16_t qclass = MDNS_CLASS_IN | MDNS_QU_BIT;
    buf[at++] = (uint8_t) (qtype >> 8);
    buf[at++] = (uint8_t) qtype;
    buf[at++] = (uint8_t) (qclass >> 8);
    buf[at++] = (uint8_t) qclass;
    return at;
}

size_t mdns_build_query(const char * name, uint16_t id, uint8_t * buf, size_t size) {
    return mdns_build_query_type(name, MDNS_TYPE_A, id, buf, size);
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

/* Walks every record of a response; fn sees each one named `name` (its type,
 * class without the cache-flush bit, and where its data is) and returns true
 * to stop with success. */
typedef bool (*mdns_record_fn)(const uint8_t * p, size_t len, uint16_t type, uint16_t klass,
                               size_t data_at, uint16_t data_len, void * user);

static bool mdns_walk(const uint8_t * p, size_t len, const char * name, mdns_record_fn fn, void * user) {
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
        if (strcasecmp(got, want) == 0 && fn(p, len, type, klass, at, rdlen, user)) return true;
        at += rdlen;
    }
    return false;
}

static bool mdns_take_a(const uint8_t * p, size_t len, uint16_t type, uint16_t klass,
                        size_t at, uint16_t rdlen, void * user) {
    (void) len;
    if (type != MDNS_TYPE_A || klass != MDNS_CLASS_IN || rdlen != 4) return false;
    memcpy(user, p + at, 4);
    return true;
}

bool mdns_parse_a_response(const uint8_t * p, size_t len, const char * name, uint32_t * out_addr) {
    return mdns_walk(p, len, name, mdns_take_a, out_addr);
}

typedef struct { char * target; size_t size; uint16_t * port; } mdns_srv_out_t;

static bool mdns_take_srv(const uint8_t * p, size_t len, uint16_t type, uint16_t klass,
                          size_t at, uint16_t rdlen, void * user) {
    mdns_srv_out_t * o = user;
    if (type != MDNS_TYPE_SRV || klass != MDNS_CLASS_IN || rdlen < 7) return false;
    size_t name_at = at + 6;
    char target[MDNS_NAME_MAX + 1];
    if (!mdns_read_name(p, len, &name_at, target, sizeof(target)) || !target[0] || strlen(target) >= o->size)
        return false;
    *o->port = mdns_u16(p + at + 4);
    snprintf(o->target, o->size, "%s", target);
    return true;
}

bool mdns_parse_srv_response(const uint8_t * p, size_t len, const char * name, char * target, size_t size,
                             uint16_t * port) {
    mdns_srv_out_t o = { target, size, port };
    return target && port && size && mdns_walk(p, len, name, mdns_take_srv, &o);
}

static uint64_t mdns_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000ULL + (uint64_t) ts.tv_nsec / 1000000ULL;
}

/* Sends the query on every IPv4 interface (a player with more than one, or
 * with no route for multicast, may otherwise send it nowhere), again after
 * 250 ms and 1 s since a dropped multicast packet is common on Wi-Fi, and
 * hands each reply to take until it says it is done. Two ways of asking at
 * once: from a private port with "answer me directly" (legacy unicast, works
 * beside any responder already on 5353), and, when port 5353 can be shared,
 * an ordinary mDNS query from it whose answer comes back to the group. */
#define MDNS_MAX_IFS 8

static int mdns_interfaces(struct in_addr * out, int max) {
    struct ifaddrs * list = NULL;
    int n = 0;
    if (getifaddrs(&list) != 0) return 0;
    for (struct ifaddrs * i = list; i && n < max; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
        if (!(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK)) continue;
        out[n++] = ((struct sockaddr_in *) i->ifa_addr)->sin_addr;
    }
    freeifaddrs(list);
    return n;
}

static void mdns_send_all(int fd, const uint8_t * query, size_t len, const struct in_addr * ifs, int nifs) {
    struct sockaddr_in group = { .sin_family = AF_INET, .sin_port = htons(MDNS_PORT) };
    inet_pton(AF_INET, MDNS_GROUP, &group.sin_addr);
    if (nifs == 0) {
        sendto(fd, query, len, 0, (struct sockaddr *) &group, sizeof(group));
        return;
    }
    for (int i = 0; i < nifs; i++) {
        setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &ifs[i], sizeof(ifs[i]));
        sendto(fd, query, len, 0, (struct sockaddr *) &group, sizeof(group));
    }
}

static int mdns_shared_5353(const struct in_addr * ifs, int nifs) {
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif
    struct sockaddr_in any = { .sin_family = AF_INET, .sin_port = htons(MDNS_PORT), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(fd, (struct sockaddr *) &any, sizeof(any)) != 0) { close(fd); return -1; }
    bool joined = false;
    for (int i = 0; i < nifs; i++) {
        struct ip_mreq m = { .imr_interface = ifs[i] };
        inet_pton(AF_INET, MDNS_GROUP, &m.imr_multiaddr);
        if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof(m)) == 0) joined = true;
    }
    if (!joined) {
        struct ip_mreq m = { .imr_interface.s_addr = htonl(INADDR_ANY) };
        inet_pton(AF_INET, MDNS_GROUP, &m.imr_multiaddr);
        if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof(m)) != 0) { close(fd); return -1; }
    }
    unsigned char ttl = 255, loop = 0;
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
    return fd;
}

static bool mdns_ask(const char * name, uint16_t qtype, uint32_t timeout_ms,
                     bool (*take)(const uint8_t * reply, size_t n, void * user), void * user) {
    uint8_t query[300], plain_query[300];
    uint16_t id = (uint16_t) (mdns_now_ms() & 0xFFFF);
    size_t query_len = mdns_build_query_type(name, qtype, id, query, sizeof(query));
    if (!query_len) return false;
    /* The same question for the 5353 socket: id 0 and no unicast bit, as mDNS asks. */
    memcpy(plain_query, query, query_len);
    plain_query[0] = plain_query[1] = 0;
    plain_query[query_len - 2] &= 0x7F;
    struct in_addr ifs[MDNS_MAX_IFS];
    int nifs = mdns_interfaces(ifs, MDNS_MAX_IFS);
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    unsigned char ttl = 255;
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    int shared = mdns_shared_5353(ifs, nifs);

    bool found = false;
    uint64_t start = mdns_now_ms(), next_send = start;
    int sends = 0;
    while (!found) {
        uint64_t now = mdns_now_ms();
        if (now - start >= timeout_ms) break;
        if (now >= next_send && sends < 3) {
            mdns_send_all(fd, query, query_len, ifs, nifs);
            if (shared >= 0) mdns_send_all(shared, plain_query, query_len, ifs, nifs);
            sends++;
            next_send = now + (sends == 1 ? 250 : 750);
        }
        uint64_t wait_until = start + timeout_ms;
        if (sends < 3 && next_send < wait_until) wait_until = next_send;
        int wait_ms = wait_until > now ? (int) (wait_until - now) : 1;
        struct pollfd pfd[2] = { { .fd = fd, .events = POLLIN }, { .fd = shared, .events = POLLIN } };
        int ready = poll(pfd, shared >= 0 ? 2 : 1, wait_ms);
        if (ready < 0 && errno != EINTR) break;
        if (ready <= 0) continue;
        for (int k = 0; k < 2 && !found; k++) {
            if (k == 1 && shared < 0) break;
            if (!(pfd[k].revents & POLLIN)) continue;
            uint8_t reply[1500];
            ssize_t n = recv(pfd[k].fd, reply, sizeof(reply), 0);
            if (n > 0 && take(reply, (size_t) n, user)) found = true;
        }
    }
    close(fd);
    if (shared >= 0) close(shared);
    return found;
}

typedef struct { const char * host; char * out; size_t size; } mdns_a_job_t;

static bool mdns_a_take(const uint8_t * reply, size_t n, void * user) {
    mdns_a_job_t * j = user;
    uint32_t addr;
    if (!mdns_parse_a_response(reply, n, j->host, &addr)) return false;
    struct in_addr in = { .s_addr = addr };
    return inet_ntop(AF_INET, &in, j->out, (socklen_t) j->size) != NULL;
}

bool mdns_resolve_ipv4(const char * host, uint32_t timeout_ms, char * out, size_t out_size) {
    if (!host || !out || out_size < INET_ADDRSTRLEN) return false;
    mdns_a_job_t j = { host, out, out_size };
    return mdns_ask(host, MDNS_TYPE_A, timeout_ms, mdns_a_take, &j);
}

typedef struct { const char * name; char * target; size_t size; uint16_t * port; } mdns_srv_job_t;

static bool mdns_srv_take(const uint8_t * reply, size_t n, void * user) {
    mdns_srv_job_t * j = user;
    return mdns_parse_srv_response(reply, n, j->name, j->target, j->size, j->port);
}

bool mdns_resolve_srv(const char * service, uint32_t timeout_ms, char * target, size_t size, uint16_t * port) {
    if (!service || !target || !size || !port) return false;
    mdns_srv_job_t j = { service, target, size, port };
    return mdns_ask(service, MDNS_TYPE_SRV, timeout_ms, mdns_srv_take, &j);
}
