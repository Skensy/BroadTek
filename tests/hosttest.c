/* SPDX-License-Identifier: MIT */
/* Userspace test for the BroadTek core: AES-128, ah(), and the HCI rewriting.
 * Built with ASan/UBSan; every buffer here is the size the kernel path uses. */
#include <stdio.h>
#include <string.h>
#include "bt_core.h"

static int fails;

#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("  FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); \
                   printf("\n"); fails++; } \
} while (0)

static void hexdiff(const char *what, const uint8_t *got, const uint8_t *want, size_t n)
{
    if (!memcmp(got, want, n)) return;
    printf("  FAIL %s:\n    got  ", what);
    for (size_t i = 0; i < n; i++) printf("%02x", got[i]);
    printf("\n    want ");
    for (size_t i = 0; i < n; i++) printf("%02x", want[i]);
    printf("\n");
    fails++;
}

/* FIPS-197 C.1 */
static void test_aes(void)
{
    uint8_t key[16], in[16], out[16];
    const uint8_t want[16] = {0x69,0xc4,0xe0,0xd8,0x6a,0x7b,0x04,0x30,
                              0xd8,0xcd,0xb7,0x80,0x70,0xb4,0xc5,0x5a};
    for (int i = 0; i < 16; i++) { key[i] = (uint8_t)i; in[i] = (uint8_t)(0x11 * i); }
    bt_aes128_encrypt(key, in, out);
    hexdiff("AES-128 FIPS-197 C.1", out, want, 16);
}

/* Core spec Vol 3 Part H D.7: IRK ec0234a357c8ad05341010a60a397d9b,
 * prand 0x708194, ah = 0x0dfbaa, so the RPA is 708194|0dfbaa. */
static const uint8_t spec_irk[16] = {0x9b,0x7d,0x39,0x0a,0xa6,0x10,0x10,0x34,
                                     0x05,0xad,0xc8,0x57,0xa3,0x34,0x02,0xec};
static const uint8_t spec_rpa[6]  = {0xaa,0xfb,0x0d,0x94,0x81,0x70};
static const uint8_t id_addr[6]   = {0x03,0x00,0x00,0x00,0x00,0x20};  /* 20:00:00:00:00:03 */

static void test_ah(void)
{
    uint8_t other[16], wrong[6];

    CHECK(bt_rpa_matches(spec_irk, spec_rpa), "spec RPA should resolve");

    memcpy(wrong, spec_rpa, 6);
    wrong[0] ^= 0x01;
    CHECK(!bt_rpa_matches(spec_irk, wrong), "a changed hash must not resolve");

    memcpy(other, spec_irk, 16);
    other[0] ^= 0x01;
    CHECK(!bt_rpa_matches(other, spec_rpa), "a different IRK must not resolve");

    memcpy(wrong, spec_rpa, 6);
    wrong[5] = 0x10;                 /* top bits 00: static, not resolvable */
    CHECK(!bt_rpa_matches(spec_irk, wrong), "a non-RPA address must be rejected");
}

static size_t on_event(struct bt_ctx *c, uint8_t *pkt, size_t len, size_t cap,
                       enum bt_verdict *v)
{
    size_t out = 0;
    enum bt_verdict r = bt_hci_on_event(c, pkt, len, cap, &out);
    if (v != NULL) *v = r;
    return out;
}

static void add_to_rl(struct bt_ctx *c)
{
    uint8_t cmd[3 + 39] = {0x27, 0x20, 39, 0x00};
    memcpy(&cmd[4], id_addr, 6);
    memcpy(&cmd[10], spec_irk, 16);
    CHECK(bt_hci_on_command(c, cmd, sizeof(cmd)) == BT_FORWARD,
          "the resolving-list command still goes to the controller");
}

static void test_realtek_link_setup(void)
{
    uint8_t features[] = {0x3e,12,4,0,0x34,2,0xbd,0x79,1,0xf7,0xf9,0xd9,1,2};
    CHECK(bt_link_procedures(features, sizeof(features)) == (BT_LINK_PHY | BT_LINK_DLE),
          "the features observed from the iPhone allow both link procedures");
    CHECK(!bt_link_procedures(features, 13), "truncated feature event");
    features[7] = 0;
    CHECK(bt_link_procedures(features, sizeof(features)) == BT_LINK_DLE, "DLE only");
    features[3] = 1;
    CHECK(!bt_link_procedures(features, sizeof(features)), "failed remote feature read");
    uint8_t mask[] = {1,0x20,8,0x81,0x12,1,2,3,4,5,6};
    const uint8_t expected[] = {1,0x20,8,0xc1,0x1a,1,2,3,4,5,6};
    CHECK(!bt_link_enable_reports(mask, 10), "truncated event mask");
    CHECK(bt_link_enable_reports(mask, sizeof(mask)) && !memcmp(mask, expected, sizeof(mask)),
          "enable only data length and PHY updates");
    CHECK(!bt_link_enable_reports(mask, sizeof(mask)), "event mask is already enabled");
    uint8_t cmd[10];
    const uint8_t phy[] = {0x32,0x20,7,0x34,2,3,0,0,0,0};
    const uint8_t dle[] = {0x22,0x20,6,0x34,2,0xfb,0,0x48,8};
    CHECK(bt_link_request(BT_LINK_PHY, 0x234, cmd) == sizeof(phy) && !memcmp(cmd, phy, sizeof(phy)),
          "Set PHY asks the controller to report its choice");
    CHECK(bt_link_request(BT_LINK_DLE, 0x234, cmd) == sizeof(dle) && !memcmp(cmd, dle, sizeof(dle)),
          "Set Data Length requests 251 octets on the live handle");
    CHECK(!bt_link_request(BT_LINK_PHY, 0xf00, cmd), "reserved connection handle");
}

