#define _GNU_SOURCE
#include "camera_app/support_network.h"
#include <arpa/inet.h>
#include <errno.h>
#ifndef __CYGWIN__
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void attribute(struct nlmsghdr *header, unsigned type,
                       const void *data, size_t size)
{
    struct rtattr *attr = (struct rtattr *)((char *)header + NLMSG_ALIGN(header->nlmsg_len));
    attr->rta_type = type;
    attr->rta_len = RTA_LENGTH(size);
    memcpy(RTA_DATA(attr), data, size);
    header->nlmsg_len = NLMSG_ALIGN(header->nlmsg_len) + RTA_ALIGN(attr->rta_len);
}

static int transact(struct nlmsghdr *header)
{
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0) return -1;
    struct timeval timeout = {.tv_sec = 2};
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    struct sockaddr_nl address = {.nl_family = AF_NETLINK};
    header->nlmsg_seq = 1;
    header->nlmsg_flags |= NLM_F_REQUEST | NLM_F_ACK;
    int result = -1;
    if (sendto(fd, header, header->nlmsg_len, 0,
               (struct sockaddr *)&address, sizeof(address)) >= 0) {
        char response[4096];
        ssize_t length = recv(fd, response, sizeof(response), 0);
        for (struct nlmsghdr *h = (struct nlmsghdr *)response;
             NLMSG_OK(h, length); h = NLMSG_NEXT(h, length)) {
            if (h->nlmsg_type != NLMSG_ERROR || h->nlmsg_seq != 1 ||
                h->nlmsg_len < NLMSG_LENGTH(sizeof(struct nlmsgerr))) continue;
            int error = ((struct nlmsgerr *)NLMSG_DATA(h))->error;
            if (!error || error == -EEXIST) result = 0;
            else errno = -error;
            break;
        }
    }
    int saved = errno;
    close(fd);
    errno = saved;
    return result;
}

int ca_support_network_configure(const struct ca_support_config *config)
{
    if (!config->enabled || (!config->network_address[0] && !config->network_gateway[0])) return 0;
    unsigned index = if_nametoindex(config->network_interface);
    if (!index) { errno = ENODEV; return -1; }
    if (config->network_address[0]) {
        char text[32];
        snprintf(text, sizeof(text), "%s", config->network_address);
        char *slash = strchr(text, '/');
        if (!slash) { errno = EINVAL; return -1; }
        *slash++ = '\0';
        char *end;
        long prefix = strtol(slash, &end, 10);
        struct in_addr address;
        if (*end || end == slash || prefix < 1 || prefix > 32 ||
            inet_pton(AF_INET, text, &address) != 1) { errno = EINVAL; return -1; }
        struct {
            struct nlmsghdr header;
            struct ifaddrmsg address;
            char attributes[64];
        } request = {
            .header = {.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifaddrmsg)),
                       .nlmsg_type = RTM_NEWADDR, .nlmsg_flags = NLM_F_CREATE | NLM_F_EXCL},
            .address = {.ifa_family = AF_INET, .ifa_prefixlen = prefix,
                        .ifa_index = index, .ifa_scope = RT_SCOPE_UNIVERSE},
        };
        attribute(&request.header, IFA_LOCAL, &address, sizeof(address));
        attribute(&request.header, IFA_ADDRESS, &address, sizeof(address));
        if (transact(&request.header) < 0) return -1;
    }
    if (config->network_gateway[0]) {
        struct in_addr gateway;
        if (inet_pton(AF_INET, config->network_gateway, &gateway) != 1) { errno = EINVAL; return -1; }
        struct {
            struct nlmsghdr header;
            struct rtmsg route;
            char attributes[64];
        } request = {
            .header = {.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg)),
                       .nlmsg_type = RTM_NEWROUTE, .nlmsg_flags = NLM_F_CREATE | NLM_F_REPLACE},
            .route = {.rtm_family = AF_INET, .rtm_table = RT_TABLE_MAIN,
                      .rtm_protocol = RTPROT_STATIC, .rtm_scope = RT_SCOPE_UNIVERSE,
                      .rtm_type = RTN_UNICAST},
        };
        attribute(&request.header, RTA_GATEWAY, &gateway, sizeof(gateway));
        attribute(&request.header, RTA_OIF, &index, sizeof(index));
        if (transact(&request.header) < 0) return -1;
    }
    return 0;
}

#else
/* Windows SITL uses the host network configured by Windows. */
int ca_support_network_configure(const struct ca_support_config *config)
{
    if (config->enabled && (config->network_address[0] || config->network_gateway[0])) { errno = ENOTSUP; return -1; }
    return 0;
}
#endif
