#ifndef MDNS_RESOLVE_H
#define MDNS_RESOLVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* True for a name in the multicast DNS ".local" domain (RFC 6762), such as
 * "jarvis.local" or "jarvis.local." (case-insensitive). */
bool mdns_is_local_name(const char * host);

/* Asks the local network for host's IPv4 address with a one-shot multicast
 * DNS query (unicast response requested, so it works beside any responder on
 * port 5353) and writes it dotted ("192.168.1.20") to out. musl's resolver
 * does not speak mDNS, so a Mac's "name.local" is otherwise unreachable.
 * Blocking up to timeout_ms; false when nobody answered. */
bool mdns_resolve_ipv4(const char * host, uint32_t timeout_ms, char * out, size_t out_size);

/* Parses one mDNS response: true and *out_addr (network order) when it holds
 * an A record for name. Exposed for tests. */
bool mdns_parse_a_response(const uint8_t * packet, size_t len, const char * name, uint32_t * out_addr);

/* Builds the query packet for name into buf; returns its length, 0 when the
 * name does not fit. Exposed for tests. */
size_t mdns_build_query(const char * name, uint16_t id, uint8_t * buf, size_t size);

#endif
