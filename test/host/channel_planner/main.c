// Host tests for channel_planner: channel lists per role, dwell defaults and
// limits, and the weighted dwell on channels with recent APs.

#include <stdio.h>
#include <string.h>

#include "channel_planner.h"

static int s_failures;
static int s_checks;

#define CHECK(cond)                                                          \
    do {                                                                     \
        s_checks++;                                                          \
        if (!(cond)) {                                                       \
            s_failures++;                                                    \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                    \
    } while (0)

#define CHECK_EQ(a, b) CHECK((long long)(a) == (long long)(b))

static ns_board_v1_Configure cfg_with(ns_board_v1_Role role, const uint32_t *ch, int n)
{
    ns_board_v1_Configure cfg = ns_board_v1_Configure_init_zero;
    cfg.role = role;
    for (int i = 0; i < n; i++) {
        cfg.channels[i] = ch[i];
    }
    cfg.channels_count = (pb_size_t)n;
    return cfg;
}

static void test_defaults(void)
{
    channel_planner_t p;
    channel_planner_init(&p, true);
    CHECK_EQ(p.role, ns_board_v1_Role_ROLE_AUTO);
    CHECK_EQ(p.count, 13 + 28);
    CHECK_EQ(p.channels[0], 1);
    CHECK_EQ(p.channels[12], 13);
    CHECK_EQ(p.channels[13], 36);
    CHECK_EQ(p.channels[p.count - 1], 177);
    CHECK_EQ(p.dwell_24_ms, CP_DEFAULT_DWELL_24_MS);
    CHECK_EQ(p.dwell_5_ms, CP_DEFAULT_DWELL_5_MS);

    cp_hop_t hop;
    CHECK(channel_planner_next(&p, &hop));
    CHECK_EQ(hop.channel, 1);
    CHECK_EQ(hop.dwell_ms, 250);
    for (int i = 1; i < 13; i++) {
        channel_planner_next(&p, &hop);
    }
    CHECK(channel_planner_next(&p, &hop));
    CHECK_EQ(hop.channel, 36);
    CHECK_EQ(hop.dwell_ms, 150);

    // Wraps after the last channel.
    for (int i = 14; i < 41; i++) {
        channel_planner_next(&p, &hop);
    }
    CHECK_EQ(hop.channel, 177);
    channel_planner_next(&p, &hop);
    CHECK_EQ(hop.channel, 1);

    channel_planner_init(&p, false);
    CHECK_EQ(p.count, 13);
}

static void test_supported(void)
{
    CHECK(channel_planner_supported(1, false));
    CHECK(channel_planner_supported(13, false));
    CHECK(!channel_planner_supported(14, true));
    CHECK(!channel_planner_supported(0, true));
    CHECK(!channel_planner_supported(36, false));
    CHECK(channel_planner_supported(36, true));
    CHECK(channel_planner_supported(144, true));
    CHECK(channel_planner_supported(177, true));
    CHECK(!channel_planner_supported(34, true));
    CHECK(!channel_planner_supported(38, true));
    CHECK(!channel_planner_supported(181, true));
}

static void test_lists(void)
{
    channel_planner_t p;
    channel_planner_init(&p, true);

    // Order kept, duplicates and unsupported channels dropped.
    const uint32_t list[] = {11, 149, 6, 14, 11, 0, 38, 200, 1, 36};
    ns_board_v1_Configure cfg = cfg_with(ns_board_v1_Role_ROLE_AUTO, list, 10);
    CHECK(channel_planner_configure(&p, &cfg, true));
    const uint8_t want[] = {11, 149, 6, 1, 36};
    CHECK_EQ(p.count, sizeof(want));
    CHECK(memcmp(p.channels, want, sizeof(want)) == 0);

    // The same list on a 2.4 GHz only chip.
    CHECK(channel_planner_configure(&p, &cfg, false));
    const uint8_t want24[] = {11, 6, 1};
    CHECK_EQ(p.count, sizeof(want24));
    CHECK(memcmp(p.channels, want24, sizeof(want24)) == 0);

    // Band roles filter the list, or use the whole band when it is empty.
    cfg = cfg_with(ns_board_v1_Role_ROLE_BAND_5, list, 10);
    CHECK(channel_planner_configure(&p, &cfg, true));
    CHECK_EQ(p.count, 2);
    CHECK_EQ(p.channels[0], 149);
    CHECK_EQ(p.channels[1], 36);
    cfg = cfg_with(ns_board_v1_Role_ROLE_BAND_5, NULL, 0);
    CHECK(channel_planner_configure(&p, &cfg, true));
    CHECK_EQ(p.count, 28);
    CHECK_EQ(p.channels[0], 36);
    cfg = cfg_with(ns_board_v1_Role_ROLE_BAND_24, NULL, 0);
    CHECK(channel_planner_configure(&p, &cfg, true));
    CHECK_EQ(p.count, 13);
    cfg = cfg_with(ns_board_v1_Role_ROLE_BAND_24, list, 10);
    CHECK(channel_planner_configure(&p, &cfg, true));
    CHECK_EQ(p.count, 3);

    // No usable channel: rejected, previous schedule kept.
    channel_planner_t before = p;
    cfg = cfg_with(ns_board_v1_Role_ROLE_BAND_5, NULL, 0);
    CHECK(!channel_planner_configure(&p, &cfg, false));
    CHECK(memcmp(&p, &before, sizeof(p)) == 0);
    const uint32_t bad[] = {14, 200};
    cfg = cfg_with(ns_board_v1_Role_ROLE_AUTO, bad, 2);
    CHECK(!channel_planner_configure(&p, &cfg, true));
    const uint32_t only5[] = {36, 40};
    cfg = cfg_with(ns_board_v1_Role_ROLE_BAND_24, only5, 2);
    CHECK(!channel_planner_configure(&p, &cfg, true));
    CHECK(memcmp(&p, &before, sizeof(p)) == 0);

    // A full 64-entry list.
    uint32_t big[64];
    for (int i = 0; i < 64; i++) {
        big[i] = (uint32_t)(i < 13 ? i + 1 : 36);
    }
    cfg = cfg_with(ns_board_v1_Role_ROLE_AUTO, big, 64);
    CHECK(channel_planner_configure(&p, &cfg, true));
    CHECK_EQ(p.count, 14);
}

static void test_fixed_and_ble(void)
{
    channel_planner_t p;
    channel_planner_init(&p, true);
    cp_hop_t hop;

    const uint32_t fixed[] = {44, 1};
    ns_board_v1_Configure cfg = cfg_with(ns_board_v1_Role_ROLE_FIXED_CHANNEL, fixed, 2);
    CHECK(channel_planner_configure(&p, &cfg, true));
    CHECK_EQ(p.count, 1);
    for (int i = 0; i < 3; i++) {
        CHECK(channel_planner_next(&p, &hop));
        CHECK_EQ(hop.channel, 44);
        CHECK_EQ(hop.dwell_ms, 150);
    }
    // No weighting when there is nowhere else to go.
    channel_planner_report(&p, 44, 50);
    channel_planner_next(&p, &hop);
    CHECK_EQ(hop.dwell_ms, 150);

    CHECK(!channel_planner_configure(&p, &cfg, false));  // 44 needs 5 GHz
    cfg = cfg_with(ns_board_v1_Role_ROLE_FIXED_CHANNEL, NULL, 0);
    CHECK(!channel_planner_configure(&p, &cfg, true));

    cfg = cfg_with(ns_board_v1_Role_ROLE_BLE_SNIFFER, fixed, 2);
    CHECK(channel_planner_configure(&p, &cfg, true));
    CHECK_EQ(p.count, 0);
    CHECK(!channel_planner_next(&p, &hop));
}

static void test_dwell_limits(void)
{
    channel_planner_t p;
    channel_planner_init(&p, true);
    const uint32_t list[] = {1, 36};
    ns_board_v1_Configure cfg = cfg_with(ns_board_v1_Role_ROLE_AUTO, list, 2);
    cfg.dwell_24_ms = 400;
    cfg.dwell_5_ms = 10;
    CHECK(channel_planner_configure(&p, &cfg, true));
    cp_hop_t hop;
    channel_planner_next(&p, &hop);
    CHECK_EQ(hop.dwell_ms, 400);
    channel_planner_next(&p, &hop);
    CHECK_EQ(hop.dwell_ms, CP_MIN_DWELL_MS);

    cfg.dwell_24_ms = 100000;
    CHECK(channel_planner_configure(&p, &cfg, true));
    channel_planner_next(&p, &hop);
    CHECK_EQ(hop.dwell_ms, CP_MAX_DWELL_MS);
}

static void test_weighting(void)
{
    channel_planner_t p;
    channel_planner_init(&p, true);
    const uint32_t list[] = {1, 6, 36};
    ns_board_v1_Configure cfg = cfg_with(ns_board_v1_Role_ROLE_AUTO, list, 3);
    CHECK(channel_planner_configure(&p, &cfg, true));
    cp_hop_t hop;

    // Cycle 1: 6 sees 8 APs (full weight), 36 sees 4 (half), 1 sees none.
    channel_planner_next(&p, &hop);
    channel_planner_report(&p, hop.channel, 0);
    channel_planner_next(&p, &hop);
    channel_planner_report(&p, hop.channel, 8);
    channel_planner_next(&p, &hop);
    channel_planner_report(&p, hop.channel, 4);

    channel_planner_next(&p, &hop);
    CHECK_EQ(hop.channel, 1);
    CHECK_EQ(hop.dwell_ms, 250);
    channel_planner_report(&p, 1, 0);
    channel_planner_next(&p, &hop);
    CHECK_EQ(hop.channel, 6);
    CHECK_EQ(hop.dwell_ms, 500);
    channel_planner_report(&p, 6, 0);
    channel_planner_next(&p, &hop);
    CHECK_EQ(hop.channel, 36);
    CHECK_EQ(hop.dwell_ms, 225);
    channel_planner_report(&p, 36, 0);

    // More than CP_BUSY_APS still caps at 2x; reports for other channels are ignored.
    channel_planner_report(&p, 1, 300);
    channel_planner_report(&p, 11, 20);
    channel_planner_next(&p, &hop);
    CHECK_EQ(hop.dwell_ms, 500);

    // The busy visit to 6 ages out after CP_HISTORY visits without APs.
    for (int cycle = 0; cycle < CP_HISTORY; cycle++) {
        channel_planner_next(&p, &hop);
        CHECK_EQ(hop.channel, 6);
        CHECK(hop.dwell_ms == (cycle < CP_HISTORY - 1 ? 500u : 250u));
        channel_planner_report(&p, 6, 0);
        channel_planner_next(&p, &hop);
        channel_planner_next(&p, &hop);
    }

    // A new Configure forgets the history.
    CHECK(channel_planner_configure(&p, &cfg, true));
    channel_planner_next(&p, &hop);
    CHECK_EQ(hop.channel, 1);
    CHECK_EQ(hop.dwell_ms, 250);
}

int main(void)
{
    test_defaults();
    test_supported();
    test_lists();
    test_fixed_and_ble();
    test_dwell_limits();
    test_weighting();
    if (s_failures) {
        printf("channel_planner: %d of %d checks FAILED\n", s_failures, s_checks);
        return 1;
    }
    printf("channel_planner: OK (%d checks)\n", s_checks);
    return 0;
}
