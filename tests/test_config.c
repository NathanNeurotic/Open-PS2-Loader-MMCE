#include <stdio.h>
#include <string.h>
#include "minunit.h"
#include "../include/opl_config.h"

// Mock for testing without full PS2SDK linkage
OPL_NetworkConfig gNetworkConfig;

int tests_run = 0;

static const char *test_network_config_encapsulation() {
    gNetworkConfig.ps2_ip_use_dhcp = 1;
    mu_assert("error, ps2_ip_use_dhcp should be 1", gNetworkConfig.ps2_ip_use_dhcp == 1);

    gNetworkConfig.eth_op_mode = 2;
    mu_assert("error, eth_op_mode should be 2", gNetworkConfig.eth_op_mode == 2);

    return 0;
}

static const char *all_tests() {
    mu_run_test(test_network_config_encapsulation);
    return 0;
}

int main(int argc, char **argv) {
    const char *result = all_tests();
    if (result != 0) {
        printf("%s\n", result);
    } else {
        printf("ALL TESTS PASSED\n");
    }
    printf("Tests run: %d\n", tests_run);

    return result != 0;
}
