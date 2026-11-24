/* SPDX-License-Identifier: MIT */
/* HCI rewriting: emulate the LE resolving list in the host, so a controller
 * without LL Privacy looks like one that has it. See docs/DESIGN.md. */
#include "bt_core.h"

#define OP(ogf, ocf) ((uint16_t)(((ogf) << 10) | (ocf)))

#define OP_READ_LOCAL_COMMANDS   OP(0x04, 0x002)
#define OP_LE_READ_FEATURES      OP(0x08, 0x003)
#define OP_LE_CREATE_CONN        OP(0x08, 0x00D)
#define OP_LE_ADD_FAL            OP(0x08, 0x011)
#define OP_LE_DEL_FAL            OP(0x08, 0x012)
#define OP_LE_ADD_RL             OP(0x08, 0x027)
#define OP_LE_DEL_RL             OP(0x08, 0x028)
#define OP_LE_CLEAR_RL           OP(0x08, 0x029)
#define OP_LE_RL_SIZE            OP(0x08, 0x02A)
#define OP_LE_READ_PEER_RPA      OP(0x08, 0x02B)
#define OP_LE_READ_LOCAL_RPA     OP(0x08, 0x02C)
#define OP_LE_SET_RES_ENABLE     OP(0x08, 0x02D)
#define OP_LE_SET_RPA_TIMEOUT    OP(0x08, 0x02E)
#define OP_LE_EXT_CREATE_CONN    OP(0x08, 0x043)
#define OP_LE_SET_PRIVACY_MODE   OP(0x08, 0x04E)

#define EVT_CMD_COMPLETE      0x0E
#define EVT_CMD_STATUS        0x0F
#define EVT_LE_META           0x3E
#define LE_CONN_COMPLETE      0x01
#define LE_ADV_REPORT         0x02
#define LE_ENH_CONN_COMPLETE  0x0A
#define LE_EXT_ADV_REPORT     0x0D

static void bcopy6(uint8_t *d, const uint8_t *s) { for (int i = 0; i < 6; i++) d[i] = s[i]; }
static int  beq6(const uint8_t *a, const uint8_t *b)
{
    for (int i = 0; i < 6; i++) if (a[i] != b[i]) return 0;
    return 1;
}

void bt_ctx_init(struct bt_ctx *c)
{
    uint8_t *p = (uint8_t *)c;
    for (size_t i = 0; i < sizeof(*c); i++) p[i] = 0;
}

static struct bt_rl_entry *rl_find(struct bt_ctx *c, uint8_t type, const uint8_t *addr)
{
    for (int i = 0; i < BT_RL_SIZE; i++)
        if (c->rl[i].valid && c->rl[i].id_type == (type & 1) && beq6(c->rl[i].id_addr, addr))
            return &c->rl[i];
    return NULL;
}

/* The entry this private address belongs to: the one whose IRK resolves it,
 * or the one that was handed this very address from outside.
 *
 * The exact match comes first and is the only test an entry with no IRK gets.
 * A zero key is still a key as far as ah() is concerned, so a mapping-only
 * entry must never be offered to it. */
static struct bt_rl_entry *rl_resolve(struct bt_ctx *c, const uint8_t *rpa)
{
    for (int i = 0; i < BT_RL_SIZE; i++)
        if (c->rl[i].valid && c->rl[i].have_rpa && beq6(c->rl[i].rpa, rpa))
            return &c->rl[i];
    for (int i = 0; i < BT_RL_SIZE; i++)
        if (c->rl[i].valid && c->rl[i].have_irk && bt_rpa_matches(c->rl[i].irk, rpa))
            return &c->rl[i];
    return NULL;
}

/* Remember the answer this command's Command Complete should carry.
 * ret may be NULL for a status-only reply. */
static void answer(struct bt_ctx *c, uint16_t opcode, const uint8_t *ret, uint8_t ret_len)
{
    struct bt_pending *p = NULL;

    if (ret_len > BT_REPLY_MAX) return;
    for (int i = 0; i < BT_PENDING_MAX; i++)
        if (c->pending[i].used && c->pending[i].opcode == opcode) { p = &c->pending[i]; break; }
    if (p == NULL)
        for (int i = 0; i < BT_PENDING_MAX && p == NULL; i++)
            if (!c->pending[i].used) p = &c->pending[i];
    if (p == NULL) return;                  /* too many in flight: let the real reply through */

    p->used = 1;
    p->opcode = opcode;
    p->len = ret_len;
    for (uint8_t i = 0; i < ret_len; i++) p->reply[i] = ret[i];
}