static void test_features_rewrite(void)
{
    /* Command Complete for LE Read Local Supported Features, as the Realtek
     * sends it: BD 5F 66 3C ... with LL Privacy clear. */
    uint8_t evt[] = {0x0e, 0x0c, 0x01, 0x03, 0x20, 0x00,
                     0xbd, 0x5f, 0x66, 0x3c, 0x01, 0x00, 0x00, 0x00};
    struct bt_ctx c;
    enum bt_verdict v;
    bt_ctx_init(&c);
    size_t n = on_event(&c, evt, sizeof(evt), sizeof(evt), &v);
    CHECK(v == BT_FORWARD && n == sizeof(evt), "features event is forwarded unchanged in length");
    CHECK(evt[6] == 0xfd, "LL Privacy bit must be set: got %02x, want fd", evt[6]);
    CHECK(evt[7] == 0x5f, "the other feature bytes must be untouched");
}

static void test_supported_commands_rewrite(void)
{
    uint8_t evt[2 + 68];
    struct bt_ctx c;
    bt_ctx_init(&c);
    memset(evt, 0, sizeof(evt));
    evt[0] = 0x0e; evt[1] = 68; evt[2] = 1; evt[3] = 0x02; evt[4] = 0x10; evt[5] = 0x00;
    uint8_t *commands = evt + 6;
    commands[0] = 0xa5;
    commands[34] = 0x03;
    commands[35] = 0x40;
    commands[39] = 0x80;

    /* Supported Commands from Core, Vol 2, Part E, section 6.27.
     * The native bitmap above does not implement LE Read Maximum Data
     * Length or LE Read PHY (octet 35, bits 3 and 4). */
    uint8_t expected[64] = {0};
    expected[0] = 0xa5;
    expected[34] = 0xfb;
    expected[35] = 0x47;
    expected[39] = 0x84;
    on_event(&c, evt, sizeof(evt), sizeof(evt), NULL);
    hexdiff("LE privacy supported commands", commands, expected, sizeof(expected));
}

static void test_advertising_cache(void)
{
    struct bt_adv_state s = {0};
    uint8_t address[] = {1, 2, 3, 4, 5, 6}, out[6], params[11] = {0};
    params[10] = 1;
    bt_adv_command(&s, 0x2036, params, sizeof(params));
    CHECK(!bt_adv_begin(&s, 0, 1, out), "a random address must be available");
    bt_adv_command(&s, 0x2005, address, sizeof(address));
    CHECK(bt_adv_begin(&s, 0, 1, out) && !memcmp(address, out, 6), "inject the first address");
    CHECK(!bt_adv_begin(&s, 0, 1, out), "one outstanding command per controller");
    CHECK(bt_adv_complete(&s, 0) && !bt_adv_begin(&s, 0, 1, out), "cache confirmed sets");
    CHECK(!bt_adv_complete(&s, 0), "pass unrelated completions");
    CHECK(bt_adv_begin(&s, 0, 0, out), "default mode refreshes on every enable");
    bt_adv_complete(&s, 0x0c);
    CHECK(bt_adv_begin(&s, 0, 1, out), "retry a controller error");
    address[0]++;
    bt_adv_command(&s, 0x2005, address, sizeof(address));
    bt_adv_complete(&s, 0);
    CHECK(bt_adv_begin(&s, 0, 1, out) && !memcmp(address, out, 6), "rotate during an injection");
    bt_adv_complete(&s, 0);
    params[0] = 2;
    bt_adv_command(&s, 0x2036, params, sizeof(params));
    CHECK(bt_adv_begin(&s, 2, 1, out), "each advertising handle has its own cache");
    bt_adv_invalidate(&s);
    bt_adv_complete(&s, 0);
    CHECK(bt_adv_begin(&s, 2, 1, out), "a late completion cannot restore invalidated state");
    bt_adv_complete(&s, 0);
    bt_adv_command(&s, 0x203c, params, 1);
    CHECK(!bt_adv_begin(&s, 2, 1, out), "a removed set must be configured again");
    bt_adv_command(&s, 0x2036, params, sizeof(params));
    CHECK(bt_adv_begin(&s, 2, 1, out), "recreated set needs its address");
    bt_adv_command(&s, 0x2035, params, 7);
    CHECK(!bt_adv_complete(&s, 0) && !bt_adv_begin(&s, 2, 0, out), "preserve native address commands");
    CHECK(!bt_adv_complete(&s, 0) && bt_adv_begin(&s, 0, 0, out), "other sets still use the workaround");
    bt_adv_command(&s, 0x0c03, NULL, 0);
    CHECK(!s.native_sets && !s.native_replies && !s.have_address && !s.random_sets, "reset controller address state");
}

/* The controller rejects a command it does not know; BroadTek turns that
 * refusal into the answer a privacy-capable controller would have given. */
