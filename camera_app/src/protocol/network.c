#define _GNU_SOURCE
#include "camera_app/network.h"
#include "apcam/network.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef __CYGWIN__
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <sys/socket.h>
#include <unistd.h>

#define SNAPSHOT_MAX 64
struct entry {
    _Alignas(struct nlmsghdr) unsigned char data[1024];
};
struct snapshot {
    struct entry addresses[SNAPSHOT_MAX], routes[SNAPSHOT_MAX];
    unsigned na, nr;
};
struct saved_state {
    uint32_t magic;
    struct ca_network_config config;
    uint32_t secondary_owned;
};
#define STATE_MAGIC UINT32_C(0x414e4331)

static void attribute(struct nlmsghdr *h, unsigned type, const void *data, size_t size)
{
    struct rtattr *a = (struct rtattr *)((char *)h + NLMSG_ALIGN(h->nlmsg_len));
    a->rta_type = type;
    a->rta_len = RTA_LENGTH(size);
    memcpy(RTA_DATA(a), data, size);
    h->nlmsg_len = NLMSG_ALIGN(h->nlmsg_len) + RTA_ALIGN(a->rta_len);
}
static int open_netlink(void)
{
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0)
        return -1;
    struct timeval timeout = {.tv_sec = 2};
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        close(fd);
        return -1;
    }
    struct sockaddr_nl peer = {.nl_family = AF_NETLINK};
    if (connect(fd, (struct sockaddr *)&peer, sizeof(peer)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}
static int transact(struct nlmsghdr *h, unsigned type)
{
    int fd = open_netlink();
    if (fd < 0)
        return -1;
    h->nlmsg_seq = 1;
    h->nlmsg_pid = 0;
    h->nlmsg_type = type;
    h->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    bool add = type == RTM_NEWADDR || type == RTM_NEWROUTE;
    if (add)
        h->nlmsg_flags |= NLM_F_CREATE | NLM_F_EXCL;
    int result = -1;
    if (send(fd, h, h->nlmsg_len, 0) == (ssize_t)h->nlmsg_len) {
        char data[8192];
        ssize_t size = recv(fd, data, sizeof(data), 0);
        for (struct nlmsghdr *reply = (void *)data; NLMSG_OK(reply, size); reply = NLMSG_NEXT(reply, size)) {
            if (reply->nlmsg_seq != 1 || reply->nlmsg_type != NLMSG_ERROR ||
                reply->nlmsg_len < NLMSG_LENGTH(sizeof(struct nlmsgerr)))
                continue;
            int error = ((struct nlmsgerr *)NLMSG_DATA(reply))->error;
            if (!error || (!add && (error == -EADDRNOTAVAIL || error == -ESRCH)))
                result = 0;
            else
                errno = -error;
            break;
        }
    }
    int saved = errno;
    close(fd);
    errno = saved;
    return result;
}
static int snapshot(unsigned index, struct snapshot *s, bool routes)
{
    int fd = open_netlink();
    if (fd < 0)
        return -1;
    struct {
        struct nlmsghdr h;
        struct rtgenmsg gen;
    } request = {
        .h = {.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtgenmsg)),
              .nlmsg_seq = 1,
              .nlmsg_type = routes ? RTM_GETROUTE : RTM_GETADDR,
              .nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP},
        .gen = {.rtgen_family = AF_INET},
    };
    int result = -1;
    if (send(fd, &request, request.h.nlmsg_len, 0) < 0)
        goto done;
    for (;;) {
        unsigned char data[32768];
        ssize_t size = recv(fd, data, sizeof(data), 0);
        if (size <= 0)
            break;
        for (struct nlmsghdr *h = (void *)data; NLMSG_OK(h, size); h = NLMSG_NEXT(h, size)) {
            if (h->nlmsg_flags & NLM_F_DUMP_INTR) {
                errno = EINTR;
                goto done;
            }
            if (h->nlmsg_type == NLMSG_DONE) {
                result = 0;
                goto done;
            }
            if (h->nlmsg_type == NLMSG_ERROR) {
                errno = EIO;
                goto done;
            }
            unsigned *count;
            struct entry *entries;
            if (!routes && h->nlmsg_type == RTM_NEWADDR) {
                struct ifaddrmsg *a = NLMSG_DATA(h);
                if (a->ifa_family != AF_INET || a->ifa_index != index || a->ifa_scope != RT_SCOPE_UNIVERSE)
                    continue;
                count = &s->na;
                entries = s->addresses;
            } else if (routes && h->nlmsg_type == RTM_NEWROUTE) {
                struct rtmsg *r = NLMSG_DATA(h);
                if (r->rtm_family != AF_INET || r->rtm_type != RTN_UNICAST)
                    continue;
                unsigned out = 0;
                int length = RTM_PAYLOAD(h);
                for (struct rtattr *a = RTM_RTA(r); RTA_OK(a, length); a = RTA_NEXT(a, length)) {
                    if (a->rta_type == RTA_OIF && RTA_PAYLOAD(a) == sizeof(out))
                        memcpy(&out, RTA_DATA(a), sizeof(out));
                    /* A shared multipath route needs a transaction spanning
                     * multiple interfaces. Refuse before changing anything. */
                    if (a->rta_type == RTA_MULTIPATH) {
                        int left = RTA_PAYLOAD(a);
                        for (struct rtnexthop *hop = RTA_DATA(a); RTNH_OK(hop, left); hop = RTNH_NEXT(hop)) {
                            if ((unsigned)hop->rtnh_ifindex == index) {
                                errno = ENOTSUP;
                                goto done;
                            }
                            left -= RTNH_ALIGN(hop->rtnh_len);
                        }
                    }
                }
                if (out != index)
                    continue;
                count = &s->nr;
                entries = s->routes;
            } else
                continue;
            if (*count >= SNAPSHOT_MAX || h->nlmsg_len > sizeof(entries[0].data)) {
                errno = E2BIG;
                goto done;
            }
            memcpy(entries[(*count)++].data, h, h->nlmsg_len);
        }
    }
