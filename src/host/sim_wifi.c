/* Simulated Wi-Fi for the desktop simulator (`make host`), in place of
 * wifi_control.c, wifi_status.c and wifi_dhcp.c, which drive wpa_supplicant
 * and udhcpc on the device. The computer's own network does the work: the
 * radio reads as on and joined to one network, so everything gated on Wi-Fi
 * (Discworld sync, the stores, Subsonic) runs and reaches the network the
 * computer is on. Turning Wi-Fi off in the simulator makes it read as off.
 *
 * COMPAS_SIM_SSID names the network (default "Simulator Wi-Fi");
 * COMPAS_SIM_WIFI=0 starts with Wi-Fi off. Never part of the device build. */
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../network/wifi/wifi_control.h"
#include "../network/wifi/wifi_dhcp.h"
#include "../network/wifi/wifi_status.h"

#define SIM_DEFAULT_SSID "Simulator Wi-Fi"
#define SIM_NETWORK_ID 0
#define SIM_SIGNAL_LEVEL 3

static atomic_int sim_enabled = -1; /* -1 until first read */
static atomic_bool sim_joined = true;

static bool sim_is_on(void) {
    int v = atomic_load(&sim_enabled);
    if (v < 0) {
        const char * env = getenv("COMPAS_SIM_WIFI");
        v = (env && strcmp(env, "0") == 0) ? 0 : 1;
        int expected = -1;
        if (!atomic_compare_exchange_strong(&sim_enabled, &expected, v)) v = atomic_load(&sim_enabled);
    }
    return v == 1;
}

static const char * sim_ssid(void) {
    const char * env = getenv("COMPAS_SIM_SSID");
    return (env && env[0]) ? env : SIM_DEFAULT_SSID;
}

static bool sim_connected(void) { return sim_is_on() && atomic_load(&sim_joined); }

/* The computer's first IPv4 address that isn't loopback, so the Wi-Fi info
 * screen shows the address the simulator actually talks from. */
static void sim_local_ip(char out[16]) {
    snprintf(out, 16, "127.0.0.1");
    struct ifaddrs * list = NULL;
    if (getifaddrs(&list) != 0) return;
    for (struct ifaddrs * ifa = list; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        if ((ifa->ifa_flags & IFF_LOOPBACK) || !(ifa->ifa_flags & IFF_UP)) continue;
        const struct sockaddr_in * sin = (const struct sockaddr_in *) ifa->ifa_addr;
        if (inet_ntop(AF_INET, &sin->sin_addr, out, 16)) break;
    }
    freeifaddrs(list);
}

/* ---- wifi_control.h ---- */

bool wifi_control_is_enabled(void) { return sim_is_on(); }

void wifi_control_enable(void) {
    atomic_store(&sim_enabled, 1);
    atomic_store(&sim_joined, true);
}

void wifi_control_disable(void) { atomic_store(&sim_enabled, 0); }

void wifi_control_scan_start(void) {}

int wifi_control_get_results(wifi_network_t * out, int max_count) {
    if (!sim_is_on() || max_count < 1) return 0;
    memset(&out[0], 0, sizeof(out[0]));
    snprintf(out[0].ssid, sizeof(out[0].ssid), "%s", sim_ssid());
    out[0].signal_level = SIM_SIGNAL_LEVEL;
    out[0].secured = true;
    out[0].is_current = sim_connected();
    return 1;
}

void wifi_control_finish_connect(bool associated) { (void) associated; }

bool wifi_control_connect_with_id(const char * ssid, const char * password, int * out_id) {
    (void) ssid;
    (void) password;
    if (!sim_is_on()) return false;
    atomic_store(&sim_joined, true);
    if (out_id) *out_id = SIM_NETWORK_ID;
    return true;
}

bool wifi_control_disconnect(void) {
    atomic_store(&sim_joined, false);
    return true;
}

int wifi_control_list_saved(wifi_saved_network_t * out, int max_count) {
    if (max_count < 1) return 0;
    memset(&out[0], 0, sizeof(out[0]));
    out[0].id = SIM_NETWORK_ID;
    snprintf(out[0].ssid, sizeof(out[0].ssid), "%s", sim_ssid());
    out[0].current = sim_connected();
    return 1;
}

bool wifi_control_forget(int id) {
    (void) id;
    return true;
}

bool wifi_control_connect_saved(int id) {
    (void) id;
    if (!sim_is_on()) return false;
    atomic_store(&sim_joined, true);
    return true;
}

bool wifi_control_get_info(wifi_info_t * out) {
    memset(out, 0, sizeof(*out));
    out->network_id = -1;
    if (!sim_connected()) return false;
    snprintf(out->ssid, sizeof(out->ssid), "%s", sim_ssid());
    out->network_id = SIM_NETWORK_ID;
    sim_local_ip(out->ip);
    snprintf(out->mac, sizeof(out->mac), "02:00:00:00:00:01");
    out->signal_level = SIM_SIGNAL_LEVEL;
    return true;
}

int wifi_control_get_dns(char out[][16], int max_count) {
    FILE * f = fopen("/etc/resolv.conf", "r");
    if (!f) return 0;
    int count = 0;
    char line[256];
    while (count < max_count && fgets(line, sizeof(line), f)) {
        char addr[64];
        struct in_addr a;
        if (sscanf(line, "nameserver %63s", addr) == 1 && inet_pton(AF_INET, addr, &a) == 1)
            snprintf(out[count++], 16, "%s", addr);
    }
    fclose(f);
    return count;
}

bool wifi_control_set_dns(const char * const * servers, int count) {
    (void) servers;
    (void) count;
    return true;
}

/* ---- wifi_status.h ---- */

bool wifi_get_connected(void) { return sim_connected(); }

bool wifi_get_status(int * out_signal_level) {
    if (!sim_connected()) return false;
    if (out_signal_level) *out_signal_level = SIM_SIGNAL_LEVEL;
    return true;
}

int wifi_wpa_ctrl_open(void) { return -1; }

void wifi_wpa_ctrl_close(int fd) { (void) fd; }

bool wifi_wpa_ctrl_request(int fd, const char * command, char * out_buf, size_t out_buf_size, int timeout_ms) {
    (void) fd;
    (void) command;
    (void) timeout_ms;
    if (out_buf && out_buf_size) out_buf[0] = '\0';
    return false;
}

/* ---- wifi_dhcp.h ---- */

void wifi_dhcp_start(void) {}

void wifi_dhcp_stop(void) {}
