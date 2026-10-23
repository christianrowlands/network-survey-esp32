// Host tests for the pairing and bond policy: every transition of
// docs/protocol.md section 3 (no bonds, first bond, stranger, button window,
// EnterPairingMode, window timeout, new bond in a window, replacement, more
// than one bond, repeat pairing, ForgetBonds and the BOOT wipe), and the
// connection tracking that orders NimBLE's events for the policy.

#include <stdio.h>
#include <string.h>

#include "conn_track.h"
#include "pairing_policy.h"

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

#define FLAGS_PAIRABLE PP_ADV_FLAG_PAIRABLE
#define FLAGS_BONDED PP_ADV_FLAG_HAS_BOND
#define FLAGS_BOTH (PP_ADV_FLAG_PAIRABLE | PP_ADV_FLAG_HAS_BOND)

enum { OWNER = 1, OTHER = 2, THIRD = 3 };

static bool dropped(const pp_out_t *out, uint16_t handle)
{
    for (int i = 0; i < out->drop_count; i++) {
        if (out->drop[i] == handle) {
            return true;
        }
    }
    return false;
}

static void check_adv(const pp_state_t *p, bool on, uint8_t flags)
{
    pp_adv_t adv = pp_advertising(p);
    CHECK_EQ(adv.on, on);
    CHECK_EQ(adv.flags, flags);
}

// A board with one bond (the owner's), nobody connected.
static void bonded_board(pp_state_t *p, int max_bonds)
{
    pp_init(p, max_bonds, 2, 1);
}

// The owner reconnects and re-encrypts with its stored keys at time t.
static void owner_connects(pp_state_t *p, uint64_t t)
{
    pp_out_t out;
    pp_connected(p, OWNER, t, &out);
    pp_encrypted(p, OWNER, true, false, p->bonds, &out);
}

static void test_fresh_board(void)
{
    pp_state_t p;
    pp_out_t out;
    pp_init(&p, 1, 2, 0);
    CHECK(pp_pairable(&p));
    CHECK_EQ(pp_pair_request(&p), PP_PAIR_ALLOW);
    check_adv(&p, true, FLAGS_PAIRABLE);

    // No timeout without bonds. An unbonded phone keeps the board advertising
    // and is dropped after the long (pairable) grace.
    pp_connected(&p, OTHER, 0, &out);
    CHECK_EQ(out.drop_count, 0);
    check_adv(&p, true, FLAGS_PAIRABLE);
    // Encrypted without a bond (pairing without bonding) changes nothing.
    pp_encrypted(&p, OTHER, false, false, 0, &out);
    CHECK(!out.changed);
    CHECK_EQ(out.drop_count, 0);
    pp_tick(&p, PP_PAIRABLE_GRACE_MS - 1, &out);
    CHECK_EQ(out.drop_count, 0);
    pp_tick(&p, PP_PAIRABLE_GRACE_MS, &out);
    CHECK(pp_pairable(&p));
    CHECK_EQ(out.drop_count, 1);
    CHECK(dropped(&out, OTHER));
    CHECK(!out.changed);
}

static void test_first_bond(void)
{
    pp_state_t p;
    pp_out_t out;
    pp_init(&p, 1, 2, 0);
    pp_connected(&p, OWNER, 1000, &out);
    pp_encrypted(&p, OWNER, true, true, 1, &out);
    CHECK(out.changed);
    CHECK_EQ(out.drop_count, 0);
    CHECK(!pp_pairable(&p));
    CHECK_EQ(pp_pair_request(&p), PP_PAIR_DENY);
    check_adv(&p, false, FLAGS_BONDED);

    // Owner stays connected well past the grace time.
    pp_tick(&p, 1000 + 10 * PP_UNBONDED_GRACE_MS, &out);
    CHECK_EQ(out.drop_count, 0);

    pp_disconnected(&p, OWNER, &out);
    CHECK_EQ(p.conn_count, 0);
    check_adv(&p, true, FLAGS_BONDED);
}

