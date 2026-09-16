#ifndef APCAM_NETWORK_H
#define APCAM_NETWORK_H
#include <arpa/inet.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Shared by config parsing, web validation and the network service. */
static inline bool apcam_ipv4_host(const char *text, struct in_addr *address)
{
    if (inet_pton(AF_INET, text, address) != 1)
        return false;
    uint32_t host = ntohl(address->s_addr);
    return (host >> 24) > 0 && (host >> 24) != 127 && (host >> 24) < 224;
}
static inline bool apcam_ipv4_prefix(const char *text, struct in_addr *address, unsigned *prefix)
{
    char copy[32], *end;
    if (strlen(text) >= sizeof(copy))
        return false;
    strcpy(copy, text);
    char *slash = strchr(copy, '/');
    if (!slash)
        return false;
    *slash++ = 0;
    if (!*slash || *slash == '0' || strspn(slash, "0123456789") != strlen(slash))
        return false;
    unsigned long bits = strtoul(slash, &end, 10);
    if (*end || bits < 1 || bits > 32 || !apcam_ipv4_host(copy, address))
        return false;
    uint32_t host = ntohl(address->s_addr), mask = UINT32_MAX << (32 - bits);
    if (bits < 31 && (!(host & ~mask) || (host & ~mask) == ~mask))
        return false;
    *prefix = (unsigned)bits;
    return true;
}
static inline bool apcam_gateway_on_link(uint32_t gateway, uint32_t address, unsigned prefix)
{
    uint32_t mask = UINT32_MAX << (32 - prefix);
    return !((gateway ^ address) & mask) &&
           (prefix >= 31 || ((gateway & ~mask) && (gateway & ~mask) != ~mask));
}
static inline bool apcam_network_valid(const char *primary, const char *secondary, const char *gateway)
{
    struct in_addr a = {0}, b = {0}, g = {0};
    unsigned pa = 32, pb = 32;
    if ((*primary && !apcam_ipv4_prefix(primary, &a, &pa)) ||
        (*secondary && !apcam_ipv4_prefix(secondary, &b, &pb)) || (*gateway && !apcam_ipv4_host(gateway, &g)))
        return false;
    if (*primary && *secondary && a.s_addr == b.s_addr)
        return false;
    if (*gateway && ((a.s_addr && a.s_addr == g.s_addr) || (b.s_addr && b.s_addr == g.s_addr)))
        return false;
    if (*primary && *gateway) {
        uint32_t ga = ntohl(g.s_addr), aa = ntohl(a.s_addr), ab = ntohl(b.s_addr);
        if (!apcam_gateway_on_link(ga, aa, pa) && (!*secondary || !apcam_gateway_on_link(ga, ab, pb)))
            return false;
    }
    return true;
}
#endif
