#include "conn_track.h"

#include <string.h>

void ct_init(ct_state_t *t)
{
    memset(t, 0, sizeof(*t));
}

static int find(const ct_state_t *t, uint16_t handle)
{
    for (int i = 0; i < t->count; i++) {
        if (t->handles[i] == handle) {
            return i;
        }
    }
    return -1;
}

bool ct_seen(ct_state_t *t, uint16_t handle)
{
    if (find(t, handle) >= 0 || t->count >= CT_MAX_CONNS) {
        return false;
    }
    t->handles[t->count++] = handle;
    return true;
}

bool ct_gone(ct_state_t *t, uint16_t handle)
{
    int i = find(t, handle);
    if (i < 0) {
        return false;
    }
    t->handles[i] = t->handles[--t->count];
    return true;
}