static void test_stranger_outside_window(void)
{
    pp_state_t p;
    pp_out_t out;
    bonded_board(&p, 1);
    check_adv(&p, true, FLAGS_BONDED);
    CHECK_EQ(pp_pair_request(&p), PP_PAIR_DENY);  // pairing request, repeat pairing and overflow

    pp_connected(&p, OTHER, 5000, &out);
    check_adv(&p, true, FLAGS_BONDED);  // the owner can still find and connect
    // A stranger cannot open the window.
    CHECK(!pp_enter_pairing_mode(&p, OTHER, 6000, &out));
    CHECK(!p.window);
    CHECK(!pp_pairable(&p));

    pp_tick(&p, 5000 + PP_UNBONDED_GRACE_MS - 1, &out);
    CHECK_EQ(out.drop_count, 0);
    pp_tick(&p, 5000 + PP_UNBONDED_GRACE_MS, &out);
    CHECK_EQ(out.drop_count, 1);
    CHECK(dropped(&out, OTHER));
    CHECK(!out.changed);
    pp_disconnected(&p, OTHER, &out);
    check_adv(&p, true, FLAGS_BONDED);

    // The owner reconnecting is unbonded for a moment, then encrypted.
    pp_connected(&p, OWNER, 100000, &out);
    pp_tick(&p, 100500, &out);
    CHECK_EQ(out.drop_count, 0);
    pp_encrypted(&p, OWNER, true, false, 1, &out);
    CHECK(!out.changed);
    pp_tick(&p, 100000 + 2 * PP_UNBONDED_GRACE_MS, &out);
    CHECK_EQ(out.drop_count, 0);
}

static void test_button_window_timeout(void)
{
    pp_state_t p;
    pp_out_t out;
    bonded_board(&p, 1);
    pp_open_window(&p, 1000, &out);
    CHECK(out.changed);
    CHECK(p.window);
    CHECK(pp_pairable(&p));
    CHECK_EQ(pp_pair_request(&p), PP_PAIR_REPLACE);  // one bond max: the new one replaces it
    check_adv(&p, true, FLAGS_BOTH);

    // A new phone connects; the window keeps advertising for a second one.
    pp_connected(&p, OTHER, 2000, &out);
    check_adv(&p, true, FLAGS_BOTH);
    pp_connected(&p, THIRD, 3000, &out);
    check_adv(&p, false, FLAGS_BOTH);
    pp_disconnected(&p, THIRD, &out);

    // The owner reconnecting with its stored keys does not end the window.
    owner_connects(&p, 4000);
    CHECK(p.window);
    CHECK(!out.changed);
    pp_disconnected(&p, OWNER, &out);

    // The window lasts 60 s; an unbonded phone is not dropped inside it.
    pp_tick(&p, 1000 + PP_WINDOW_MS - 1, &out);
    CHECK(p.window);
    CHECK_EQ(out.drop_count, 0);
    CHECK(!out.changed);

    // At 60 s a phone without a bond is still connected (maybe pairing): one
    // 30 s extension, still pairable.
    pp_tick(&p, 1000 + PP_WINDOW_MS, &out);
    CHECK(p.window);
    CHECK(pp_pairable(&p));
    CHECK_EQ(out.drop_count, 0);
    CHECK(!out.changed);
    pp_tick(&p, 1000 + PP_WINDOW_MS + PP_WINDOW_EXTEND_MS - 1, &out);
    CHECK(p.window);
    pp_tick(&p, 1000 + PP_WINDOW_MS + PP_WINDOW_EXTEND_MS, &out);
    CHECK(!p.window);
    CHECK(out.changed);
    CHECK(!pp_pairable(&p));
    CHECK_EQ(out.drop_count, 1);
    CHECK(dropped(&out, OTHER));
    CHECK_EQ(pp_pair_request(&p), PP_PAIR_DENY);
    pp_disconnected(&p, OTHER, &out);
    check_adv(&p, true, FLAGS_BONDED);

    // Further ticks do nothing.
    pp_tick(&p, 1000 + 3 * PP_WINDOW_MS, &out);
    CHECK(!out.changed);
    CHECK_EQ(out.drop_count, 0);
}

static void test_window_restart(void)
{
    pp_state_t p;
    pp_out_t out;
    bonded_board(&p, 1);
    pp_open_window(&p, 0, &out);
    CHECK(out.changed);
    pp_open_window(&p, 50000, &out);  // a second hold restarts the 60 s
    CHECK(!out.changed);
    pp_tick(&p, PP_WINDOW_MS, &out);
    CHECK(p.window);
    pp_tick(&p, 50000 + PP_WINDOW_MS, &out);
    CHECK(!p.window);
    CHECK(out.changed);
}