static void test_refusal_becomes_answer(void)
{
    struct bt_ctx c;
    uint8_t buf[64];
    enum bt_verdict v;
    bt_ctx_init(&c);

    uint8_t size_cmd[] = {0x2a, 0x20, 0x00};
    bt_hci_on_command(&c, size_cmd, sizeof(size_cmd));

    /* Unknown HCI Command (0x01), the shortest possible Command Complete. */
    uint8_t cc[] = {0x0e, 0x04, 0x01, 0x2a, 0x20, 0x01};
    memcpy(buf, cc, sizeof(cc));
    size_t n = on_event(&c, buf, sizeof(cc), sizeof(buf), &v);
    CHECK(v == BT_FORWARD, "the rewritten Command Complete is delivered");
    CHECK(n == 7, "it must grow by the list-size byte: got %zu, want 7", n);
    CHECK(buf[1] == 5, "parameter length must be updated, got %u", buf[1]);
    CHECK(buf[5] == 0x00, "status must become success, got %02x", buf[5]);
    CHECK(buf[6] == BT_RL_SIZE, "the list size must be ours, got %u", buf[6]);

    /* Only once: a later Command Complete for the same opcode is the
     * controller's own and must pass through untouched. */
    memcpy(buf, cc, sizeof(cc));
    n = on_event(&c, buf, sizeof(cc), sizeof(buf), &v);
    CHECK(n == sizeof(cc) && buf[5] == 0x01, "a second refusal is not rewritten");

    /* A Command Status refusal for an answered command must not reach the
     * host, or it would see a failure BroadTek has promised to handle. */
    uint8_t add[3 + 39] = {0x27, 0x20, 39, 0x00};
    memcpy(&add[10], spec_irk, 16);
    bt_hci_on_command(&c, add, sizeof(add));
    uint8_t cs[] = {0x0f, 0x04, 0x01, 0x01, 0x27, 0x20};
    cs[3] = 3;
    memcpy(buf, cs, sizeof(cs));
    n = on_event(&c, buf, sizeof(cs), sizeof(buf), &v);
    const uint8_t completed[] = {0x0e, 4, 3, 0x27, 0x20, 0};
    CHECK(v == BT_FORWARD && n == sizeof(completed) &&
          memcmp(buf, completed, n) == 0, "complete a refused opcode with its original credits");
    memcpy(buf, cs, sizeof(cs));
    on_event(&c, buf, sizeof(cs), sizeof(buf), &v);
    CHECK(memcmp(buf, cs, sizeof(cs)) == 0, "an unsolicited refusal passes unchanged");
}

/* A reply must never be written past the buffer it arrived in. */
static void test_reply_respects_capacity(void)
{
    struct bt_ctx c;
    uint8_t buf[6];
    enum bt_verdict v;
    bt_ctx_init(&c);

    uint8_t peer[3 + 7] = {0x2b, 0x20, 7, 0x00};
    memcpy(&peer[4], id_addr, 6);
    bt_hci_on_command(&c, peer, sizeof(peer));

    uint8_t cc[] = {0x0e, 0x04, 0x01, 0x2b, 0x20, 0x01};   /* needs 12 bytes to answer */
    memcpy(buf, cc, sizeof(cc));
    size_t n = on_event(&c, buf, sizeof(cc), sizeof(buf), &v);
    CHECK(n == sizeof(cc), "with no room the event must be left alone, got %zu", n);
    CHECK(buf[5] == 0x01, "and its status left as the controller set it");
}

static void test_resolving_list_full(void)
{
    struct bt_ctx c;
    uint8_t buf[64];
    bt_ctx_init(&c);

    /* Each command is answered before the next is sent, as on the wire. */
    for (int i = 0; i < BT_RL_SIZE; i++) {
        uint8_t cmd[3 + 39] = {0x27, 0x20, 39, 0x00};
        cmd[4] = (uint8_t)(0x40 + i);
        memcpy(&cmd[10], spec_irk, 16);
        CHECK(bt_hci_on_command(&c, cmd, sizeof(cmd)) == BT_FORWARD, "always forwarded");
        const uint8_t cc[] = {0x0e, 0x04, 0x01, 0x27, 0x20, 0x01};
        memcpy(buf, cc, sizeof(cc));
        CHECK(on_event(&c, buf, sizeof(cc), sizeof(buf), NULL) == sizeof(cc) &&
              buf[5] == 0x00, "each add is answered with success");
    }

    /* The 17th is not stored, nothing was promised for it, so the
     * controller's own refusal reaches the host unchanged. */
    uint8_t over[3 + 39] = {0x27, 0x20, 39, 0x00};
    over[4] = 0x7f;
    memcpy(&over[10], spec_irk, 16);
    bt_hci_on_command(&c, over, sizeof(over));
    const uint8_t cc[] = {0x0e, 0x04, 0x01, 0x27, 0x20, 0x01};
    memcpy(buf, cc, sizeof(cc));
    enum bt_verdict v;
    size_t n = on_event(&c, buf, sizeof(cc), sizeof(buf), &v);
    CHECK(n == sizeof(cc) && buf[5] == 0x01, "a full list lets the refusal through");
}

