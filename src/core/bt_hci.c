/* SPDX-License-Identifier: MIT */
/* HCI rewriting: emulate the LE resolving list in the host, so a controller
 * without LL Privacy looks like one that has it. See docs/DESIGN.md. */
#include "bt_core.h"

#define OP(ogf, ocf) ((uint16_t)(((ogf) << 10) | (ocf)))

#define OP_READ_LOCAL_COMMANDS   OP(0x04, 0x002)
#define OP_LE_READ_FEATURES      OP(0x08, 0x003)
#define OP_LE_CREATE_CONN        OP(0x08, 0x00D)
#define OP_LE_CLEAR_FAL          OP(0x08, 0x010)
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

#define EVT_DISCONN_COMPLETE  0x05
#define EVT_ENCRYPT_CHANGE    0x08
#define EVT_KEY_REFRESH       0x30
#define EVT_ENCRYPT_CHANGE_V2 0x59
#define EVT_CMD_COMPLETE      0x0E
#define EVT_CMD_STATUS        0x0F
#define EVT_LE_META           0x3E
#define LE_CONN_COMPLETE      0x01
#define LE_ADV_REPORT         0x02
#define LE_ENH_CONN_COMPLETE  0x0A
#define LE_EXT_ADV_REPORT     0x0D

unsigned bt_link_procedures(const uint8_t *event, size_t len)
{
    /* LE Read Remote Features Complete, followed by eight feature octets. */
    if (len < 14 || event[0] != EVT_LE_META || event[1] != 12 ||
        event[2] != 0x04 || event[3] != 0) return 0;
    return ((event[6] & 0x20) ? BT_LINK_DLE : 0) |
           ((event[7] & 0x01) ? BT_LINK_PHY : 0);
}

int bt_link_enable_reports(uint8_t *command, size_t len)
{
    if (len < 11 || command[0] != 0x01 || command[1] != 0x20 || command[2] != 8)
        return 0;
    if ((command[3] & 0x40) && (command[4] & 0x08)) return 0;
    command[3] |= 0x40; /* LE Data Length Change */
    command[4] |= 0x08; /* LE PHY Update Complete */
    return 1;
}

size_t bt_link_request(enum bt_link_procedure procedure, uint16_t handle, uint8_t out[10])
{
    if (handle > 0x0eff) return 0;
    /* LE Set PHY, no TX or RX preference. Even an unchanged PHY completes
     * with a controller-generated PHY Update Complete event (Core, 7.8.49). */
    const uint8_t phy[] = {0x32, 0x20, 7, 0, 0, 3, 0, 0, 0, 0};
    /* LE Set Data Length: request 251 octets, 2120 us. The controller alone
     * reports the negotiated values when the effective lengths change. */
    const uint8_t dle[] = {0x22, 0x20, 6, 0, 0, 0xfb, 0x00, 0x48, 0x08};
    const uint8_t *src;
    size_t n;
    switch (procedure) {
    case BT_LINK_PHY: src = phy; n = sizeof(phy); break;
    case BT_LINK_DLE: src = dle; n = sizeof(dle); break;
    default: return 0;
    }
    for (size_t i = 0; i < n; i++) out[i] = src[i];
    out[3] = (uint8_t)handle;
    out[4] = (uint8_t)(handle >> 8);
    return n;
}

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
 * controller's own accept list and connection attempts work unchanged.
 *
 * Never a guess: an address that has not been proven stands *beside* the
 * identity on the accept list (bt_guess_before_command), never in its place,
 * so a wrong guess cannot lock the real device out. */
static int substitute(struct bt_ctx *c, uint8_t *type, uint8_t *addr)
{
    struct bt_rl_entry *e = rl_find(c, *type, addr);

    if (e == NULL || !e->have_rpa || e->guess) return 0;
    *type = 0x01;                            /* random */
    bcopy6(addr, e->rpa);
    c->substituted++;
    return 1;
}

/* ---- Guessing and proving ------------------------------------------------
 *
 * The iPhone has no IRK on this Mac (docs/STATE.md), so nothing here can
 * derive its current private address. macOS recognises it from the NearbyInfo
 * tag, but that join happens inside bluetoothd and only reaches the outside
 * as a log line. What BroadTek *can* see is enough to do without it:
 *
 *   - every private address an Apple device is advertising NearbyInfo from,
 *     with its signal strength and when it appeared (the candidates);
 *   - bluetoothd encrypting the link with the identity's own bond key the
 *     moment it connects, and the controller reporting whether that worked.
 *
 * Only the device that owns the identity holds that key, so the encryption
 * result is a proof: a link to the wrong device fails to encrypt, and the
 * right one succeeds. So when bluetoothd asks for an identity BroadTek has no
 * address for, the likeliest candidate is put on the accept list next to it;
 * the encryption result then confirms it (and later accept-list adds carry it
 * in the identity's place, as with a key) or rejects it, and the next
 * candidate gets its turn. When the proven address rotates, the address that
 * appears as it goes quiet is tried first.
 *
 * An identity that never proves anything -- a non-Apple peripheral asking to
 * be reconnected -- stops being guessed for after BT_GUESS_GIVE_UP failures,
 * and since its own address was never taken off the accept list, it was able
 * to connect throughout. */