static struct bt_pending *take_pending(struct bt_ctx *c, uint16_t opcode)
{
    for (int i = 0; i < BT_PENDING_MAX; i++)
        if (c->pending[i].used && c->pending[i].opcode == opcode) {
            c->pending[i].used = 0;
            return &c->pending[i];
        }
    return NULL;
}

/* The address the controller should really be told about: an identity address
 * BroadTek has an IRK and a current RPA for is replaced by that RPA, so the
 * controller's own accept list and connection attempts work unchanged. */
static int substitute(struct bt_ctx *c, uint8_t *type, uint8_t *addr)
{
    struct bt_rl_entry *e = rl_find(c, *type, addr);

    if (e == NULL || !e->have_rpa) return 0;
    *type = 0x01;                            /* random */
    bcopy6(addr, e->rpa);
    c->substituted++;
    return 1;
}

/* Put an identity and its IRK into the resolving list, replacing any entry
 * for the same identity so a re-install refreshes rather than fills the list.
 *
 * Nothing in the HCI path can do this: macOS never sends the resolving-list
 * commands that would (docs/STATE.md §3), so the key arrives from outside --
 * from a bonding exchange on the ACL link, or read out of the keychain and
 * handed down. Both land here. */
int bt_rl_install(struct bt_ctx *c, uint8_t id_type, const uint8_t id_addr[6],
                  const uint8_t irk[16])
{
    struct bt_rl_entry *e = rl_find(c, id_type, id_addr);

    if (e == NULL)
        for (int i = 0; i < BT_RL_SIZE && e == NULL; i++)
            if (!c->rl[i].valid) e = &c->rl[i];
    if (e == NULL) return 0;

    /* The same key again (the helper re-installs it every few minutes):
     * keep the sighting, so resolution does not lapse until the next advert. */
    int same = e->valid && e->have_irk;
    for (int i = 0; same && i < 16; i++) same = e->irk[i] == irk[i];
    if (same) { e->pinned = 1; return 1; }

    e->valid = 1;
    e->pinned = 1;
    e->id_type = (uint8_t)(id_type & 1);
    bcopy6(e->id_addr, id_addr);
    for (int i = 0; i < 16; i++) e->irk[i] = irk[i];
    e->have_irk = 1;
    e->have_rpa = 0;                    /* no sighting yet for this key */
    return 1;
}

int bt_rl_install_map(struct bt_ctx *c, uint8_t id_type, const uint8_t id_addr[6],
                      const uint8_t rpa[6])
{
    struct bt_rl_entry *e = rl_find(c, id_type, id_addr);

    if (e == NULL)
        for (int i = 0; i < BT_RL_SIZE && e == NULL; i++)
            if (!c->rl[i].valid) e = &c->rl[i];
    if (e == NULL) return 0;

    e->valid = 1;
    e->id_type = (uint8_t)(id_type & 1);
    bcopy6(e->id_addr, id_addr);
    /* An address, not a key: if this identity had an IRK it keeps it, and the
     * handed-down address simply becomes its current sighting. */
    bcopy6(e->rpa, rpa);
    e->have_rpa = 1;
    e->pinned = 1;
    return 1;
}

enum bt_verdict bt_hci_on_command(struct bt_ctx *c, uint8_t *pkt, size_t len)
{
    if (len < 3) return BT_FORWARD;

    uint16_t opcode = (uint16_t)(pkt[0] | (pkt[1] << 8));
    uint8_t plen = pkt[2];
    uint8_t *p = &pkt[3];

    if (len < (size_t)3 + plen) return BT_FORWARD;