/* A truncated or lying advertising report must not read past the packet. */
static void test_malformed_reports(void)
{
    struct bt_ctx c;
    bt_ctx_init(&c);

    uint8_t trunc[] = {0x3e, 0x05, 0x02, 0x01, 0x00, 0x01, 0xaa};
    CHECK(on_event(&c, trunc, sizeof(trunc), sizeof(trunc), NULL) == sizeof(trunc),
          "truncated report survives");

    uint8_t liar[] = {0x3e, 0x0c, 0x02, 0x01, 0x00, 0x01,
                      0xaa, 0xfb, 0x0d, 0x94, 0x81, 0x70, 0xff, 0xc5};
    on_event(&c, liar, sizeof(liar), sizeof(liar), NULL);

    uint8_t short_evt[] = {0x3e, 0x00};
    on_event(&c, short_evt, sizeof(short_evt), sizeof(short_evt), NULL);

    uint8_t ext_trunc[] = {0x3e, 0x08, 0x0d, 0x01, 0x00, 0x00, 0x01, 0xaa, 0xfb, 0x0d};
    on_event(&c, ext_trunc, sizeof(ext_trunc), sizeof(ext_trunc), NULL);

    uint8_t stub_cc[] = {0x0e, 0x01, 0x01};
    on_event(&c, stub_cc, sizeof(stub_cc), sizeof(stub_cc), NULL);
    printf("  (malformed packets survived)\n");
}

/* The whole sequence: resolve a report, then the accept list, then the link. */
static void test_end_to_end(void)
{
    struct bt_ctx c;
    bt_ctx_init(&c);

    add_to_rl(&c);

    uint8_t enable[] = {0x2d, 0x20, 0x01, 0x01};
    bt_hci_on_command(&c, enable, sizeof(enable));

    /* Before any sighting there is no RPA, so the identity goes through as-is. */
    uint8_t fal[3 + 7] = {0x11, 0x20, 7, 0x00};
    memcpy(&fal[4], id_addr, 6);
    bt_hci_on_command(&c, fal, sizeof(fal));
    hexdiff("accept list keeps the identity until an RPA is seen", &fal[4], id_addr, 6);

    /* The phone advertises from its RPA. */
    uint8_t rpt[] = {0x3e, 0x0d, 0x02, 0x01, 0x00, 0x01,
                     0xaa, 0xfb, 0x0d, 0x94, 0x81, 0x70, 0x01, 0x00, 0xc5};
    on_event(&c, rpt, sizeof(rpt), sizeof(rpt), NULL);
    hexdiff("report address resolved to the identity", &rpt[6], id_addr, 6);
    CHECK(rpt[5] == 0x02, "address type must become 'resolved public', got %02x", rpt[5]);

    /* ...so the next accept-list entry carries the RPA instead. */
    uint8_t fal2[3 + 7] = {0x11, 0x20, 7, 0x00};
    memcpy(&fal2[4], id_addr, 6);
    CHECK(bt_hci_on_command(&c, fal2, sizeof(fal2)) == BT_FORWARD, "forwarded");
    CHECK(fal2[3] == 0x01, "the RPA is a random address, got %02x", fal2[3]);
    hexdiff("accept list now carries the RPA", &fal2[4], spec_rpa, 6);

    /* A direct connect to the identity goes out to the RPA instead. */
    uint8_t create[3 + 25] = {0x0d, 0x20, 25, 0x60,0x00, 0x30,0x00, 0x00, 0x00};
    memcpy(&create[9], id_addr, 6);
    create[15] = 0x02;                       /* own address type: RPA, else public */
    bt_hci_on_command(&c, create, sizeof(create));
    CHECK(create[8] == 0x01, "peer type must become random");
    hexdiff("create connection targets the RPA", &create[9], spec_rpa, 6);
    CHECK(create[15] == 0x00, "own address type 0x02 must map to public");

    /* The link comes up against the RPA; the host must see the identity. */
    uint8_t conn[] = {0x3e, 0x1e, 0x0a, 0x00, 0x40, 0x00, 0x00, 0x01,
                      0xaa, 0xfb, 0x0d, 0x94, 0x81, 0x70,
                      0,0,0,0,0,0,  0,0,0,0,0,0,
                      0x06, 0x00, 0x00, 0x00, 0x2a, 0x00, 0x05};
    on_event(&c, conn, sizeof(conn), sizeof(conn), NULL);
    hexdiff("connection peer rewritten to the identity", &conn[8], id_addr, 6);
    CHECK(conn[7] == 0x02, "peer type must be 'resolved public', got %02x", conn[7]);
    hexdiff("peer RPA field filled in", &conn[20], spec_rpa, 6);
    CHECK(c.conn_events == 1, "one connection event counted, got %u", c.conn_events);
    CHECK(c.conn_translated == 1, "and one translated, got %u", c.conn_translated);

    /* An unknown device's report must be left exactly as it came. */
    uint8_t other[] = {0x3e, 0x0d, 0x02, 0x01, 0x00, 0x01,
                       0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x01, 0x00, 0xc5};
    uint8_t copy[sizeof(other)];
    memcpy(copy, other, sizeof(other));
    on_event(&c, other, sizeof(other), sizeof(other), NULL);
    hexdiff("an unrelated advertiser is untouched", other, copy, sizeof(other));
}

/* An HCI ACL data packet carrying one SMP PDU: header, L2CAP length and CID,
 * then the PDU. Built the way the controller delivers it. */