#define BT_GUESS_GIVE_UP 3

static int fresh(const struct bt_ctx *c, uint32_t t, uint32_t window)
{
    return (uint32_t)(c->now - t) <= window;
}

/* Apple's manufacturer data (4c 00) carrying a NearbyInfo record (0x10). */
static int has_nearby_info(const uint8_t *d, size_t n)
{
    size_t off = 0;
    while (off + 2 <= n) {
        uint8_t adlen = d[off];
        if (adlen == 0 || off + 1 + adlen > n) return 0;
        if (d[off + 1] == 0xff && adlen >= 4 && d[off + 2] == 0x4c && d[off + 3] == 0x00) {
            size_t s = off + 4, end = off + 1 + adlen;
            while (s + 2 <= end) {
                if (s + 2 + d[s + 1] > end) break;
                if (d[s] == 0x10) return 1;
                s += 2u + d[s + 1];
            }
        }
        off += 1u + adlen;
    }
    return 0;
}

static struct bt_candidate *cand_find(struct bt_ctx *c, const uint8_t *addr)
{
    for (int i = 0; i < BT_CAND_MAX; i++)
        if (c->cand[i].used && beq6(c->cand[i].addr, addr)) return &c->cand[i];
    return NULL;
}

static void note_candidate(struct bt_ctx *c, int connectable, uint8_t type,
                           const uint8_t *addr, const uint8_t *data, size_t dlen, int8_t rssi)
{
    if (!c->guessing || type != 0x01 || (addr[5] & 0xc0) != 0x40) return;
    if (!has_nearby_info(data, dlen)) return;

    struct bt_candidate *k = cand_find(c, addr);
    if (k == NULL) {
        for (int i = 0; i < BT_CAND_MAX && k == NULL; i++)
            if (!c->cand[i].used) k = &c->cand[i];
        if (k == NULL) {                     /* full: the longest silent goes */
            k = &c->cand[0];
            for (int i = 1; i < BT_CAND_MAX; i++)
                if ((uint32_t)(c->now - c->cand[i].seen) > (uint32_t)(c->now - k->seen))
                    k = &c->cand[i];
        }
        k->used = 1;
        k->connectable = 0;
        bcopy6(k->addr, addr);
        k->first_seen = c->now;
    }
    k->seen = c->now;
    k->rssi = rssi == 127 ? -127 : rssi;     /* 127: not available */
    if (connectable) k->connectable = 1;

    for (int i = 0; i < BT_RL_SIZE; i++)
        if (c->rl[i].valid && c->rl[i].have_rpa && beq6(c->rl[i].rpa, addr))
            c->rl[i].rpa_seen = c->now;
}

static int rejected_for(const struct bt_rl_entry *e, const uint8_t *addr)
{
    for (int i = 0; i < e->n_rejected && i < BT_REJECT_MAX; i++)
        if (beq6(e->rejected[i], addr)) return 1;
    return 0;
}

/* Some other identity already holds this address, guessed or proven. */
static int taken(const struct bt_ctx *c, const struct bt_rl_entry *e, const uint8_t *addr)
{
    for (int i = 0; i < BT_RL_SIZE; i++)
        if (&c->rl[i] != e && c->rl[i].valid && c->rl[i].have_rpa && beq6(c->rl[i].rpa, addr))
            return 1;
    return 0;
}

static struct bt_candidate *pick(struct bt_ctx *c, struct bt_rl_entry *e)
{
    struct bt_candidate *best = NULL;
    int best_score = 0;

    for (int i = 0; i < BT_CAND_MAX; i++) {
        struct bt_candidate *k = &c->cand[i];
        if (!k->used || !fresh(c, k->seen, BT_CAND_FRESH_MS)) continue;
        if (rejected_for(e, k->addr) || taken(c, e, k->addr)) continue;

        int score = 128 + k->rssi;            /* 1..148: louder is likelier */
        if (k->connectable) score += 1000;    /* it can be connected to at all */
        if (e->proven) {
            /* The address that appeared as the proven one went quiet. */
            int32_t d = (int32_t)(k->first_seen - e->rpa_seen);
            if (d >= -5000 && d <= (int32_t)BT_SUCCESSOR_MS)
                score += 4000 - (d < 0 ? -d : d) / 10;   /* the nearest in time wins */
        }
        if (best == NULL || score > best_score) { best = k; best_score = score; }
    }
    return best;
}