static void test_enter_pairing_mode_and_replace(void)
{
    pp_state_t p;
    pp_out_t out;
    bonded_board(&p, 1);
    owner_connects(&p, 0);
    check_adv(&p, false, FLAGS_BONDED);

    CHECK(pp_enter_pairing_mode(&p, OWNER, 1000, &out));
    CHECK(out.changed);
    CHECK(pp_pairable(&p));
    // Owner stays connected; connectable advertising resumes for the new phone.
    check_adv(&p, true, FLAGS_BOTH);
    CHECK_EQ(p.window_end_ms, 1000 + PP_WINDOW_MS);

    pp_connected(&p, OTHER, 5000, &out);
    check_adv(&p, false, FLAGS_BOTH);  // two connections: the stack limit
    CHECK_EQ(pp_pair_request(&p), PP_PAIR_REPLACE);

    // The new phone bonds; the store replaced the owner's bond (count stays 1).
    pp_encrypted(&p, OTHER, true, true, 1, &out);
    CHECK(!p.window);
    CHECK(out.changed);
    CHECK(!pp_pairable(&p));
    CHECK_EQ(out.drop_count, 0);  // both marked bonded; the glue disconnects the replaced phone
    CHECK_EQ(pp_pair_request(&p), PP_PAIR_DENY);
    pp_disconnected(&p, OWNER, &out);  // the replaced phone goes away
    check_adv(&p, false, FLAGS_BONDED);
    pp_tick(&p, 1000 + 2 * PP_WINDOW_MS, &out);
    CHECK_EQ(out.drop_count, 0);
}

static void test_window_ends_with_unbonded_second(void)
{
    pp_state_t p;
    pp_out_t out;
    bonded_board(&p, 1);
    owner_connects(&p, 0);
    CHECK(pp_enter_pairing_mode(&p, OWNER, 0, &out));
    pp_connected(&p, OTHER, 10000, &out);
    pp_tick(&p, PP_WINDOW_MS, &out);  // extended once: OTHER may be pairing
    CHECK(p.window);
    CHECK_EQ(out.drop_count, 0);
    pp_tick(&p, PP_WINDOW_MS + PP_WINDOW_EXTEND_MS, &out);
    CHECK(!p.window);
    CHECK_EQ(out.drop_count, 1);
    CHECK(dropped(&out, OTHER));
    CHECK(!dropped(&out, OWNER));
}

static void test_new_bond_drops_other_unbonded(void)
{
    // Window open on a board without a phone; two new phones connect, one bonds.
    pp_state_t p;
    pp_out_t out;
    bonded_board(&p, 1);
    pp_open_window(&p, 0, &out);
    pp_connected(&p, OTHER, 1000, &out);
    pp_connected(&p, THIRD, 2000, &out);
    pp_encrypted(&p, OTHER, true, true, 1, &out);
    CHECK(!p.window);
    CHECK_EQ(out.drop_count, 1);
    CHECK(dropped(&out, THIRD));
}

static void test_more_bonds(void)
{
    pp_state_t p;
    pp_out_t out;
    bonded_board(&p, 2);
    CHECK(!pp_pairable(&p));  // room for a bond does not make the board pairable
    CHECK_EQ(pp_pair_request(&p), PP_PAIR_DENY);
    owner_connects(&p, 0);
    CHECK(pp_enter_pairing_mode(&p, OWNER, 0, &out));
    CHECK_EQ(pp_pair_request(&p), PP_PAIR_ALLOW);  // room: nothing is replaced
    pp_connected(&p, OTHER, 1000, &out);
    pp_encrypted(&p, OTHER, true, true, 2, &out);
    CHECK(!p.window);
    CHECK(out.changed);
    CHECK_EQ(out.drop_count, 0);  // both bonded, both stay
    CHECK_EQ(p.bonds, 2);

    // Full now: the next window replaces the oldest.
    CHECK(pp_enter_pairing_mode(&p, OTHER, 5000, &out));
    CHECK_EQ(pp_pair_request(&p), PP_PAIR_REPLACE);
}

static void test_repeat_pairing(void)
{
    // A bonded phone that lost its keys pairs again: allowed only while pairable.
    pp_state_t p;
    pp_out_t out;
    bonded_board(&p, 1);
    pp_connected(&p, OWNER, 0, &out);
    CHECK_EQ(pp_pair_request(&p), PP_PAIR_DENY);

    pp_open_window(&p, 0, &out);
    CHECK(pp_pair_request(&p) != PP_PAIR_DENY);
    // The glue deletes the old bond, then the pairing stores a new one.
    pp_bonds_changed(&p, 0, &out);
    CHECK(out.changed);
    CHECK_EQ(pp_pair_request(&p), PP_PAIR_ALLOW);
    pp_encrypted(&p, OWNER, true, true, 1, &out);
    CHECK(!p.window);
    CHECK_EQ(out.drop_count, 0);
    CHECK(!pp_pairable(&p));
}

