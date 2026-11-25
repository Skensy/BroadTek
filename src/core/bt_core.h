/* SPDX-License-Identifier: MIT */
/* BroadTek core: host-side LL Privacy emulation over raw HCI packets.
 * Freestanding C: no libc, no kernel headers. Addresses and keys are in HCI
 * byte order (least significant byte first). See docs/DESIGN.md.
 *
 * Nothing here ever creates a packet. Every reply BroadTek owes the host is a
 * Command Complete the controller is about to send anyway (rejecting a command
 * it does not know), and every accept-list entry BroadTek needs is a command
 * the host is already sending. Both are edited in flight, which is why no
 * packet has to be injected into either direction -- with one exception: an
 * address guessed for a keyless identity goes on the accept list beside it,
 * which needs an accept-list command of BroadTek's own
 * (bt_guess_before_command). */
#ifndef BT_CORE_H
#define BT_CORE_H

#include <stdint.h>
#include <stddef.h>

#define BT_RL_SIZE      16
#define BT_PENDING_MAX  8
#define BT_REPLY_MAX    16
#define BT_SMP_MAX      8
#define BT_CAND_MAX     32
#define BT_REJECT_MAX   6

/* How long, in the caller's milliseconds (bt_ctx.now), an advertisement keeps
 * a private address in play. Apple devices advertise NearbyInfo several times
 * a second while awake, so a candidate unheard for this long is gone. */
#define BT_CAND_FRESH_MS   30000u
/* A rotated address shows up within this long of the old one going quiet. */
#define BT_SUCCESSOR_MS    15000u

/* A private address an Apple device is advertising right now, with Apple's
 * NearbyInfo record in it. These are the only addresses a keyless identity
 * can be guessed onto (see bt_hci.c, "Guessing and proving"). */
struct bt_candidate {
    uint8_t  used;
    uint8_t  connectable;   /* ever seen in a connectable advertisement */
    uint8_t  addr[6];
    int8_t   rssi;
    uint32_t first_seen;
    uint32_t seen;
};

struct bt_rl_entry {
    uint8_t valid;
    uint8_t id_type;        /* 0 public, 1 random static */
    uint8_t id_addr[6];
    uint8_t irk[16];
    uint8_t have_irk;       /* 0 for an entry whose address was handed down */
    uint8_t have_rpa;
    uint8_t rpa[6];         /* last RPA seen for this identity */
    uint8_t pinned;         /* installed from userspace: bluetoothd's own clear
                             * and delete leave it, since it never added it */

    /* Keyless entries only: the private address was guessed, not resolved,
     * and stays a guess until a link to it encrypts with the identity's key. */
    uint8_t  guess;
    uint8_t  proven;        /* a link to `rpa` encrypted: it is this device */
    uint8_t  on_link;
    uint16_t handle;
    /* What the controller's accept list really holds for this identity: a
     * guess BroadTek added beside it, or the address it was replaced by. A
     * removal has to name exactly that, even if `rpa` has moved on since. */
    uint8_t  fal_inj, fal_sub;
    uint8_t  fal_inj_rpa[6], fal_sub_rpa[6];
    uint32_t rpa_seen;      /* last advertisement from `rpa`, for successors */
    uint8_t  fails;         /* guesses disproven before any was proven */
    uint8_t  n_rejected;
    uint8_t  rejected[BT_REJECT_MAX][6];
};

/* Accept-list commands BroadTek has to send ahead of one of bluetoothd's
 * (LE Add/Remove Device To/From Filter Accept List, 10 bytes each). */
#define BT_INJECT_MAX 4
struct bt_inject {
    uint8_t n;
    uint8_t cmd[BT_INJECT_MAX][10];
};

/* A command BroadTek answered on the controller's behalf: when its Command
 * Complete comes back, this reply replaces the contents. */
struct bt_pending {
    uint8_t  used;
    uint16_t opcode;
    uint8_t  len;
    uint8_t  reply[BT_REPLY_MAX];   /* the return parameters, after the status */
};

/* An IRK read out of a bonding exchange on the ACL link, with the identity
 * address it belongs to. Held apart from the resolving list on purpose:
 * capture is one step, trusting a capture enough to resolve with it is
 * another. See bt_smp.c. */
struct bt_smp_capture {
    uint8_t  used;
    uint16_t handle;            /* the connection it was seen on */
    uint8_t  have_irk;
    uint8_t  irk[16];
    uint8_t  have_id;
    uint8_t  id_type;
    uint8_t  id_addr[6];
};

/* Address state for a single USB HCI controller. The plugin serializes
 * access from command submission and interrupt completion. */
struct bt_adv_state {
    uint8_t address[6], have_address, handle, can_confirm;
    uint16_t random_sets, confirmed_sets, native_sets;
    uint32_t native_replies;
    uint64_t serial, pending;
};

void bt_adv_invalidate(struct bt_adv_state *s);
void bt_adv_reset(struct bt_adv_state *s);
void bt_adv_command(struct bt_adv_state *s, uint16_t opcode, const uint8_t *p, size_t len);
uint64_t bt_adv_begin(struct bt_adv_state *s, uint8_t handle, int cache, uint8_t address[6]);
int bt_adv_complete(struct bt_adv_state *s, uint8_t status);