static size_t acl_smp(uint8_t *out, uint16_t handle, uint8_t pb,
                      uint16_t cid, const uint8_t *pdu, size_t pdu_len)
{
    uint16_t hdr = (uint16_t)((handle & 0x0fff) | ((pb & 3) << 12));
    out[0] = (uint8_t)(hdr & 0xff);
    out[1] = (uint8_t)(hdr >> 8);
    out[2] = (uint8_t)((pdu_len + 4) & 0xff);
    out[3] = (uint8_t)((pdu_len + 4) >> 8);
    out[4] = (uint8_t)(pdu_len & 0xff);
    out[5] = (uint8_t)(pdu_len >> 8);
    out[6] = (uint8_t)(cid & 0xff);
    out[7] = (uint8_t)(cid >> 8);
    for (size_t i = 0; i < pdu_len; i++) out[8 + i] = pdu[i];
    return 8 + pdu_len;
}

/* The IRK arrives on the ACL link during bonding, because macOS will never
 * hand it over through HCI. See src/core/bt_smp.c. */
static void test_smp_capture(void)
{
    struct bt_ctx c;
    uint8_t pkt[64], pdu[32];
    bt_ctx_init(&c);

    /* Identity Information: opcode 0x08 then the IRK, least significant
     * octet first, which is the order ah() wants. */
    pdu[0] = 0x08;
    memcpy(&pdu[1], spec_irk, 16);
    CHECK(bt_acl_scan(&c, pkt, acl_smp(pkt, 0x0040, 0x02, 0x0006, pdu, 17)) == 1,
          "an Identity Information PDU must be captured");
    CHECK(c.irks_seen == 1, "one IRK seen, got %u", c.irks_seen);
    CHECK(bt_smp_complete(&c) == 0, "an IRK with no identity address is not complete");

    /* Identity Address Information: type then the address. */
    pdu[0] = 0x09;
    pdu[1] = 0x00;                       /* public */
    memcpy(&pdu[2], id_addr, 6);
    CHECK(bt_acl_scan(&c, pkt, acl_smp(pkt, 0x0040, 0x02, 0x0006, pdu, 8)) == 1,
          "an Identity Address PDU must be captured");
    CHECK(bt_smp_complete(&c) == 1, "both halves on one handle make a complete capture");

    /* The captured key must be the one that resolves the spec's RPA: this is
     * what makes a capture worth anything. */
    CHECK(c.smp[0].have_irk && c.smp[0].have_id, "capture should hold both");
    CHECK(bt_rpa_matches(c.smp[0].irk, spec_rpa),
          "the captured IRK must resolve the RPA it belongs to");
    hexdiff("captured identity address", c.smp[0].id_addr, id_addr, 6);

    /* Capture must not touch the resolving list: promoting one is a separate
     * decision, and observe-only has to mean it. */
    for (int i = 0; i < BT_RL_SIZE; i++)
        CHECK(!c.rl[i].valid, "capture must not populate the resolving list");
}

/* Anything that is not an SMP identity PDU must be left entirely alone, and
 * nothing may read past the length the packet declares. */
static void test_smp_rejects(void)
{
    struct bt_ctx c;
    uint8_t pkt[64], pdu[32];
    size_t n;

    bt_ctx_init(&c);
    pdu[0] = 0x08;
    memcpy(&pdu[1], spec_irk, 16);

    /* A different L2CAP channel is not SMP. */
    bt_acl_scan(&c, pkt, acl_smp(pkt, 0x0040, 0x02, 0x0004, pdu, 17));
    CHECK(c.irks_seen == 0 && c.smp_pdus == 0, "only channel 0x0006 is SMP");

    /* A continuation fragment is counted, not guessed at. */
    bt_acl_scan(&c, pkt, acl_smp(pkt, 0x0040, 0x01, 0x0006, pdu, 17));
    CHECK(c.irks_seen == 0, "a continuation must not be parsed");
    CHECK(c.acl_continuations == 1, "and must be counted, got %u", c.acl_continuations);

    /* An Identity Information PDU too short for its 16-byte key. */
    n = acl_smp(pkt, 0x0040, 0x02, 0x0006, pdu, 10);
    bt_acl_scan(&c, pkt, n);
    CHECK(c.irks_seen == 0, "a short IRK must be refused");
    CHECK(c.smp_truncated == 1, "and counted, got %u", c.smp_truncated);

    /* An L2CAP length longer than the ACL payload actually holds. */
    n = acl_smp(pkt, 0x0040, 0x02, 0x0006, pdu, 17);
    pkt[4] = 0xff; pkt[5] = 0x00;
    bt_acl_scan(&c, pkt, n);
    CHECK(c.irks_seen == 0, "a lying L2CAP length must be refused");

    /* An ACL length longer than the buffer handed in. */
    n = acl_smp(pkt, 0x0040, 0x02, 0x0006, pdu, 17);
    pkt[2] = 0xff; pkt[3] = 0x00;
    bt_acl_scan(&c, pkt, n);
    CHECK(c.irks_seen == 0, "a lying ACL length must be refused");

    /* Runts, at every header boundary. */
    for (size_t i = 0; i <= 12; i++) {
        uint8_t runt[16];
        memset(runt, 0, sizeof(runt));
        runt[2] = 8;
        runt[6] = 0x06;
        bt_acl_scan(&c, runt, i);
    }
    CHECK(c.irks_seen == 0, "no runt may yield an IRK");
    printf("  (SMP rejects survived)\n");
}

/* The kext reads only the first 64 bytes of an ACL packet, because that is
 * all an SMP identity PDU can occupy and a completion routine's stack cannot
 * hold more. Two things have to hold for that to be safe: an identity PDU
 * must still parse inside the prefix, and a larger frame seen only as a
 * prefix must be stepped over rather than read past. */