static void test_forget_bonds(void)
{
    pp_state_t p;
    pp_out_t out;
    bonded_board(&p, 1);
    owner_connects(&p, 0);
    CHECK(pp_enter_pairing_mode(&p, OWNER, 0, &out));
    pp_connected(&p, OTHER, 0, &out);

    pp_bonds_wiped(&p, &out);
    CHECK(out.changed);
    CHECK_EQ(out.drop_count, 2);
    CHECK(dropped(&out, OWNER));
    CHECK(dropped(&out, OTHER));
    CHECK(!p.window);
    CHECK(pp_pairable(&p));
    CHECK_EQ(p.bonds, 0);
    CHECK_EQ(pp_pair_request(&p), PP_PAIR_ALLOW);
    pp_disconnected(&p, OWNER, &out);
    pp_disconnected(&p, OTHER, &out);
    check_adv(&p, true, FLAGS_PAIRABLE);
}

static void test_boot_wipe_without_phone(void)
{
    pp_state_t p;
    pp_out_t out;
    bonded_board(&p, 1);
    pp_open_window(&p, 3000, &out);  // the 3 s hold fires first during the 10 s hold
    pp_bonds_wiped(&p, &out);
    CHECK_EQ(out.drop_count, 0);
    CHECK(!p.window);
    check_adv(&p, true, FLAGS_PAIRABLE);
}

static void test_window_on_fresh_board(void)
{
    // A window on a board without bonds admits two phones; when it times out
    // with still no bond, nobody is dropped (the board stays pairable).
    pp_state_t p;
    pp_out_t out;
    pp_init(&p, 1, 2, 0);
    pp_open_window(&p, 0, &out);
    CHECK(out.changed);
    pp_connected(&p, OTHER, 0, &out);
    check_adv(&p, true, FLAGS_PAIRABLE);
    pp_connected(&p, THIRD, 0, &out);
    check_adv(&p, false, FLAGS_PAIRABLE);  // both slots taken
    pp_tick(&p, PP_WINDOW_MS, &out);  // no bond: no extension, no drop
    CHECK(!p.window);
    CHECK(!p.window_extended);
    CHECK_EQ(out.drop_count, 0);
    CHECK(pp_pairable(&p));
    pp_disconnected(&p, THIRD, &out);
    check_adv(&p, true, FLAGS_PAIRABLE);
}

static void test_owner_displaces_stranger(void)
{
    // Bonded board, a stranger connects first: the board keeps advertising,
    // the owner connects as a second phone, and its encryption drops the
    // stranger and stops advertising.
    pp_state_t p;
    pp_out_t out;
    bonded_board(&p, 1);
    pp_connected(&p, OTHER, 0, &out);
    check_adv(&p, true, FLAGS_BONDED);
    pp_connected(&p, OWNER, 2000, &out);
    check_adv(&p, false, FLAGS_BONDED);  // stack limit
    pp_encrypted(&p, OWNER, true, false, 1, &out);
    CHECK_EQ(out.drop_count, 1);
    CHECK(dropped(&out, OTHER));
    CHECK(!out.changed);
    pp_disconnected(&p, OTHER, &out);
    check_adv(&p, false, FLAGS_BONDED);  // the owner is the only phone

    // Fresh board: a stranger holds a slot; the owner pairs as the second
    // phone and the stranger is dropped once the bond exists.
    pp_init(&p, 1, 2, 0);
    pp_connected(&p, OTHER, 0, &out);
    pp_connected(&p, OWNER, 1000, &out);
    pp_encrypted(&p, OWNER, true, true, 1, &out);
    CHECK(out.changed);
    CHECK_EQ(out.drop_count, 1);
    CHECK(dropped(&out, OTHER));

    // Inside a window an owner re-encryption does not drop the new phone.
    bonded_board(&p, 1);
    pp_open_window(&p, 0, &out);
    pp_connected(&p, OTHER, 0, &out);
    pp_connected(&p, OWNER, 1000, &out);
    pp_encrypted(&p, OWNER, true, false, 1, &out);
    CHECK_EQ(out.drop_count, 0);
    CHECK(p.window);
}