done: {
    int saved = errno;
    close(fd);
    errno = saved;
}
    return result;
}
static bool matches(struct entry *entry, struct in_addr address, unsigned prefix)
{
    struct nlmsghdr *h = (void *)entry->data;
    struct ifaddrmsg *ifa = NLMSG_DATA(h);
    int length = IFA_PAYLOAD(h);
    if (ifa->ifa_prefixlen != prefix)
        return false;
    for (struct rtattr *a = IFA_RTA(ifa); RTA_OK(a, length); a = RTA_NEXT(a, length)) {
        if (a->rta_type == IFA_LOCAL && RTA_PAYLOAD(a) == sizeof(address) &&
            !memcmp(RTA_DATA(a), &address, sizeof(address)))
            return true;
    }
    return false;
}
static bool has_address(struct snapshot *s, const char *text)
{
    struct in_addr ip;
    unsigned prefix;
    if (!apcam_ipv4_prefix(text, &ip, &prefix))
        return false;
    for (unsigned i = 0; i < s->na; i++)
        if (matches(&s->addresses[i], ip, prefix))
            return true;
    return false;
}
/* Kernel address order matters for source selection, even if the requested
 * set of IPs is unchanged. Rebuild when promoting the old secondary. */
static bool first_address_matches(struct snapshot *s, const char *text)
{
    struct in_addr ip;
    unsigned prefix;
    return s->na && apcam_ipv4_prefix(text, &ip, &prefix) && matches(&s->addresses[0], ip, prefix) &&
           !(((struct ifaddrmsg *)NLMSG_DATA((struct nlmsghdr *)s->addresses[0].data))->ifa_flags &
             IFA_F_SECONDARY);
}
static bool default_route(struct entry *entry)
{
    struct nlmsghdr *h = (void *)entry->data;
    struct rtmsg *r = NLMSG_DATA(h);
    unsigned table = r->rtm_table;
    int length = RTM_PAYLOAD(h);
    for (struct rtattr *a = RTM_RTA(r); RTA_OK(a, length); a = RTA_NEXT(a, length))
        if (a->rta_type == RTA_TABLE && RTA_PAYLOAD(a) == sizeof(table))
            memcpy(&table, RTA_DATA(a), sizeof(table));
    return table == RT_TABLE_MAIN && r->rtm_dst_len == 0;
}
static bool route_gateway_matches(struct entry *entry, const char *text)
{
    struct in_addr ip;
    if (!apcam_ipv4_host(text, &ip))
        return false;
    struct nlmsghdr *h = (void *)entry->data;
    struct rtmsg *r = NLMSG_DATA(h);
    int length = RTM_PAYLOAD(h);
    for (struct rtattr *a = RTM_RTA(r); RTA_OK(a, length); a = RTA_NEXT(a, length))
        if (a->rta_type == RTA_GATEWAY && RTA_PAYLOAD(a) == sizeof(ip) &&
            !memcmp(RTA_DATA(a), &ip, sizeof(ip)))
            return true;
    return false;
}
/* Connected/local routes are recreated by the kernel. Preserve user routes
 * if they remain reachable; otherwise fail and roll back the address change. */