    switch (opcode) {
    case OP_LE_ADD_RL: {                     /* type, addr[6], peer IRK[16], local IRK[16] */
        if (plen < 39) break;
        struct bt_rl_entry *e = rl_find(c, p[0], &p[1]);
        if (e == NULL) {
            for (int i = 0; i < BT_RL_SIZE && e == NULL; i++)
                if (!c->rl[i].valid) e = &c->rl[i];
            if (e != NULL) e->pinned = 0;    /* a free slot: bluetoothd's own entry */
        }
        if (e == NULL) break;                /* full: the controller's refusal stands */
        e->valid = 1;
        e->id_type = p[0] & 1;
        bcopy6(e->id_addr, &p[1]);
        for (int i = 0; i < 16; i++) e->irk[i] = p[7 + i];
        e->have_irk = 1;
        e->have_rpa = 0;
        answer(c, opcode, NULL, 0);
        break;
    }
    case OP_LE_DEL_RL: {
        if (plen < 7) break;
        struct bt_rl_entry *e = rl_find(c, p[0], &p[1]);
        if (e != NULL && !e->pinned) e->valid = 0;
        answer(c, opcode, NULL, 0);
        break;
    }
    case OP_LE_CLEAR_RL:
        /* bluetoothd clears the list as it starts, which can come after the
         * key helper has already installed the Watch's key at boot. */
        for (int i = 0; i < BT_RL_SIZE; i++) if (!c->rl[i].pinned) c->rl[i].valid = 0;
        answer(c, opcode, NULL, 0);
        break;
    case OP_LE_RL_SIZE: {
        uint8_t size = BT_RL_SIZE;
        answer(c, opcode, &size, 1);
        break;
    }
    case OP_LE_READ_PEER_RPA: {
        if (plen < 7) break;
        struct bt_rl_entry *e = rl_find(c, p[0], &p[1]);
        uint8_t rpa[6] = {0};
        if (e != NULL && e->have_rpa) bcopy6(rpa, e->rpa);
        answer(c, opcode, rpa, 6);
        break;
    }
    case OP_LE_READ_LOCAL_RPA: {             /* no local privacy: all zeroes */
        uint8_t zero[6] = {0};
        answer(c, opcode, zero, 6);
        break;
    }
    case OP_LE_SET_RES_ENABLE:
        if (plen < 1) break;
        c->res_enabled = p[0] ? 1 : 0;
        answer(c, opcode, NULL, 0);
        break;
    case OP_LE_SET_RPA_TIMEOUT:
    case OP_LE_SET_PRIVACY_MODE:
        answer(c, opcode, NULL, 0);
        break;

    /* The accept list holds the peer's current RPA in place of its identity,
     * so the controller matches the advertisements it actually hears. */
    case OP_LE_ADD_FAL:
    case OP_LE_DEL_FAL:
        if (plen < 7) break;
        substitute(c, &p[0], &p[1]);
        break;

    case OP_LE_CREATE_CONN:
        /* interval(2) window(2) policy(1) peer_type(1) peer[6] own_type(1) ... */
        if (plen < 13) break;
        if (p[4] == 0) substitute(c, &p[5], &p[6]);
        if (p[12] >= 0x02) p[12] = (uint8_t)(p[12] & 1);   /* own type 2/3 -> 0/1 */
        break;

    case OP_LE_EXT_CREATE_CONN:
        /* policy(1) own_type(1) peer_type(1) peer[6] phys(1) ... */
        if (plen < 10) break;
        if (p[0] == 0) substitute(c, &p[2], &p[3]);
        if (p[1] >= 0x02) p[1] = (uint8_t)(p[1] & 1);
        break;

    default:
        break;
    }
    return BT_FORWARD;
}

/* One advertising report's address field: resolve in place, return 1 if the
 * address was replaced by the identity. */
static int resolve_addr(struct bt_ctx *c, uint8_t *type, uint8_t *addr)
{
    if (*type != 0x01) return 0;             /* only a random address can be an RPA */
    if ((addr[5] & 0xc0) != 0x40) return 0;

    struct bt_rl_entry *e = rl_resolve(c, addr);
    if (e == NULL) return 0;

    /* A match: this advertisement came from a device BroadTek holds a key
     * for. Counted whether or not the report is rewritten, because the match
     * is what says the key is right. */
    c->resolved++;
    if (!e->have_rpa || !beq6(e->rpa, addr)) {
        bcopy6(e->rpa, addr);
        e->have_rpa = 1;
    }
    if (!c->res_enabled) return 0;           /* track it, but report it unchanged */
    bcopy6(addr, e->id_addr);
    *type = (uint8_t)(e->id_type ? 0x03 : 0x02);
    return 1;
}