static void test_stale_bonded_flag(void)
{
    // A connection whose bond is deleted (repeat pairing, replace) or whose
    // encryption fails loses its bonded mark, so the drops apply to it again.
    pp_state_t p;
    pp_out_t out;
    bonded_board(&p, 1);
    owner_connects(&p, 0);
    CHECK(pp_link_bonded(&p, OWNER));
    check_adv(&p, false, FLAGS_BONDED);
    pp_open_window(&p, 1000, &out);
    pp_link_unbonded(&p, OWNER, 0, &out);  // repeat pairing deleted its bond
    CHECK(out.changed);
    CHECK(!pp_link_bonded(&p, OWNER));
    CHECK_EQ(p.bonds, 0);
    CHECK(!pp_link_bonded(&p, 99));
    // The retried pairing fails; a bond appears elsewhere later: the window
    // timeout and the grace now see OWNER as unbonded.
    pp_link_unbonded(&p, OWNER, 1, &out);
    pp_tick(&p, 1000 + PP_WINDOW_MS, &out);  // extension (unbonded phone present)
    pp_tick(&p, 1000 + PP_WINDOW_MS + PP_WINDOW_EXTEND_MS, &out);
    CHECK(!p.window);
    CHECK(dropped(&out, OWNER));

    // The wipe clears the marks too.
    bonded_board(&p, 1);
    owner_connects(&p, 0);
    pp_bonds_wiped(&p, &out);
    CHECK(!pp_link_bonded(&p, OWNER));
}

static void test_window_extension_once(void)
{
    // The extension happens once per window, and a reopen resets it.
    pp_state_t p;
    pp_out_t out;
    bonded_board(&p, 1);
    pp_open_window(&p, 0, &out);
    pp_tick(&p, PP_WINDOW_MS, &out);  // nobody connected: closes on time
    CHECK(!p.window);

    pp_open_window(&p, 100000, &out);
    pp_connected(&p, OTHER, 100000, &out);
    pp_tick(&p, 100000 + PP_WINDOW_MS, &out);
    CHECK(p.window_extended);
    pp_open_window(&p, 150000, &out);  // a new hold restarts the full window
    CHECK(!p.window_extended);
    CHECK_EQ(p.window_end_ms, 150000 + PP_WINDOW_MS);
    pp_tick(&p, 150000 + PP_WINDOW_MS, &out);
    CHECK(p.window);  // extended again (new window)
    // OTHER (connected at 100 s) reaches the 90 s pairable grace inside it.
    CHECK(dropped(&out, OTHER));
}

static void test_connection_table(void)
{
    pp_state_t p;
    pp_out_t out;
    pp_init(&p, 1, 9, 0);
    CHECK_EQ(p.max_conns, PP_MAX_CONNS);
    pp_connected(&p, 7, 0, &out);
    pp_connected(&p, 7, 0, &out);  // duplicate
    CHECK_EQ(p.conn_count, 1);
    pp_disconnected(&p, 99, &out);  // unknown
    CHECK_EQ(p.conn_count, 1);
    for (uint16_t h = 10; h < 10 + PP_MAX_CONNS - 1; h++) {
        pp_connected(&p, h, 0, &out);
        CHECK_EQ(out.drop_count, 0);
    }
    pp_connected(&p, 50, 0, &out);
    CHECK_EQ(out.drop_count, 1);
    CHECK(dropped(&out, 50));
    CHECK_EQ(p.conn_count, PP_MAX_CONNS);
    // Removing from the middle keeps the others.
    pp_disconnected(&p, 10, &out);
    CHECK_EQ(p.conn_count, PP_MAX_CONNS - 1);
    pp_bonds_wiped(&p, &out);
    CHECK_EQ(out.drop_count, PP_MAX_CONNS - 1);
    CHECK(dropped(&out, 7));
    CHECK(!dropped(&out, 10));

    pp_init(&p, 0, 0, 0);  // clamped
    CHECK_EQ(p.max_bonds, 1);
    CHECK_EQ(p.max_conns, 1);
}

static void test_out_cleared(void)
{
    pp_state_t p;
    pp_out_t out;
    memset(&out, 0xA5, sizeof(out));
    pp_init(&p, 1, 2, 1);
    pp_disconnected(&p, 1, &out);
    CHECK(!out.changed);
    CHECK_EQ(out.drop_count, 0);
}

// gap.c reports a connection to the policy at its first event, whichever
// NimBLE delivers first (CONNECT, ENC_CHANGE or SUBSCRIBE), and only once.
// Returns whether it reported the connection.
static bool gap_event(ct_state_t *t, pp_state_t *p, uint16_t handle, uint64_t now)
{
    pp_out_t out;
    if (!ct_seen(t, handle)) {
        return false;
    }
    pp_connected(p, handle, now, &out);
    return true;
}