static void reject(struct bt_ctx *c, struct bt_rl_entry *e)
{
    if (e->n_rejected >= BT_REJECT_MAX) {
        for (int i = 1; i < BT_REJECT_MAX; i++) bcopy6(e->rejected[i - 1], e->rejected[i]);
        e->n_rejected = BT_REJECT_MAX - 1;
    }
    bcopy6(e->rejected[e->n_rejected++], e->rpa);
    if (!e->proven && e->fails < 255) e->fails++;
    e->have_rpa = 0;
    e->guess = 0;
    c->rejected++;
}

/* A proven or handed-down address that has stopped advertising has rotated
 * away. Only an address the candidate table has actually heard can be judged:
 * one it never saw is left as it is. */
static void refresh(struct bt_ctx *c, struct bt_rl_entry *e)
{
    if (!c->guessing || !e->have_rpa || e->have_irk || e->guess) return;
    struct bt_candidate *k = cand_find(c, e->rpa);
    if (k == NULL || fresh(c, k->seen, BT_CAND_FRESH_MS)) return;
    e->rpa_seen = k->seen;
    e->have_rpa = 0;
    c->stale++;
}

static int identity_shaped(uint8_t type, const uint8_t *addr)
{
    if (type == 0x00) return 1;                          /* public */
    return type == 0x01 && (addr[5] & 0xc0) == 0xc0;     /* static random */
}

static size_t fal_cmd(uint8_t *out, uint16_t op, const uint8_t *addr)
{
    out[0] = (uint8_t)op; out[1] = (uint8_t)(op >> 8); out[2] = 7; out[3] = 0x01;
    bcopy6(&out[4], addr);
    return 10;
}

static void emit(struct bt_inject *out, uint16_t op, const uint8_t *addr)
{
    if (out->n < BT_INJECT_MAX) fal_cmd(out->cmd[out->n++], op, addr);
}

/* Guess for this entry and put the guess on the accept list beside it. */
static void guess_onto_fal(struct bt_ctx *c, struct bt_rl_entry *e, struct bt_inject *out)
{
    if (e->fails >= BT_GUESS_GIVE_UP || out->n >= BT_INJECT_MAX) return;
    struct bt_candidate *k = pick(c, e);
    if (k == NULL) return;
    bcopy6(e->rpa, k->addr);
    e->have_rpa = 1;
    e->guess = 1;
    e->rpa_seen = k->seen;
    c->guesses++;
    emit(out, OP_LE_ADD_FAL, e->rpa);
    e->fal_inj = 1;
    bcopy6(e->fal_inj_rpa, e->rpa);
}

void bt_guess_before_command(struct bt_ctx *c, const uint8_t *pkt, size_t len,
                             struct bt_inject *out)
{
    out->n = 0;
    if (!c->guessing || len < 3) return;
    uint16_t opcode = (uint16_t)(pkt[0] | (pkt[1] << 8));
    uint8_t plen = pkt[2];
    const uint8_t *p = &pkt[3];
    if (len < (size_t)3 + plen) return;

