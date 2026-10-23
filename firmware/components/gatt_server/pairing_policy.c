#include "pairing_policy.h"

#include <string.h>

static void clear(pp_out_t *out)
{
    memset(out, 0, sizeof(*out));
}

static void drop(pp_out_t *out, uint16_t handle)
{
    for (int i = 0; i < out->drop_count; i++) {
        if (out->drop[i] == handle) {
            return;
        }
    }
    if (out->drop_count < PP_MAX_CONNS) {
        out->drop[out->drop_count++] = handle;
    }
}

static pp_conn_t *find(pp_state_t *p, uint16_t handle)
{
    for (int i = 0; i < p->conn_count; i++) {
        if (p->conns[i].handle == handle) {
            return &p->conns[i];
        }
    }
    return NULL;
}

static bool any_conn(const pp_state_t *p, bool bonded)
{
    for (int i = 0; i < p->conn_count; i++) {
        if (p->conns[i].bonded == bonded) {
            return true;
        }
    }
    return false;
}

static void drop_unbonded(pp_state_t *p, pp_out_t *out)
{
    for (int i = 0; i < p->conn_count; i++) {
        if (!p->conns[i].bonded) {
            drop(out, p->conns[i].handle);
        }
    }
}

// Window closed (timeout or new bond): once the board has a bond, phones that
// have not bonded lose their place (protocol.md: "when it ends, an unbonded
// second connection is dropped").
static void close_window(pp_state_t *p, pp_out_t *out)
{
    p->window = false;
    out->changed = true;
    if (p->bonds > 0) {
        drop_unbonded(p, out);
    }
}

void pp_init(pp_state_t *p, int max_bonds, int max_conns, int bonds)
{
    memset(p, 0, sizeof(*p));
    p->max_bonds = max_bonds < 1 ? 1 : max_bonds;
    p->max_conns = max_conns < 1 ? 1 : (max_conns > PP_MAX_CONNS ? PP_MAX_CONNS : max_conns);
    p->bonds = bonds;
}

bool pp_pairable(const pp_state_t *p)
{
    return p->bonds == 0 || p->window;
}

pp_pair_t pp_pair_request(const pp_state_t *p)
{
    if (!pp_pairable(p)) {
        return PP_PAIR_DENY;
    }
    return p->bonds >= p->max_bonds ? PP_PAIR_REPLACE : PP_PAIR_ALLOW;
}

pp_adv_t pp_advertising(const pp_state_t *p)
{
    pp_adv_t adv = {0};
    // Once a bonded phone is connected it is the only one, except that a
    // window admits a second phone.
    int limit = (p->window || !any_conn(p, true)) ? p->max_conns : 1;
    adv.on = p->conn_count < limit;
    if (pp_pairable(p)) {
        adv.flags |= PP_ADV_FLAG_PAIRABLE;
    }
    if (p->bonds > 0) {
        adv.flags |= PP_ADV_FLAG_HAS_BOND;
    }
    return adv;
}

void pp_connected(pp_state_t *p, uint16_t handle, uint64_t now_ms, pp_out_t *out)
{
    clear(out);
    if (find(p, handle) != NULL) {
        return;
    }
    if (p->conn_count >= PP_MAX_CONNS) {
        drop(out, handle);
        return;
    }
    p->conns[p->conn_count++] = (pp_conn_t){.handle = handle, .bonded = false, .since_ms = now_ms};
}

void pp_disconnected(pp_state_t *p, uint16_t handle, pp_out_t *out)
{
    clear(out);
    pp_conn_t *c = find(p, handle);
    if (c == NULL) {
        return;
    }
    *c = p->conns[--p->conn_count];
}

void pp_encrypted(pp_state_t *p, uint16_t handle, bool bonded, bool new_bond, int bonds, pp_out_t *out)
{
    clear(out);
    pp_conn_t *c = find(p, handle);
    if (c != NULL) {
        c->bonded = bonded;
    }
    if (bonds != p->bonds) {
        p->bonds = bonds;
        out->changed = true;
    }
    if (new_bond && p->window) {
        close_window(p, out);
    }
    // The owner is here: strangers lose their place (outside a window).
    if (bonded && !p->window) {
        drop_unbonded(p, out);
    }
}

void pp_link_unbonded(pp_state_t *p, uint16_t handle, int bonds, pp_out_t *out)
{
    clear(out);
    pp_conn_t *c = find(p, handle);
    if (c != NULL) {
        c->bonded = false;
    }
    if (bonds != p->bonds) {
        p->bonds = bonds;
        out->changed = true;
    }
}

bool pp_link_bonded(const pp_state_t *p, uint16_t handle)
{
    for (int i = 0; i < p->conn_count; i++) {
        if (p->conns[i].handle == handle) {
            return p->conns[i].bonded;
        }
    }
    return false;
}

void pp_bonds_changed(pp_state_t *p, int bonds, pp_out_t *out)
{
    clear(out);
    if (bonds != p->bonds) {
        p->bonds = bonds;
        out->changed = true;
    }
}

void pp_open_window(pp_state_t *p, uint64_t now_ms, pp_out_t *out)
{
    clear(out);
    out->changed = !p->window;
    p->window = true;
    p->window_extended = false;
    p->window_end_ms = now_ms + PP_WINDOW_MS;
}

bool pp_enter_pairing_mode(pp_state_t *p, uint16_t handle, uint64_t now_ms, pp_out_t *out)
{
    clear(out);
    const pp_conn_t *c = find(p, handle);
    if (c == NULL || !c->bonded) {
        return false;
    }
    pp_open_window(p, now_ms, out);
    return true;
}

void pp_bonds_wiped(pp_state_t *p, pp_out_t *out)
{
    clear(out);
    p->bonds = 0;
    p->window = false;
    out->changed = true;
    for (int i = 0; i < p->conn_count; i++) {
        p->conns[i].bonded = false;
        drop(out, p->conns[i].handle);
    }
}

void pp_tick(pp_state_t *p, uint64_t now_ms, pp_out_t *out)
{
    clear(out);
    if (p->window && now_ms >= p->window_end_ms) {
        if (!p->window_extended && p->bonds > 0 && any_conn(p, false)) {
            p->window_extended = true;
            p->window_end_ms = now_ms + PP_WINDOW_EXTEND_MS;
        } else {
            close_window(p, out);
        }
    }
    uint64_t grace = pp_pairable(p) ? PP_PAIRABLE_GRACE_MS : PP_UNBONDED_GRACE_MS;
    for (int i = 0; i < p->conn_count; i++) {
        const pp_conn_t *c = &p->conns[i];
        if (!c->bonded && now_ms - c->since_ms >= grace) {
            drop(out, c->handle);
        }
    }
}