static void test_encryption_before_connect(void)
{
    // Seen on hardware: the owner's phone reconnects and encrypts before
    // NimBLE reports CONNECT (it waits for the phone's version and features),
    // then its CCCDs are restored. The owner must count as bonded and keep
    // its connection past the 30 s grace for unbonded links.
    ct_state_t t;
    pp_state_t p;
    pp_out_t out;
    ct_init(&t);
    bonded_board(&p, 1);
    CHECK(gap_event(&t, &p, OWNER, 1680));  // ENC_CHANGE
    pp_encrypted(&p, OWNER, true, false, 1, &out);
    CHECK(pp_link_bonded(&p, OWNER));
    CHECK_EQ(p.conn_count, 1);
    check_adv(&p, false, FLAGS_BONDED);
    for (int i = 0; i < 6; i++) {
        CHECK(!gap_event(&t, &p, OWNER, 1690));  // SUBSCRIBE (restored CCCDs)
    }
    CHECK(!gap_event(&t, &p, OWNER, 1740));  // CONNECT
    CHECK_EQ(p.conn_count, 1);
    CHECK(pp_link_bonded(&p, OWNER));  // not reset by a second report
    pp_tick(&p, 1740 + PP_UNBONDED_GRACE_MS, &out);
    CHECK_EQ(out.drop_count, 0);
    pp_tick(&p, 600000, &out);
    CHECK_EQ(out.drop_count, 0);

    // Disconnect, then the same handle again in the usual order.
    CHECK(ct_gone(&t, OWNER));
    pp_disconnected(&p, OWNER, &out);
    CHECK(!ct_gone(&t, OWNER));
    CHECK(gap_event(&t, &p, OWNER, 700000));  // CONNECT
    CHECK_EQ(p.conn_count, 1);
    CHECK(!gap_event(&t, &p, OWNER, 700100));  // ENC_CHANGE
    pp_encrypted(&p, OWNER, true, false, 1, &out);
    CHECK_EQ(p.conn_count, 1);
    CHECK(pp_link_bonded(&p, OWNER));
    pp_tick(&p, 700000 + PP_UNBONDED_GRACE_MS, &out);
    CHECK_EQ(out.drop_count, 0);

    // A connection that went away before any event was never reported, so
    // its disconnection is not reported either.
    CHECK(!ct_gone(&t, OTHER));

    // A stranger reported at its first event still gets the grace drop.
    ct_init(&t);
    bonded_board(&p, 1);
    CHECK(gap_event(&t, &p, OTHER, 0));  // SUBSCRIBE (Service Changed, unencrypted)
    CHECK(!gap_event(&t, &p, OTHER, 50));  // CONNECT
    CHECK_EQ(p.conn_count, 1);
    pp_tick(&p, PP_UNBONDED_GRACE_MS, &out);
    CHECK(dropped(&out, OTHER));

    // Table limit: a handle beyond it is not tracked (and not reported twice).
    ct_init(&t);
    for (uint16_t h = 0; h < CT_MAX_CONNS; h++) {
        CHECK(ct_seen(&t, h));
    }
    CHECK(!ct_seen(&t, CT_MAX_CONNS));
    CHECK(ct_gone(&t, 1));
    CHECK(ct_seen(&t, CT_MAX_CONNS));
    CHECK(ct_gone(&t, 0));
    CHECK(ct_gone(&t, 2));
    CHECK(ct_gone(&t, 3));
    CHECK(ct_gone(&t, CT_MAX_CONNS));
    CHECK_EQ(t.count, 0);
}

int main(void)
{
    test_fresh_board();
    test_first_bond();
    test_stranger_outside_window();
    test_button_window_timeout();
    test_window_restart();
    test_enter_pairing_mode_and_replace();
    test_window_ends_with_unbonded_second();
    test_new_bond_drops_other_unbonded();
    test_more_bonds();
    test_repeat_pairing();
    test_forget_bonds();
    test_boot_wipe_without_phone();
    test_window_on_fresh_board();
    test_owner_displaces_stranger();
    test_stale_bonded_flag();
    test_window_extension_once();
    test_connection_table();
    test_out_cleared();
    test_encryption_before_connect();
    if (s_failures) {
        printf("pairing: %d of %d checks FAILED\n", s_failures, s_checks);
        return 1;
    }
    printf("pairing: OK (%d checks)\n", s_checks);
    return 0;
}