static int restore_routes(struct snapshot *s, bool rollback)
{
    for (unsigned i = 0; i < s->nr; i++) {
        struct nlmsghdr *h = (void *)s->routes[i].data;
        struct rtmsg *r = NLMSG_DATA(h);
        if (!rollback && (default_route(&s->routes[i]) || r->rtm_protocol == RTPROT_KERNEL))
            continue;
        if (transact(h, RTM_NEWROUTE) < 0 && errno != EEXIST)
            return -1;
    }
    return 0;
}
static int address(unsigned index, const char *text, bool add)
{
    if (!*text)
        return 0;
    struct in_addr ip;
    unsigned prefix;
    if (!apcam_ipv4_prefix(text, &ip, &prefix)) {
        errno = EINVAL;
        return -1;
    }
    struct {
        struct nlmsghdr h;
        struct ifaddrmsg a;
        char attrs[64];
    } request = {
        .h = {.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifaddrmsg))},
        .a = {.ifa_family = AF_INET,
              .ifa_prefixlen = prefix,
              .ifa_index = index,
              .ifa_scope = RT_SCOPE_UNIVERSE},
    };
    attribute(&request.h, IFA_LOCAL, &ip, sizeof(ip));
    attribute(&request.h, IFA_ADDRESS, &ip, sizeof(ip));
    if (prefix < 31) {
        struct in_addr broadcast = {.s_addr = ip.s_addr | htonl(UINT32_MAX >> prefix)};
        attribute(&request.h, IFA_BROADCAST, &broadcast, sizeof(broadcast));
    }
    if (transact(&request.h, add ? RTM_NEWADDR : RTM_DELADDR) == 0)
        return 0;
    /* Exact duplicates are harmless; a different prefix is not equivalent. */
    if (add && errno == EEXIST) {
        struct snapshot *s = calloc(1, sizeof(*s));
        if (!s)
            return -1;
        bool found = false;
        if (snapshot(index, s, false) == 0)
            for (unsigned i = 0; i < s->na; i++)
                found |= matches(&s->addresses[i], ip, prefix);
        free(s);
        if (found)
            return 0;
        errno = EEXIST;
    }
    return -1;
}
static int gateway(unsigned index, const char *text, bool add)
{
    if (!*text)
        return 0;
    struct in_addr ip;
    if (!apcam_ipv4_host(text, &ip)) {
        errno = EINVAL;
        return -1;
    }
    struct {
        struct nlmsghdr h;
        struct rtmsg r;
        char attrs[64];
    } request = {
        .h = {.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg))},
        .r = {.rtm_family = AF_INET,
              .rtm_table = RT_TABLE_MAIN,
              .rtm_protocol = RTPROT_STATIC,
              .rtm_scope = RT_SCOPE_UNIVERSE,
              .rtm_type = RTN_UNICAST},
    };
    attribute(&request.h, RTA_GATEWAY, &ip, sizeof(ip));
    attribute(&request.h, RTA_OIF, &index, sizeof(index));
    return transact(&request.h, add ? RTM_NEWROUTE : RTM_DELROUTE);
}
static int configure_gateway(unsigned index, const struct ca_network_config *config,
                             const struct saved_state *previous)
{
    struct snapshot *current = calloc(1, sizeof(*current));
    if (!current)
        return -1;
    int result = -1;
    if (snapshot(index, current, true) < 0)
        goto done;
    unsigned defaults = 0, matches = 0;
    for (unsigned i = 0; i < current->nr; i++) {
        struct entry *entry = &current->routes[i];
        if (!default_route(entry))
            continue;
        defaults++;
        matches += route_gateway_matches(entry, config->gateway);
    }
    if (config->primary_address[0] || config->gateway[0]) {
        /* No netlink writes when the requested default already exists. */
        if (defaults == (config->gateway[0] ? 1U : 0U) && matches == defaults) {
            result = 0;
            goto done;
        }
        for (unsigned i = 0; i < current->nr; i++)
            if (default_route(&current->routes[i]) &&
                transact((void *)current->routes[i].data, RTM_DELROUTE) < 0)
                goto done;
        if (gateway(index, config->gateway, true) < 0)
            goto done;
    } else if (previous->magic && previous->config.gateway[0]) {
        /* Remove only the previously configured gateway, including a route
         * that was already present with a different protocol or metric. */
        for (unsigned i = 0; i < current->nr; i++)
            if (default_route(&current->routes[i]) &&
                route_gateway_matches(&current->routes[i], previous->config.gateway) &&
                transact((void *)current->routes[i].data, RTM_DELROUTE) < 0)
                goto done;
    }
    result = 0;
done:
    free(current);
    return result;
}
static int remove_entries(struct entry *entries, unsigned count, unsigned type)
{
    /* Secondary addresses first: deleting a primary can also delete its siblings. */
    for (unsigned i = count; i; i--)
        if (transact((void *)entries[i - 1].data, type) < 0)
            return -1;
    return 0;
}
static bool valid_state(struct saved_state *s)
{
    return s->magic == STATE_MAGIC && s->secondary_owned <= 1 &&
           memchr(s->config.interface, 0, sizeof(s->config.interface)) &&
           memchr(s->config.primary_address, 0, sizeof(s->config.primary_address)) &&
           memchr(s->config.secondary_address, 0, sizeof(s->config.secondary_address)) &&
           memchr(s->config.gateway, 0, sizeof(s->config.gateway)) &&
           apcam_network_valid(s->config.primary_address, s->config.secondary_address, s->config.gateway);
}
int ca_network_configure(const struct ca_network_config *config, const char *state_path)
{
    if (!config || !state_path ||
        !apcam_network_valid(config->primary_address, config->secondary_address, config->gateway)) {
        errno = EINVAL;
        return -1;
    }
    struct saved_state previous = {0};
    FILE *file = fopen(state_path, "rb");
    if (file) {
        if (fread(&previous, sizeof(previous), 1, file) != 1 || !valid_state(&previous))
            memset(&previous, 0, sizeof(previous));
        fclose(file);
    }
    if (!config->primary_address[0] && !config->secondary_address[0] && !config->gateway[0] &&
        !previous.magic)
        return 0;
    unsigned index = if_nametoindex(config->interface);
    if (!index) {
        errno = ENODEV;
        return -1;
    }
    /* Configuration applies only to the selected interface. */
    if (previous.magic && strcmp(previous.config.interface, config->interface))
        memset(&previous, 0, sizeof(previous));
    struct snapshot *before = calloc(1, sizeof(*before));
    if (!before)
        return -1;
    int result = -1;
    if (snapshot(index, before, false) < 0 || snapshot(index, before, true) < 0)
        goto done;
    char temporary[4096];
    if (snprintf(temporary, sizeof(temporary), "%s.XXXXXX", state_path) >= (int)sizeof(temporary)) {
        errno = ENAMETOOLONG;
        goto done;
    }
    struct saved_state next = {.magic = STATE_MAGIC, .config = *config};
    /* Do not claim a pre-existing address as ours: clearing the secondary
     * later must not delete the camera's boot-time primary address. */
    next.secondary_owned = config->secondary_address[0] &&
                           (config->primary_address[0] || !has_address(before, config->secondary_address) ||
                            (previous.magic && previous.secondary_owned &&
                             !strcmp(previous.config.secondary_address, config->secondary_address)));
    bool replace_addresses =
        config->primary_address[0] &&
        (before->na != (config->secondary_address[0] ? 2U : 1U) ||
         !first_address_matches(before, config->primary_address) ||
         (config->secondary_address[0] && !has_address(before, config->secondary_address)));
    int temp_fd = mkstemp(temporary);
    if (temp_fd < 0)
        goto done;
    file = fdopen(temp_fd, "wb");
    if (!file) {
        int saved = errno;
        close(temp_fd);
        unlink(temporary);
        errno = saved;
        goto done;
    }
    bool written = fwrite(&next, sizeof(next), 1, file) == 1;
    if (fclose(file))
        written = false;
    if (!written) {
        unlink(temporary);
        goto done;
    }
    if (config->primary_address[0]) {
        if (replace_addresses && (remove_entries(before->addresses, before->na, RTM_DELADDR) < 0 ||
                                  address(index, config->primary_address, true) < 0))
            goto rollback;
    } else if (previous.magic && previous.secondary_owned &&
               strcmp(previous.config.secondary_address, config->secondary_address) &&
               address(index, previous.config.secondary_address, false) < 0)
        goto rollback;
    if (address(index, config->secondary_address, true) < 0)
        goto rollback;
    if (restore_routes(before, false) < 0 || configure_gateway(index, config, &previous) < 0)
        goto rollback;
    if (rename(temporary, state_path) < 0)
        goto rollback;
    result = 0;
    goto done;
rollback: {
    int saved = errno;
    bool restored = true;
    struct snapshot *current = calloc(1, sizeof(*current));
    if (current) {
        if (snapshot(index, current, false) == 0 && snapshot(index, current, true) == 0) {
            if (remove_entries(current->routes, current->nr, RTM_DELROUTE) < 0)
                restored = false;
            if (remove_entries(current->addresses, current->na, RTM_DELADDR) < 0)
                restored = false;
        } else
            restored = false;
        free(current);
    } else
        restored = false;
    for (unsigned i = 0; i < before->na; i++)
        if (transact((void *)before->addresses[i].data, RTM_NEWADDR) < 0 && errno != EEXIST)
            restored = false;
    for (unsigned i = 0; i < before->nr; i++)
        if (transact((void *)before->routes[i].data, RTM_NEWROUTE) < 0 && errno != EEXIST)
            restored = false;
    if (!restored)
        fprintf(stderr, "network: failed to restore all addresses/routes on %s after: %s\n",
                config->interface, strerror(saved));
    unlink(temporary);
    errno = saved;
}
done:
    free(before);
    return result;
}
#else
int ca_network_configure(const struct ca_network_config *config, const char *state_path)
{
    (void)state_path;
    if (config->primary_address[0] || config->secondary_address[0] || config->gateway[0]) {
        errno = ENOTSUP;
        return -1;
    }
    return 0;
}
#endif