static void test_acl_prefix_only(void)
{
    struct bt_ctx c;
    uint8_t pkt[1024], pdu[32];
    bt_ctx_init(&c);

    /* An identity PDU is 25 bytes all in; the prefix is 64. */
    pdu[0] = 0x08;
    memcpy(&pdu[1], spec_irk, 16);
    size_t whole = acl_smp(pkt, 0x0040, 0x02, 0x0006, pdu, 17);
    CHECK(whole <= 64, "an identity PDU must fit the prefix: %zu bytes", whole);
    CHECK(bt_acl_scan(&c, pkt, whole) == 1, "and must parse inside it");

    /* A 600-byte L2CAP frame, handed over as the 64-byte prefix the kext
     * would actually read. It is not SMP and must yield nothing, without
     * reading beyond the 64 bytes given (ASan would catch that). */
    bt_ctx_init(&c);
    memset(pkt, 0xab, sizeof(pkt));
    pkt[0] = 0x40; pkt[1] = 0x20;
    pkt[2] = (uint8_t)(604 & 0xff); pkt[3] = (uint8_t)(604 >> 8);
    pkt[4] = (uint8_t)(600 & 0xff); pkt[5] = (uint8_t)(600 >> 8);
    pkt[6] = 0x06; pkt[7] = 0x00;                 /* claims to be SMP */
    CHECK(bt_acl_scan(&c, pkt, 64) == 0, "a frame longer than the prefix yields nothing");
    CHECK(c.irks_seen == 0, "and no IRK");

    /* The same, declared honestly but still only partly handed over. */
    bt_ctx_init(&c);
    CHECK(bt_acl_scan(&c, pkt, 8) == 0, "a header-only read yields nothing");
}

/* Two devices bonding give two captures, each on its own handle. */
static void test_smp_two_devices(void)
{
    struct bt_ctx c;
    uint8_t pkt[64], pdu[32];
    uint8_t other_irk[16], other_addr[6] = {0x11,0x22,0x33,0x44,0x55,0x66};
    bt_ctx_init(&c);

    memcpy(other_irk, spec_irk, 16);
    other_irk[0] ^= 0xff;

    pdu[0] = 0x08; memcpy(&pdu[1], spec_irk, 16);
    bt_acl_scan(&c, pkt, acl_smp(pkt, 0x0040, 0x02, 0x0006, pdu, 17));
    pdu[0] = 0x09; pdu[1] = 0x00; memcpy(&pdu[2], id_addr, 6);
    bt_acl_scan(&c, pkt, acl_smp(pkt, 0x0040, 0x02, 0x0006, pdu, 8));

    pdu[0] = 0x08; memcpy(&pdu[1], other_irk, 16);
    bt_acl_scan(&c, pkt, acl_smp(pkt, 0x0041, 0x02, 0x0006, pdu, 17));
    pdu[0] = 0x09; pdu[1] = 0x01; memcpy(&pdu[2], other_addr, 6);
    bt_acl_scan(&c, pkt, acl_smp(pkt, 0x0041, 0x02, 0x0006, pdu, 8));

    CHECK(bt_smp_complete(&c) == 2, "two handles, two captures, got %d", bt_smp_complete(&c));
    CHECK(c.smp[0].handle == 0x0040 && c.smp[1].handle == 0x0041, "kept per handle");
    CHECK(c.smp[1].id_type == 1, "a random identity address keeps its type");

    /* A repeat on the same handle updates that capture rather than making a
     * new one: a device re-bonding must not fill the table. */
    pdu[0] = 0x08; memcpy(&pdu[1], spec_irk, 16);
    for (int i = 0; i < BT_SMP_MAX + 4; i++)
        bt_acl_scan(&c, pkt, acl_smp(pkt, 0x0040, 0x02, 0x0006, pdu, 17));
    CHECK(bt_smp_complete(&c) == 2, "re-bonding must not add slots, got %d",
          bt_smp_complete(&c));
}

/* An IRK that came from outside the HCI stream -- read out of the keychain,
 * or promoted from a bonding capture -- must do everything an IRK learned the
 * intended way would have done. This is the whole point of the project, so it
 * is tested end to end: install a key, see a report from that device, and
 * check the accept-list entry that follows carries the private address. */
