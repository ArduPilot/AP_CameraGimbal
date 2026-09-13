#define _GNU_SOURCE
#include "camera_app/support_network.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    // This test must run in a disposable network namespace, never on the host.
    char current[128] = {0}, initial[128] = {0};
    assert(readlink("/proc/self/ns/net", current, sizeof(current) - 1) > 0);
    assert(readlink("/proc/1/ns/net", initial, sizeof(initial) - 1) > 0);
    assert(strcmp(current, initial) != 0);
    struct ca_support_config config = {0};
    config.enabled = true;
    strcpy(config.network_interface, "ca-proxy-test");
    strcpy(config.network_address, "192.0.2.25/24");
    strcpy(config.network_gateway, "192.0.2.1");
    assert(ca_support_network_configure(&config) == 0);
    assert(ca_support_network_configure(&config) == 0);
    strcpy(config.network_gateway, "bad");
    assert(ca_support_network_configure(&config) < 0);
    config.enabled = false;
    assert(ca_support_network_configure(&config) == 0);
    puts("PASS SupportProxy network setup, idempotence, invalid gateway and disabled mode");
    return 0;
}