struct bt_ctx {
    struct bt_rl_entry rl[BT_RL_SIZE];
    struct bt_pending pending[BT_PENDING_MAX];
    uint8_t res_enabled;

    /* Captured by bt_acl_scan(), used by nothing yet. The counters are
     * diagnostics: they say whether the ACL path is being read at all, and
     * whether anything had to be given up on. */
    struct bt_smp_capture smp[BT_SMP_MAX];
    uint32_t smp_pdus;          /* SMP PDUs seen on L2CAP channel 0x0006 */
    uint32_t irks_seen;          /* Identity Information PDUs parsed */
    uint32_t acl_continuations;  /* fragments skipped rather than reassembled */
    uint32_t smp_truncated;      /* PDUs that did not fit their own length */

    /* Did the key work? Without these, an IRK stored in the wrong byte order
     * and a device that simply has not advertised yet look identical, and
     * both look like nothing happening at all. */
    uint32_t resolved;           /* advertisements whose RPA matched an IRK */
    uint32_t substituted;        /* commands where an address was replaced */

    /* The last link: a connection that forms against a private address has to
     * be reported to the host as the identity it asked for, or the host does
     * not recognise the device it just connected to. */
    uint32_t conn_events;        /* LE Connection Complete, plain or enhanced */
    uint32_t conn_translated;    /* ... whose peer address was put back */

    /* Keyless identities (the iPhone): which private address is it now? */
    uint8_t  guessing;           /* set by the caller to turn guessing on */
    uint32_t now;                /* the caller's clock, milliseconds */
    struct bt_candidate cand[BT_CAND_MAX];
    uint32_t guesses;            /* addresses guessed for an identity */
    uint32_t proven;             /* ... and proven by an encrypted link */
    uint32_t rejected;           /* ... and disproven */
    uint32_t stale;              /* proven addresses dropped after rotating */
};

enum bt_verdict {
    BT_FORWARD = 0,         /* pass on, possibly edited in place */
    BT_DROP    = 2,         /* event: do not deliver to the host */
};

void bt_aes128_encrypt(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);
int  bt_rpa_matches(const uint8_t irk_le[16], const uint8_t rpa_le[6]);

/* Standard HCI link procedures used by the optional RTL8822CU readiness fix. */
enum bt_link_procedure { BT_LINK_DLE = 1, BT_LINK_PHY = 2 };
unsigned bt_link_procedures(const uint8_t *event, size_t len);
int bt_link_enable_reports(uint8_t *command, size_t len);
size_t bt_link_request(enum bt_link_procedure procedure, uint16_t handle, uint8_t out[10]);

void bt_ctx_init(struct bt_ctx *c);

/* pkt: an HCI command (opcode LE16, plen, params), edited in place. Its length
 * never changes: BroadTek only ever substitutes an address inside it. */
enum bt_verdict bt_hci_on_command(struct bt_ctx *c, uint8_t *pkt, size_t len);

/* pkt: an HCI event (code, plen, params), edited in place. A Command Complete
 * that BroadTek answers can grow, up to cap; *out_len is the length to deliver
 * and is always set. */
enum bt_verdict bt_hci_on_event(struct bt_ctx *c, uint8_t *pkt, size_t len,
                                size_t cap, size_t *out_len);

/* Before one of bluetoothd's commands is forwarded: the accept-list commands
 * BroadTek has to send first so a guessed address stands beside an identity
 * it has no proven address for (see bt_hci.c, "Guessing and proving"). The
 * caller sends out->cmd[0..n-1] in order, each to completion, and must not let
 * their Command Completes reach the host. Call before bt_hci_on_command. */
void bt_guess_before_command(struct bt_ctx *c, const uint8_t *pkt, size_t len,
                             struct bt_inject *out);

/* pkt: an HCI ACL data packet (handle and flags LE16, total length LE16, then
 * the payload). Read-only: it captures an IRK out of a bonding exchange and
 * changes nothing. Returns 1 when something was captured. */
int bt_acl_scan(struct bt_ctx *c, const uint8_t *pkt, size_t len);

/* Captures holding both an IRK and the identity address it belongs to. */
int bt_smp_complete(const struct bt_ctx *c);

/* Put an identity and its IRK into the resolving list, replacing any entry
 * for the same identity. Returns 1 if stored, 0 if the list is full. The key
 * comes from outside the HCI stream: macOS never hands one over. */
int bt_rl_install(struct bt_ctx *c, uint8_t id_type, const uint8_t id_addr[6],
                  const uint8_t irk[16]);

/* Put an identity and its *current private address* into the resolving list,
 * with no key at all. Returns 1 if stored, 0 if the list is full.
 *
 * Some devices have no IRK on this machine to resolve with -- the iPhone is
 * one (docs/STATE.md) -- yet macOS recognises them anyway, by a tag carried in
 * the advertisement. Whoever can do that join can hand the answer down here,
 * and everything downstream is unchanged: substitute() only ever reads
 * `have_rpa`, so an address learned this way is used exactly as a resolved one
 * is. An entry installed here never resolves anything by itself; it is matched
 * only against the address it was given. */
int bt_rl_install_map(struct bt_ctx *c, uint8_t id_type, const uint8_t id_addr[6],
                      const uint8_t rpa[6]);

/* Move completed captures into the resolving list. Returns how many. */
int bt_promote_captures(struct bt_ctx *c);

#endif