static void test_install_and_resolve(void)
{
    struct bt_ctx c;
    bt_ctx_init(&c);

    CHECK(bt_rl_install(&c, 0x00, id_addr, spec_irk) == 1, "the key must install");
    CHECK(c.rl[0].valid && c.rl[0].id_type == 0, "and occupy a resolving-list entry");
    CHECK(!c.rl[0].have_rpa, "with no sighting yet");

    /* Installing the same identity again refreshes rather than filling up. */
    for (int i = 0; i < BT_RL_SIZE + 4; i++)
        CHECK(bt_rl_install(&c, 0x00, id_addr, spec_irk) == 1, "re-install must succeed");
    int used = 0;
    for (int i = 0; i < BT_RL_SIZE; i++) if (c.rl[i].valid) used++;
    CHECK(used == 1, "re-installing one identity must use one entry, got %d", used);

    /* Address resolution on, then the device advertises from its RPA. */
    uint8_t enable[] = {0x2d, 0x20, 0x01, 0x01};
    bt_hci_on_command(&c, enable, sizeof(enable));
    const uint8_t cc_en[] = {0x0e, 0x04, 0x01, 0x2d, 0x20, 0x01};
    uint8_t buf[64];
    memcpy(buf, cc_en, sizeof(cc_en));
    on_event(&c, buf, sizeof(cc_en), sizeof(buf), NULL);

    uint8_t rpt[] = {0x3e, 0x0d, 0x02, 0x01, 0x00, 0x01,
                     0xaa, 0xfb, 0x0d, 0x94, 0x81, 0x70, 0x01, 0x00, 0xc5};
    on_event(&c, rpt, sizeof(rpt), sizeof(rpt), NULL);
    hexdiff("a key from outside resolves the report", &rpt[6], id_addr, 6);
    CHECK(rpt[5] == 0x02, "and reports it as a resolved identity, got %02x", rpt[5]);

    /* Which is what makes the accept-list substitution work: this is the
     * command macOS actually sends (broadtek-conn-targets, §3). */
    uint8_t fal[3 + 7] = {0x11, 0x20, 7, 0x00};
    memcpy(&fal[4], id_addr, 6);
    CHECK(bt_hci_on_command(&c, fal, sizeof(fal)) == BT_FORWARD, "forwarded");
    CHECK(fal[3] == 0x01, "the accept list must take a random address, got %02x", fal[3]);
    hexdiff("accept list carries the private address", &fal[4], spec_rpa, 6);

    /* The two counters that tell a wrong key from a quiet device. */
    CHECK(c.resolved == 1, "one resolution counted, got %u", c.resolved);
    CHECK(c.substituted == 1, "one substitution counted, got %u", c.substituted);

    /* The helper re-installs the same key every few minutes: that must not
     * forget the sighting, or the accept list loses the RPA until the next
     * advert. A different key for the identity does start over. */
    CHECK(c.rl[0].have_rpa, "the report gave a sighting");
    CHECK(bt_rl_install(&c, 0x00, id_addr, spec_irk) == 1, "the same key again");
    CHECK(c.rl[0].have_rpa, "the same key again must keep the sighting");
    {
        uint8_t other[16];
        memcpy(other, spec_irk, 16);
        other[15] ^= 0x01;
        CHECK(bt_rl_install(&c, 0x00, id_addr, other) == 1, "a new key for the identity");
        CHECK(!c.rl[0].have_rpa, "a new key must drop the old sighting");
        CHECK(bt_rl_install(&c, 0x00, id_addr, spec_irk) == 1, "and the first key back");
    }

    /* bluetoothd clears the list as it starts, which can come after the key
     * helper has run at boot: the installed key must survive it, while an
     * entry bluetoothd added itself goes as the controller's would. */
    {
        uint8_t add[3 + 39] = {0x27, 0x20, 39, 0x00, 1, 2, 3, 4, 5, 6};
        bt_hci_on_command(&c, add, sizeof(add));      /* bluetoothd adds one of its own */
        int before = 0;
        for (int i = 0; i < BT_RL_SIZE; i++) if (c.rl[i].valid) before++;
        CHECK(before == 2, "two entries before the clear, got %d", before);
        uint8_t clear[3] = {0x29, 0x20, 0x00};
        bt_hci_on_command(&c, clear, sizeof(clear));
        int after = 0, ours = -1;
        for (int i = 0; i < BT_RL_SIZE; i++)
            if (c.rl[i].valid) { after++; ours = i; }
        CHECK(after == 1, "the clear must leave only the installed key, got %d", after);
        CHECK(ours >= 0 && memcmp(c.rl[ours].id_addr, id_addr, 6) == 0, "and it is the Watch's");
        uint8_t del[3 + 7] = {0x28, 0x20, 7, 0x00};
        memcpy(&del[4], id_addr, 6);
        bt_hci_on_command(&c, del, sizeof(del));
        CHECK(c.rl[ours].valid, "bluetoothd's delete must leave the installed key too");
    }

    /* A key that matches nothing must count nothing. */
    struct bt_ctx w;
    bt_ctx_init(&w);
    uint8_t wrong_irk[16];
    memcpy(wrong_irk, spec_irk, 16);
    wrong_irk[0] ^= 0xff;
    bt_rl_install(&w, 0x00, id_addr, wrong_irk);
    uint8_t rpt2[] = {0x3e, 0x0d, 0x02, 0x01, 0x00, 0x01,
                      0xaa, 0xfb, 0x0d, 0x94, 0x81, 0x70, 0x01, 0x00, 0xc5};
    on_event(&w, rpt2, sizeof(rpt2), sizeof(rpt2), NULL);
    CHECK(w.resolved == 0, "a key that does not match must resolve nothing");
    uint8_t fal2[3 + 7] = {0x11, 0x20, 7, 0x00};
    memcpy(&fal2[4], id_addr, 6);
    bt_hci_on_command(&w, fal2, sizeof(fal2));
    CHECK(w.substituted == 0, "and must substitute nothing");
    CHECK(fal2[3] == 0x00, "leaving the identity address in place");
}

