/* SPDX-License-Identifier: MIT */
#include "bt_core.h"

void bt_adv_invalidate(struct bt_adv_state *s)
{
    s->confirmed_sets = 0;
    s->can_confirm = 0;
}

void bt_adv_reset(struct bt_adv_state *s)
{
    /* A caller may still be waiting for the pre-reset ticket. */
    uint64_t serial = s->serial;
    *s = (struct bt_adv_state){0};
    s->serial = serial;
}

void bt_adv_command(struct bt_adv_state *s, uint16_t opcode, const uint8_t *p, size_t len)
{
    switch (opcode) {
    case 0x0c03: /* Reset */
        bt_adv_reset(s);
        break;
    case 0x2005: /* Set Random Address */
        if (len != 6) break;
        for (size_t i = 0; i < 6; i++) {
            if (s->address[i] != p[i]) bt_adv_invalidate(s);
            s->address[i] = p[i];
        }
        s->have_address = 1;
        break;
    case 0x2035: /* Let the host manage this set and see its own reply. */
        if (len != 7) break;
        if (p[0] < 16) s->native_sets |= (uint16_t)(1u << p[0]);
        if (s->pending) {
            /* The opcode alone cannot identify these overlapping replies. */
            s->native_replies++;
            s->pending = 0;
        }
        s->native_replies++;
        bt_adv_invalidate(s);
        break;
    case 0x2036: /* Set Extended Advertising Parameters */
    case 0x203c: { /* Remove Advertising Set */
        if (!len || p[0] >= 16 || (opcode == 0x2036 && len < 11)) break;
        uint16_t bit = (uint16_t)(1u << p[0]);
        if (opcode == 0x203c) s->native_sets &= (uint16_t)~bit;
        if (opcode == 0x2036 && p[10] == 1) s->random_sets |= bit;
        else {
            s->random_sets &= (uint16_t)~bit;
            s->confirmed_sets &= (uint16_t)~bit;
            if (s->handle == p[0]) s->can_confirm = 0;
        }
        break;
    }
    case 0x203d: /* Clear Advertising Sets */
        s->random_sets = s->native_sets = 0;
        bt_adv_invalidate(s);
        break;
    }
}

uint64_t bt_adv_begin(struct bt_adv_state *s, uint8_t handle, int cache, uint8_t address[6])
{
    if (handle >= 16 || !s->have_address || s->native_replies || s->pending) return 0;
    uint16_t bit = (uint16_t)(1u << handle);
    if (!(s->random_sets & bit) || (s->native_sets & bit) || (cache && (s->confirmed_sets & bit))) return 0;
    for (size_t i = 0; i < 6; i++) address[i] = s->address[i];
    s->confirmed_sets &= (uint16_t)~bit;
    s->handle = handle;
    s->can_confirm = 1;
    s->pending = ++s->serial;
    return s->pending;
}

int bt_adv_complete(struct bt_adv_state *s, uint8_t status)
{
    if (s->native_replies) {
        s->native_replies--;
        return 0;
    }
    if (!s->pending) return 0;
    if (!status && s->can_confirm) s->confirmed_sets |= (uint16_t)(1u << s->handle);
    s->pending = 0;
    return 1;
}