    switch (opcode) {
    case OP_LE_ADD_FAL: {
        if (plen < 7 || !identity_shaped(p[0], &p[1])) return;
        struct bt_rl_entry *e = rl_find(c, p[0], &p[1]);
        if (e != NULL && (e->have_irk || e->fal_inj)) return;
        if (e != NULL) refresh(c, e);
        if (e != NULL && e->have_rpa) return;          /* substitute() has it */
        if (e == NULL) {
            for (int i = 0; i < BT_RL_SIZE && e == NULL; i++)
                if (!c->rl[i].valid) e = &c->rl[i];
            if (e == NULL) return;
            uint8_t *z = (uint8_t *)e;
            for (size_t i = 0; i < sizeof(*e); i++) z[i] = 0;
            e->valid = 1;
            e->id_type = p[0] & 1;
            bcopy6(e->id_addr, &p[1]);
        }
        guess_onto_fal(c, e, out);
        return;
    }
    case OP_LE_DEL_FAL: {
        if (plen < 7) return;
        struct bt_rl_entry *e = rl_find(c, p[0], &p[1]);
        if (e == NULL || !e->fal_inj) return;
        emit(out, OP_LE_DEL_FAL, e->fal_inj_rpa);
        e->fal_inj = 0;
        return;
    }
    case OP_LE_CLEAR_FAL:
        for (int i = 0; i < BT_RL_SIZE; i++) { c->rl[i].fal_inj = 0; c->rl[i].fal_sub = 0; }
        return;
    case OP_LE_CREATE_CONN:
    case OP_LE_EXT_CREATE_CONN: {
        /* Accept-list initiation only. A guess that was disproven since it
         * went on the list is swapped for the next candidate now, so this
         * attempt does not reconnect to the device that just failed. */
        uint8_t policy = opcode == OP_LE_CREATE_CONN ? (plen >= 5 ? p[4] : 0) : (plen >= 1 ? p[0] : 0);
        if (policy != 1) return;
        for (int i = 0; i < BT_RL_SIZE; i++) {
            struct bt_rl_entry *e = &c->rl[i];
            if (!e->valid || !e->fal_inj) continue;
            if (e->have_rpa && beq6(e->rpa, e->fal_inj_rpa)) continue;
            if (out->n + 2 > BT_INJECT_MAX) break;
            emit(out, OP_LE_DEL_FAL, e->fal_inj_rpa);
            e->fal_inj = 0;
            if (!e->have_rpa) guess_onto_fal(c, e, out);
        }
        return;
    }
    default:
        return;
    }
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
    case OP_LE_ADD_FAL: {
        if (plen < 7) break;
        struct bt_rl_entry *e = rl_find(c, p[0], &p[1]);
        if (substitute(c, &p[0], &p[1]) && e != NULL) {
            e->fal_sub = 1;
            bcopy6(e->fal_sub_rpa, &p[1]);
        }
        break;
    }
    case OP_LE_DEL_FAL: {
        if (plen < 7) break;
        /* Remove exactly what the add put there, which is not necessarily
         * the entry's address now. */
        struct bt_rl_entry *e = rl_find(c, p[0], &p[1]);
        if (e != NULL && e->fal_sub) {
            p[0] = 0x01;
            bcopy6(&p[1], e->fal_sub_rpa);
            e->fal_sub = 0;
            c->substituted++;
        }   /* otherwise the add went out unchanged, and so does this */
        break;
    }
    case OP_LE_CLEAR_FAL:
        for (int i = 0; i < BT_RL_SIZE; i++) { c->rl[i].fal_inj = 0; c->rl[i].fal_sub = 0; }
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
    if (e == NULL || e->guess) return 0;     /* a guess resolves nothing yet */

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

static void link_up(struct bt_rl_entry *e, const uint8_t *q)
{
    e->on_link = 1;
    e->handle = (uint16_t)((q[1] | (q[2] << 8)) & 0x0fff);
}

static struct bt_rl_entry *link_entry(struct bt_ctx *c, uint8_t lo, uint8_t hi)
{
    uint16_t handle = (uint16_t)((lo | (hi << 8)) & 0x0fff);
    for (int i = 0; i < BT_RL_SIZE; i++)
        if (c->rl[i].valid && c->rl[i].on_link && c->rl[i].handle == handle && !c->rl[i].have_irk)
            return &c->rl[i];
    return NULL;
}

static void prove(struct bt_ctx *c, struct bt_rl_entry *e)
{
    if (!e->have_rpa) return;
    if (e->guess) c->proven++;
    e->guess = 0;
    e->proven = 1;
    e->fails = 0;
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
            cmds[34] |= 0xf8;                        /* add/remove/clear, size, read peer RPA */
            cmds[35] |= 0x07;                        /* read local RPA, enable, timeout */
            cmds[39] |= 0x04;                        /* LE Set Privacy Mode */
        }
        return BT_FORWARD;
    }

    /* The proof: did the link to a guessed address encrypt with the
     * identity's key? (status, handle, enabled; v2 adds the key size) */
    if ((code == EVT_ENCRYPT_CHANGE || code == EVT_ENCRYPT_CHANGE_V2) && plen >= 4) {
        struct bt_rl_entry *e = link_entry(c, p[1], p[2]);
        if (e != NULL) {
            if (p[0] == 0x00 && p[3] != 0x00) prove(c, e);
            else if (p[0] != 0x00) reject(c, e);
        }
        return BT_FORWARD;
    }
    if (code == EVT_KEY_REFRESH && plen >= 3) {
        struct bt_rl_entry *e = link_entry(c, p[1], p[2]);
        if (e != NULL) {
            if (p[0] == 0x00) prove(c, e);
            else reject(c, e);
        }
        return BT_FORWARD;
    }
    if (code == EVT_DISCONN_COMPLETE && plen >= 4 && p[0] == 0x00) {
        struct bt_rl_entry *e = link_entry(c, p[1], p[2]);
        if (e != NULL) {
            e->on_link = 0;
            if (e->guess) reject(c, e);      /* connected, never encrypted */
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
            note_candidate(c, q[off] <= 0x01, q[off + 1], &q[off + 2], &q[off + 9], dlen,
                           (int8_t)q[off + 9 + dlen]);
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
            note_candidate(c, q[off] & 0x01, q[off + 2], &q[off + 3], &q[off + 24], dlen,
                           (int8_t)q[off + 13]);
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
                link_up(e, q);
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
                link_up(e, q);
            }
        }
        return BT_FORWARD;
    }

    return BT_FORWARD;
}