/* A capture is promoted only when asked, and then behaves like any key. */
static void test_promote(void)
{
    struct bt_ctx c;
    uint8_t pkt[64], pdu[32];
    bt_ctx_init(&c);

    pdu[0] = 0x08; memcpy(&pdu[1], spec_irk, 16);
    bt_acl_scan(&c, pkt, acl_smp(pkt, 0x0040, 0x02, 0x0006, pdu, 17));
    pdu[0] = 0x09; pdu[1] = 0x00; memcpy(&pdu[2], id_addr, 6);
    bt_acl_scan(&c, pkt, acl_smp(pkt, 0x0040, 0x02, 0x0006, pdu, 8));

    for (int i = 0; i < BT_RL_SIZE; i++)
        CHECK(!c.rl[i].valid, "capture alone must not reach the resolving list");

    CHECK(bt_promote_captures(&c) == 1, "one capture promotes");
    CHECK(c.rl[0].valid, "and lands in the list");
    hexdiff("promoted identity", c.rl[0].id_addr, id_addr, 6);
    CHECK(bt_rpa_matches(c.rl[0].irk, spec_rpa), "with a key that resolves its RPA");

    /* Promoting twice must not duplicate. */
    bt_promote_captures(&c);
    int used = 0;
    for (int i = 0; i < BT_RL_SIZE; i++) if (c.rl[i].valid) used++;
    CHECK(used == 1, "promoting twice must not duplicate, got %d", used);

    /* A capture with only half the pair is not promoted. */
    struct bt_ctx h;
    bt_ctx_init(&h);
    pdu[0] = 0x08; memcpy(&pdu[1], spec_irk, 16);
    bt_acl_scan(&h, pkt, acl_smp(pkt, 0x0041, 0x02, 0x0006, pdu, 17));
    CHECK(bt_promote_captures(&h) == 0, "an IRK with no identity must not promote");
}

/* A mapping handed down from outside, with no key at all.
 *
 * Some devices have no IRK on this machine (the iPhone: docs/STATE.md), yet
 * their current private address can be learned another way. An entry installed
 * that way must do everything a resolved one does -- substitute on the accept
 * list, and translate a connection complete back to the identity -- while
 * never pretending to resolve anything by itself. */
static void test_install_map(void)
{
    struct bt_ctx c;
    bt_ctx_init(&c);

    CHECK(bt_rl_install_map(&c, 0x00, id_addr, spec_rpa) == 1, "a mapping must install");
    CHECK(c.rl[0].valid && c.rl[0].have_rpa, "with its address ready to use");
    CHECK(!c.rl[0].have_irk, "and no key");

    /* The accept-list add macOS really sends must come out carrying the
     * private address, exactly as it does with a key. */
    uint8_t fal[3 + 7] = {0x11, 0x20, 7, 0x00};
    memcpy(&fal[4], id_addr, 6);
    CHECK(bt_hci_on_command(&c, fal, sizeof(fal)) == BT_FORWARD, "forwarded");
    CHECK(fal[3] == 0x01, "the accept list must take a random address, got %02x", fal[3]);
    hexdiff("a keyless mapping substitutes too", &fal[4], spec_rpa, 6);
    CHECK(c.substituted == 1, "one substitution counted, got %u", c.substituted);

    /* And the link that forms against it must be reported as the identity. */
    uint8_t conn[] = {0x3e, 0x0c, 0x01, 0x00, 0x40, 0x00, 0x00, 0x01,
                      0xaa, 0xfb, 0x0d, 0x94, 0x81, 0x70};
    memcpy(&conn[8], spec_rpa, 6);
    on_event(&c, conn, sizeof(conn), sizeof(conn), NULL);
    hexdiff("connection complete carries the identity", &conn[8], id_addr, 6);
    CHECK(c.conn_translated == 1, "one translation counted, got %u", c.conn_translated);

    /* A zero key is still a key to ah(): a mapping-only entry must never be
     * offered to it, or it would resolve strangers by chance. */
    struct bt_ctx z;
    bt_ctx_init(&z);
    bt_rl_install_map(&z, 0x00, id_addr, spec_rpa);
    uint8_t other[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    other[5] = 0x40;                        /* shaped like an RPA, not ours */
    uint8_t rpt[] = {0x3e, 0x0d, 0x02, 0x01, 0x00, 0x01,
                     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0xc5};
    memcpy(&rpt[6], other, 6);
    on_event(&z, rpt, sizeof(rpt), sizeof(rpt), NULL);
    CHECK(z.resolved == 0, "a mapping must not resolve an address it was not given");
    hexdiff("and must leave that report alone", &rpt[6], other, 6);
}

/* The list is finite and must refuse rather than overflow. */
static void test_install_full(void)
{
    struct bt_ctx c;
    uint8_t addr[6];
    bt_ctx_init(&c);

    for (int i = 0; i < BT_RL_SIZE; i++) {
        memcpy(addr, id_addr, 6);
        addr[0] = (uint8_t)(0x10 + i);
        CHECK(bt_rl_install(&c, 0x00, addr, spec_irk) == 1, "entry %d must install", i);
    }
    memcpy(addr, id_addr, 6);
    addr[0] = 0xfe;
    CHECK(bt_rl_install(&c, 0x00, addr, spec_irk) == 0, "a full list must refuse");
}

int main(void)
{
    printf("BroadTek core test\n");
    test_aes();
    test_ah();
    test_realtek_link_setup();
    test_features_rewrite();
    test_supported_commands_rewrite();
    test_advertising_cache();
    test_refusal_becomes_answer();
    test_reply_respects_capacity();
    test_resolving_list_full();
    test_malformed_reports();
    test_end_to_end();
    test_smp_capture();
    test_smp_rejects();
    test_smp_two_devices();
    test_acl_prefix_only();
    test_install_and_resolve();
    test_promote();
    test_install_full();
    test_install_map();
    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("all checks passed\n");
    return 0;
}