enum bt_verdict bt_hci_on_event(struct bt_ctx *c, uint8_t *pkt, size_t len,
                                size_t cap, size_t *out_len)
{
    *out_len = len;
    if (len < 2) return BT_FORWARD;

    uint8_t code = pkt[0], plen = pkt[1];
    uint8_t *p = &pkt[2];

    if (len < (size_t)2 + plen) return BT_FORWARD;

    /* An unsupported command may end with either event. Preserve the
     * controller's command credits and complete the opcode the host issued. */
    int complete = code == EVT_CMD_COMPLETE && plen >= 3;
    int refused = code == EVT_CMD_STATUS && plen >= 4 && p[0] != 0;
    if (complete || refused) {
        uint8_t *op = &p[complete ? 1 : 2];
        uint16_t opcode = (uint16_t)(op[0] | (op[1] << 8));
        struct bt_pending *pend = take_pending(c, opcode);
        if (pend != NULL) {
            size_t need = 6u + pend->len;
            if (need <= cap) {
                uint8_t credits = p[complete ? 0 : 1];
                pkt[0] = EVT_CMD_COMPLETE;
                pkt[1] = (uint8_t)(4 + pend->len);
                p[0] = credits;
                p[1] = (uint8_t)opcode;
                p[2] = (uint8_t)(opcode >> 8);
                p[3] = 0;
                for (uint8_t i = 0; i < pend->len; i++) p[4 + i] = pend->reply[i];
                *out_len = need;
            }
            return BT_FORWARD;
        }
        if (!complete) return BT_FORWARD;

        if (opcode == OP_LE_READ_FEATURES && plen >= 12 && p[3] == 0x00)
            p[4] |= 0x40;                            /* LL Privacy (feature bit 6) */
        if (opcode == OP_READ_LOCAL_COMMANDS && plen >= 68 && p[3] == 0x00) {
            uint8_t *cmds = &p[4];
            cmds[34] |= 0xe0;                        /* add/remove/clear resolving list */
            cmds[35] |= 0x1f;                        /* size, read RPAs, enable, timeout */
            cmds[39] |= 0x04;                        /* LE Set Privacy Mode */
        }
        return BT_FORWARD;
    }

    if (code != EVT_LE_META || plen < 1) return BT_FORWARD;

    uint8_t sub = p[0];
    uint8_t *q = &p[1];
    size_t avail = (size_t)plen - 1;

    if (sub == LE_ADV_REPORT) {
        if (avail < 1) return BT_FORWARD;
        uint8_t n = q[0];
        size_t off = 1;
        for (uint8_t i = 0; i < n; i++) {
            if (off + 9 > avail) break;
            uint8_t dlen = q[off + 8];
            if (off + 9 + dlen + 1 > avail) break;
            resolve_addr(c, &q[off + 1], &q[off + 2]);
            off += 9 + dlen + 1;
        }
        return BT_FORWARD;
    }

    if (sub == LE_EXT_ADV_REPORT) {
        if (avail < 1) return BT_FORWARD;
        uint8_t n = q[0];
        size_t off = 1;
        for (uint8_t i = 0; i < n; i++) {
            if (off + 24 > avail) break;
            uint8_t dlen = q[off + 23];
            if (off + 24 + dlen > avail) break;
            resolve_addr(c, &q[off + 2], &q[off + 3]);
            off += 24 + dlen;
        }
        return BT_FORWARD;
    }

    if (sub == LE_CONN_COMPLETE) {
        /* status(1) handle(2) role(1) peer_type(1) peer[6] ... */
        if (avail < 11 || q[0] != 0x00) return BT_FORWARD;
        c->conn_events++;
        if (q[4] == 0x01) {
            struct bt_rl_entry *e = rl_resolve(c, &q[5]);
            if (e != NULL) {
                bcopy6(&q[5], e->id_addr);
                q[4] = e->id_type;
                c->conn_translated++;
            }
        }
        return BT_FORWARD;
    }

    if (sub == LE_ENH_CONN_COMPLETE) {
        /* status(1) handle(2) role(1) peer_type(1) peer[6] local_rpa[6] peer_rpa[6] ... */
        if (avail < 23 || q[0] != 0x00) return BT_FORWARD;
        c->conn_events++;
        if (q[4] == 0x01) {
            struct bt_rl_entry *e = rl_resolve(c, &q[5]);
            if (e != NULL) {
                c->conn_translated++;
                bcopy6(&q[17], &q[5]);               /* Peer_Resolvable_Private_Address */
                bcopy6(&q[5], e->id_addr);
                q[4] = (uint8_t)(e->id_type ? 0x03 : 0x02);
            }
        }
        return BT_FORWARD;
    }

    return BT_FORWARD;
}
