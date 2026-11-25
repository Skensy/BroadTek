/* SPDX-License-Identifier: MIT */
/* BroadTek: watch the HCI traffic bluetoothd pushes through IOUSBHostFamily,
 * and (with -broadtekrw) rewrite it so a controller without LL Privacy looks
 * like one that has it.
 *
 * Phase 0 is observe-only: without -broadtekrw nothing is changed, the plugin
 * only logs which hook sees what. The hook points below are deductions from
 * the logs, not yet confirmed on hardware; that confirmation is what the
 * observe-only build is for. See docs/DESIGN.md. */
#include <Headers/kern_api.hpp>
#include <Headers/kern_util.hpp>
#include <IOKit/IOLib.h>
#include <IOKit/IOMessage.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <kern/thread_call.h>
#include <sys/vnode.h>
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSData.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/usb/IOUSBHostPipe.h>
#include <IOKit/usb/IOUSBHostInterface.h>
#include <IOKit/usb/IOUSBHostDevice.h>
#include <IOKit/usb/StandardUSB.h>

#include "kern_broadtek.hpp"
#include "kern_usbclass.hpp"

extern "C" {
#include "bt_core.h"
}

/* HCI over USB (Core spec Vol 4 Part B): commands are class-specific OUT
 * control requests, events arrive on the interrupt IN endpoint.
 *
 * The recipient field is the part worth spelling out. The transport is
 * usually written as bmRequestType 0x20, class request to the *device*, and
 * this dongle addresses the *interface* instead: 0x21. Both are a class OUT
 * request with bRequest 0, and the data stage carries the same HCI command
 * packet either way, so the recipient is not something to filter on.
 *
 * Insisting on 0x20 is what hid bluetoothd's 549 commands behind four rounds
 * of looking for a hook that was already in the right place. */
static inline bool isHCICommand(const StandardUSB::DeviceRequest &request)
{
    return (request.bmRequestType == 0x20 || request.bmRequestType == 0x21) &&
           request.bRequest == 0 && request.wValue == 0;
}
static constexpr size_t  kMaxHCI               = 260;
/* Only the head of an ACL packet is read, because only the head can hold the
 * thing being looked for: an SMP identity PDU is 17 bytes at most, 25 with its
 * L2CAP and ACL headers. A buffer big enough for a whole ACL payload would
 * also be too big for a completion routine's stack -- the compiler rejects a
 * frame over 1024 bytes, and it is right to.
 *
 * A larger frame than this is therefore not SMP, and bt_acl_scan's own length
 * check steps over it rather than reading past what was handed in. */
static constexpr size_t  kMaxACL               = 64;

/* Rewriting comes in two stages, because they carry different risk.
 *
 * Stage one (-broadtekrwev) edits events only, and needs no new logic to be
 * safe: with no command ever handed to the core, the resolving list stays
 * empty and no pending reply is ever registered, so report resolution,
 * connection translation and Command Complete substitution are all inert by
 * construction. What is left is the features rewrite, exactly §4 step 1, and
 * the test is whether bluetoothd then starts sending 2027.
 *
 * Stage two (-broadtekrw) adds the command path, which also means writing
 * back into a control transfer's memory descriptor. That write has never run
 * on this hardware: observe mode never reached it. */
static bool g_rw_events;
static bool g_rw_cmds;
static bt_ctx g_ctx;
static IOSimpleLock *g_lock;

/* advaddr (default on, -broadtekadvaddroff): give each extended advertising set the private address.
 *
 * bluetoothd sets every set up with own address type random (0x2036), but
 * only ever sends the legacy LE Set Random Address (0x2005), never LE Set
 * Advertising Set Random Address (0x2035). A set's address comes from 0x2035
 * alone, so the sets have none of bluetoothd's RPA, and an iPhone holding
 * this Mac's IRK cannot recognise the adverts.
 *
 * The one place BroadTek sends a command of its own: just before bluetoothd
 * enables a set, a 0x2035 carrying the latest 0x2005 address goes out on the
 * same user-client path, synchronously, and the caller waits briefly for its
 * Command Complete. That reply becomes the NOP credit update, so bluetoothd
 * never sees an answer to a command it did not send. */
static bool g_adv_addr;
/* Set once an Intel Bluetooth device appears on USB (or by -broadtekintel). */
static volatile bool g_intel;
static bool g_intel_allowed, g_force_intel;
static bool g_rtl_ready_allowed;
static volatile bool g_rtl_ready;
static OSNumber *g_rtl_phy_events, *g_rtl_dle_events;
static OSNumber *g_rtl_phy_status, *g_rtl_dle_status;
static volatile SInt32 g_intel_inj_pending;
static OSNumber *g_n_intel_pages;
/* Completion context: the link-ready reports (below). Returns the new length
 * if it rewrote buf, 0 to pass the event on as it is. */
static size_t intelOnEvent(uint8_t *buf, size_t len, size_t room);

static bt_adv_state g_adv_state;
static IOLock *g_adv_send_lock;
static bool g_adv_cache;
static uint64_t g_adv_injected, g_adv_inj_timeouts, g_adv_inj_failed, g_adv_inj_cc;
static uint8_t g_adv_inj_status = 0xff;
static OSNumber *g_n_adv_injected, *g_n_adv_inj_timeouts, *g_n_adv_inj_failed, *g_n_adv_inj_cc;
static OSNumber *g_n_adv_inj_status;
static uint64_t g_maps_installed;
static OSNumber *g_n_maps_installed;

/* Lilu compiles DBGLOG out of a release build, and this build exists to be
 * read, so the first packets each hook sees go to the log unconditionally.
 * After that only DBGLOG, which keeps a busy Bluetooth link from filling the
 * kernel buffer. */
static constexpr int kLogFirst = 64;
static int g_logged_cmd;
static int g_logged_evt;

/* The kernel message buffer on the test machine is 128 KB and wraps within
 * minutes of boot, so anything logged while Lilu starts is gone before it can
 * be read. The same facts therefore go into the IO registry, where they can be
 * read at any time, without sudo:
 *
 *     ioreg -l | grep -i broadtek
 */
static int g_routed;
static uint64_t g_cmds;          /* HCI commands, whichever hook saw them */
static uint64_t g_cmds_pipe;     /* ... through IOUSBHostPipe (in-kernel callers) */
static uint64_t g_cmds_uc;       /* ... through a user client's ControlRequest */
static uint64_t g_cmds_dev;      /* ... through IOUSBHostDevice (every control transfer) */
static uint64_t g_cmds_fw;       /* ... through the IOUSBHost framework client */
static uint64_t g_evts;          /* completions on the event pipe */
static uint64_t g_io_calls;      /* every IOUSBHostPipe::io, event pipe or not */
static uint64_t g_acl_in;        /* ACL transfers read from the peer */

/* A command the filter accepted and the parser never saw.
 *
 * Both of these were silent returns, and together they hid most of
 * bluetoothd's traffic: 743 control requests of HCI command type were seen
 * against 251 parsed, and no scan-enable command ever appeared even though
 * advertising reports were arriving by the thousand. A command that cannot be
 * read is a different fact from a command that was not sent, and the two must
 * not look alike again. */
/* Every table below silently stopped recording when it filled up, and that
 * cost this project its central mystery. broadtek-opcodes-uc held exactly 32
 * opcodes against a limit of 32: the startup sequence filled it, so the scan
 * and connection commands that come later could never appear, and for several
 * rounds their absence was read as bluetoothd not sending them.
 * broadtek-ctrl-types was full at 12 the same way, which is why its entries
 * swapped in and out between runs.
 *
 * The limits are now generous, and a full table says so instead of dropping
 * quietly. If broadtek-tables-full is ever not zero, no list is complete. */
static uint64_t g_tables_full;
static OSNumber *g_n_tables_full;

static void tableFull()
{
    g_tables_full++;
    if (g_n_tables_full != nullptr) g_n_tables_full->setValue(g_tables_full);
}

/* Candidate USB transfers outside the selected Bluetooth interface. */
static uint64_t g_pipes_elsewhere;
static OSNumber *g_n_pipes_elsewhere;

static uint64_t g_cmd_nobuf;     /* no data buffer at all */
static uint64_t g_cmd_short;     /* a buffer that yielded fewer than 3 bytes */
/* Keys handed down from userspace, and captures promoted. Counts only: a key
 * never reaches the registry or the log. */
static uint64_t g_keys_installed;
static OSNumber *g_n_keys_installed;
static OSNumber *g_n_rl_entries;

/* Whether an edit actually reached the device. See writeMD(). */
static uint64_t g_write_noprepare;    /* prepare() refused */
static uint64_t g_write_short;        /* prepare() worked, writeBytes refused */
static uint64_t g_write_unverified;   /* wrote, but the read-back disagreed */
static OSNumber *g_n_write_noprepare;
static OSNumber *g_n_write_short;
static OSNumber *g_n_write_unverified;
static OSNumber *g_n_resolved;
static OSNumber *g_n_substituted;
static OSNumber *g_n_conn_events;
static OSNumber *g_n_conn_translated;

static OSNumber *g_n_cmd_nobuf;
static OSNumber *g_n_cmd_short;

/* What is published about a captured IRK, and what is not.
 *
 * An IRK is a cryptographic key, and the IO registry is world readable --
 * `ioreg -l` needs no privileges. So the key never leaves the kext: not into
 * the registry, not into the log. What gets published is how many captures
 * there are and which identity addresses they belong to, and an identity
 * address is not a secret: bluetoothd prints them and they are already in the
 * logs this project quotes. */
static OSNumber *g_n_acl_in;
static OSNumber *g_n_smp_pdus;
static OSNumber *g_n_irks;
static OSNumber *g_n_smp_bad;

/* The counters live in the registry as OSNumbers created once, so updating
 * them is a plain store: no allocation, and therefore safe to do from a USB
 * completion. Publishing only every so often made the numbers stale and the
 * first reading unreadable. */
static OSNumber *g_n_hooks;
static OSNumber *g_n_cmds;
static OSNumber *g_n_cmds_pipe;
static OSNumber *g_n_cmds_uc;
static OSNumber *g_n_cmds_dev;
static OSNumber *g_n_cmds_fw;
static OSNumber *g_n_evts;
static OSNumber *g_n_io_calls;

/* What the commands actually are matters as much as how many: the opcodes say
 * whether this is really the HCI stream and which of them BroadTek must
 * answer. */
static constexpr int kOpcodes = 192;

/* Kept per path, because which path carries which opcodes is the whole
 * question: the in-kernel firmware loader and bluetoothd both speak HCI over
 * the same USB device, and only one of them is the traffic BroadTek wants. */
struct OpcodeSet {
    const char *key;
    uint16_t v[kOpcodes];
    int n;
};

static OpcodeSet g_ops_pipe { "broadtek-opcodes", {}, 0 };
static OpcodeSet g_ops_uc   { "broadtek-opcodes-uc", {}, 0 };
static OpcodeSet g_ops_dev  { "broadtek-opcodes-dev", {}, 0 };
static OpcodeSet g_ops_fw   { "broadtek-opcodes-fw", {}, 0 };

/* Harvested from the replies rather than the commands: a Command Complete
 * carries the opcode of the command it answers, so the event path alone says
 * what bluetoothd is sending while the command path is still unfound.
 * Filled in the completion, published by the next caller. */
static OpcodeSet g_ops_cc   { "broadtek-opcodes-cc", {}, 0 };
static volatile SInt32 g_ops_cc_dirty;

static OSNumber *makeCounter(const char *key)
{
    IORegistryEntry *platform = IOService::getPlatform();
    OSNumber *n = OSNumber::withNumber(static_cast<unsigned long long>(0), 64);
    if (n != nullptr && platform != nullptr) platform->setProperty(key, n);
    return n;
}

static void publishStatus()
{
    if (g_n_hooks != nullptr)     g_n_hooks->setValue(static_cast<uint64_t>(g_routed));
    if (g_n_cmds != nullptr)      g_n_cmds->setValue(g_cmds);
    if (g_n_cmds_pipe != nullptr) g_n_cmds_pipe->setValue(g_cmds_pipe);
    if (g_n_cmds_uc != nullptr)   g_n_cmds_uc->setValue(g_cmds_uc);
    if (g_n_cmds_dev != nullptr)  g_n_cmds_dev->setValue(g_cmds_dev);
    if (g_n_cmds_fw != nullptr)   g_n_cmds_fw->setValue(g_cmds_fw);
    if (g_n_evts != nullptr)      g_n_evts->setValue(g_evts);
    if (g_n_io_calls != nullptr)  g_n_io_calls->setValue(g_io_calls);
    if (g_n_acl_in != nullptr)    g_n_acl_in->setValue(g_acl_in);
    if (g_n_tables_full != nullptr) g_n_tables_full->setValue(g_tables_full);
    if (g_n_pipes_elsewhere != nullptr) g_n_pipes_elsewhere->setValue(g_pipes_elsewhere);
    if (g_n_cmd_nobuf != nullptr) g_n_cmd_nobuf->setValue(g_cmd_nobuf);
    if (g_n_keys_installed != nullptr) g_n_keys_installed->setValue(g_keys_installed);
    if (g_n_rl_entries != nullptr) {
        uint64_t live = 0;
        for (int i = 0; i < BT_RL_SIZE; i++) if (g_ctx.rl[i].valid) live++;
        g_n_rl_entries->setValue(live);
    }
    if (g_n_resolved != nullptr)    g_n_resolved->setValue(g_ctx.resolved);
    if (g_n_substituted != nullptr) g_n_substituted->setValue(g_ctx.substituted);
    if (g_n_conn_events != nullptr)     g_n_conn_events->setValue(g_ctx.conn_events);
    if (g_n_conn_translated != nullptr) g_n_conn_translated->setValue(g_ctx.conn_translated);
    if (g_n_write_noprepare != nullptr) g_n_write_noprepare->setValue(g_write_noprepare);
    if (g_n_write_short != nullptr)     g_n_write_short->setValue(g_write_short);
    if (g_n_write_unverified != nullptr)
        g_n_write_unverified->setValue(g_write_unverified);
    if (g_n_cmd_short != nullptr) g_n_cmd_short->setValue(g_cmd_short);
    if (g_n_smp_pdus != nullptr)  g_n_smp_pdus->setValue(g_ctx.smp_pdus);
    if (g_n_irks != nullptr)      g_n_irks->setValue(static_cast<uint64_t>(bt_smp_complete(&g_ctx)));
    if (g_n_smp_bad != nullptr)
        g_n_smp_bad->setValue(g_ctx.smp_truncated + g_ctx.acl_continuations);
}

/* Caller context only (an allocation). */
static void publishOpcodes(const OpcodeSet &set)
{
    IORegistryEntry *platform = IOService::getPlatform();
    if (platform == nullptr || set.n <= 0) return;
    OSData *d = OSData::withBytes(set.v, static_cast<unsigned>(set.n) * sizeof(uint16_t));
    if (d == nullptr) return;
    platform->setProperty(set.key, d);
    d->release();
}

/* The distinct callers seen, as class names. Caller context only. */
static constexpr int kClients = 8;
static const OSMetaClass *g_client[kClients];
static int g_n_client;

static void noteClient(IOService *forClient)
{
    if (forClient == nullptr) return;
    const OSMetaClass *mc = forClient->getMetaClass();
    if (mc == nullptr) return;
    for (int i = 0; i < g_n_client; i++)
        if (g_client[i] == mc) return;
    if (g_n_client >= kClients) { tableFull(); return; }
    g_client[g_n_client++] = mc;

    char names[512];
    size_t at = 0;
    for (int i = 0; i < g_n_client; i++) {
        const char *nm = g_client[i]->getClassName();
        if (nm == nullptr) continue;
        if (at > 0 && at + 1 < sizeof(names)) names[at++] = ' ';
        for (const char *q = nm; *q != '\0' && at + 1 < sizeof(names); q++) names[at++] = *q;
    }
    names[at] = '\0';

    IORegistryEntry *platform = IOService::getPlatform();
    if (platform != nullptr) platform->setProperty("broadtek-cmd-clients", names);
}

/* Transfer counts for the selected interface. Only caller context publishes
 * strings in the registry; completions mark the other tables dirty. */
static constexpr int kEndpoints = 16;
struct EndpointUse {
    uint8_t  addr;
    uint8_t  type;
    uint64_t count;
};
static EndpointUse g_eps[kEndpoints];
static int g_n_eps;

static constexpr int kEvtCodes = 64;
static uint8_t g_evt_code[kEvtCodes];
static int g_n_evt_code;
static volatile SInt32 g_evt_code_dirty;

/* HCI events are code, parameter length, then exactly that many bytes. */
static uint64_t g_evt_shaped;
static uint64_t g_evt_other;
static OSNumber *g_n_evt_shaped;
static OSNumber *g_n_evt_other;

/* Completion context: no allocation, no locks. */
static void noteEventCode(uint8_t code)
{
    for (int i = 0; i < g_n_evt_code; i++)
        if (g_evt_code[i] == code) return;
    if (g_n_evt_code >= kEvtCodes) { tableFull(); return; }
    g_evt_code[g_n_evt_code++] = code;
    g_evt_code_dirty = 1;
}

static const char *epTypeName(uint8_t type)
{
    switch (type) {
    case 0:  return "ctrl";
    case 1:  return "isoc";
    case 2:  return "bulk";
    default: return "int";
    }
}

/* The identity addresses a capture belongs to, as text. Not the keys. Set
 * from the completion, published by the next caller that may allocate. */
static volatile SInt32 g_irk_dirty;
static void publishCaptures();
static volatile SInt32 g_adv_dirty;
static void publishAdv();
static volatile SInt32 g_nearby_dirty;
static void publishNearby();
static volatile SInt32 g_l2cap_dirty;
static void publishL2cap();
static void noteL2cap(int dir, const uint8_t *acl, size_t len);
static void clearL2capHandle(uint16_t handle);

/* Caller context only. */
static void publishTraffic(const EndpointUse *eps, int count)
{
    IORegistryEntry *platform = IOService::getPlatform();
    if (platform == nullptr) return;

    char line[256];
    size_t at = 0;
    for (int i = 0; i < count && at + 32 < sizeof(line); i++) {
        at += static_cast<size_t>(snprintf(line + at, sizeof(line) - at, "%s%02x:%s:%llu",
                                           at > 0 ? " " : "", eps[i].addr,
                                           epTypeName(eps[i].type), eps[i].count));
    }
    line[at] = '\0';
    platform->setProperty("broadtek-endpoints", line);

    if (OSCompareAndSwap(1, 0, reinterpret_cast<volatile UInt32 *>(&g_ops_cc_dirty)))
        publishOpcodes(g_ops_cc);

    if (OSCompareAndSwap(1, 0, reinterpret_cast<volatile UInt32 *>(&g_irk_dirty)))
        publishCaptures();

    if (OSCompareAndSwap(1, 0, reinterpret_cast<volatile UInt32 *>(&g_adv_dirty)))
        publishAdv();

    if (OSCompareAndSwap(1, 0, reinterpret_cast<volatile UInt32 *>(&g_nearby_dirty)))
        publishNearby();

    if (OSCompareAndSwap(1, 0, reinterpret_cast<volatile UInt32 *>(&g_l2cap_dirty)))
        publishL2cap();

    if (OSCompareAndSwap(1, 0, reinterpret_cast<volatile UInt32 *>(&g_evt_code_dirty))) {
        OSData *d = OSData::withBytes(g_evt_code, static_cast<unsigned>(g_n_evt_code));
        if (d != nullptr) {
            platform->setProperty("broadtek-event-codes", d);
            d->release();
        }
    }
}

/* Caller context only. */
static void noteEndpoint(const StandardUSB::EndpointDescriptor *ed)
{
    if (ed == nullptr) return;
    static uint64_t lastPublish;
    EndpointUse snapshot[kEndpoints];
    const uint8_t addr = ed->bEndpointAddress, type = ed->bmAttributes & 3;
    uint64_t now;
    absolutetime_to_nanoseconds(mach_absolute_time(), &now);
    IOSimpleLockLock(g_lock);
    int i = 0;
    while (i < g_n_eps && (g_eps[i].addr != addr || g_eps[i].type != type)) i++;
    if (i == kEndpoints) { IOSimpleLockUnlock(g_lock); tableFull(); return; }
    bool fresh = i == g_n_eps;
    if (fresh) g_eps[g_n_eps++] = EndpointUse{addr, type, 0};
    g_eps[i].count++;
    if (!fresh && (now <= lastPublish || now - lastPublish < 250000000ULL)) {
        IOSimpleLockUnlock(g_lock);
        return;
    }
    lastPublish = now;
    int count = g_n_eps;
    memcpy(snapshot, g_eps, static_cast<size_t>(count) * sizeof(EndpointUse));
    IOSimpleLockUnlock(g_lock);
    publishTraffic(snapshot, count);
}

/* Completion context: record only, never publish. */
static void publishCaptures()
{
    IORegistryEntry *platform = IOService::getPlatform();
    if (platform == nullptr) return;

    char line[256];
    size_t at = 0;
    for (int i = 0; i < BT_SMP_MAX && at + 24 < sizeof(line); i++) {
        const bt_smp_capture &cap = g_ctx.smp[i];
        if (!cap.used || !cap.have_id) continue;
        at += static_cast<size_t>(snprintf(line + at, sizeof(line) - at,
                                           "%s%02x:%02x:%02x:%02x:%02x:%02x/%s%s",
                                           at > 0 ? " " : "",
                                           cap.id_addr[5], cap.id_addr[4], cap.id_addr[3],
                                           cap.id_addr[2], cap.id_addr[1], cap.id_addr[0],
                                           cap.id_type ? "random" : "public",
                                           cap.have_irk ? "+key" : "-key"));
    }
    line[at] = '\0';
    if (at > 0) platform->setProperty("broadtek-irk-ids", line);
}

/* The address bluetoothd actually asks the controller to connect to.
 *
 * This is the design's central question and it has never been observed: every
 * run so far caught only bluetoothd's startup, with no LE connection command
 * in it at all, because nobody tried Instant Hotspot while the hooks were in.
 *
 * What matters is the address *type* as much as the bytes. A public identity
 * address means macOS is asking the controller to do something it cannot --
 * the failure this project exists to fix. A random address whose top two bits
 * are 01 is a resolvable private address, which would mean bluetoothd has
 * already resolved the peer host-side and no IRK is needed from anywhere.
 *
 * Addresses are not secret: bluetoothd prints them and ioreg already carries
 * the controller's own. Keys are a different matter and are never published. */
static constexpr int kTargets = 16;
struct ConnTarget {
    uint16_t opcode;
    uint8_t  type;
    uint8_t  addr[6];
};
static ConnTarget g_targets[kTargets];
static int g_n_targets;

static const char *opcodeTag(uint16_t opcode)
{
    switch (opcode) {
    case 0x2011: return "fal+";
    case 0x2012: return "fal-";
    case 0x200d: return "conn";
    case 0x2043: return "econn";
    default:     return "?";
    }
}

/* ---- advertising capture (read-only) -----------------------------------
 * Why the iPhone never resolves this Mac's adverts comes down to what the
 * controller is told: the own-address type in LE Set Extended Advertising
 * Parameters (0x2036), and whether a random address ever reaches the set
 * (0x2035) or only the legacy slot (0x2005). The last parameters of each
 * advertising command, per advertising handle, are kept here with a count,
 * and the status of each one's Command Complete next to them. Addresses are
 * not secret (bluetoothd logs its own); nothing else is captured. */
static constexpr int kAdvRecs = 24;
static constexpr int kAdvBytes = 26;
struct AdvRec {
    uint16_t opcode;
    uint8_t  handle;        /* advertising handle, or 0xff when none */
    uint8_t  len;
    uint8_t  p[kAdvBytes];
    uint32_t count;
};
static AdvRec g_adv[kAdvRecs];
static int g_n_adv;

/* Command Complete status per advertising opcode, and the last Advertising
 * Set Terminated. Written in the completion, published by the next caller. */
struct AdvStatus { uint16_t opcode; uint8_t status; uint8_t seen; uint32_t fails; };
static AdvStatus g_adv_st[8];
static uint8_t g_adv_term[6];

static bool isAdvOpcode(uint16_t op)
{
    switch (op) {
    case 0x2005: case 0x2006: case 0x2008: case 0x200a:
    case 0x2035: case 0x2036: case 0x2037: case 0x2039:
    case 0x203c: case 0x203d:
        return true;
    default:
        return false;
    }
}

/* One publisher at a time: the buffers are static (too big for the kernel
 * stack) and the first build, which published straight from every command,
 * panicked in setProperty with two bluetoothd threads in here at once. */
static volatile SInt32 g_adv_busy;

static void publishAdv()
{
    if (!OSCompareAndSwap(0, 1, reinterpret_cast<volatile UInt32 *>(&g_adv_busy))) {
        g_adv_dirty = 1;                        /* someone else is in: try again later */
        return;
    }
    IORegistryEntry *platform = IOService::getPlatform();
    if (platform == nullptr) { g_adv_busy = 0; return; }

    static char line[1600];          /* too big for the kernel stack */
    size_t at = 0;
    for (int i = 0; i < g_n_adv && at + 80 < sizeof(line); i++) {
        const AdvRec &r = g_adv[i];
        at += static_cast<size_t>(snprintf(line + at, sizeof(line) - at, "%s%04x/%02x#%u:",
                                           at > 0 ? " " : "", r.opcode, r.handle, r.count));
        for (int k = 0; k < r.len && at + 4 < sizeof(line); k++)
            at += static_cast<size_t>(snprintf(line + at, sizeof(line) - at, "%02x", r.p[k]));
    }
    line[at] = '\0';
    /* The keys are held as symbols for the life of the kext: the panic was
     * in OSSymbol::free of a key symbol, which a held reference rules out. */
    static const OSSymbol *k_cmds, *k_status;
    if (k_cmds == nullptr) k_cmds = OSSymbol::withCStringNoCopy("broadtek-adv-cmds");
    if (k_status == nullptr) k_status = OSSymbol::withCStringNoCopy("broadtek-adv-status");
    if (k_cmds == nullptr || k_status == nullptr) {
        OSCompareAndSwap(1, 0, reinterpret_cast<volatile UInt32 *>(&g_adv_busy));
        return;
    }
    if (OSString *v = OSString::withCString(line)) { platform->setProperty(k_cmds, v); v->release(); }

    static char st[256];
    at = 0;
    for (int i = 0; i < 8 && at + 32 < sizeof(st); i++) {
        if (!g_adv_st[i].seen) continue;
        at += static_cast<size_t>(snprintf(st + at, sizeof(st) - at, "%s%04x:%02x/fails%u",
                                           at > 0 ? " " : "", g_adv_st[i].opcode,
                                           g_adv_st[i].status, g_adv_st[i].fails));
    }
    if (g_adv_term[0] != 0 || g_adv_term[1] != 0)
        at += static_cast<size_t>(snprintf(st + at, sizeof(st) - at,
                                           " term:%02x%02x%02x%02x%02x", g_adv_term[0],
                                           g_adv_term[1], g_adv_term[2], g_adv_term[3],
                                           g_adv_term[4]));
    st[at < sizeof(st) ? at : sizeof(st) - 1] = '\0';
    if (OSString *v = OSString::withCString(st)) { platform->setProperty(k_status, v); v->release(); }
    OSCompareAndSwap(1, 0, reinterpret_cast<volatile UInt32 *>(&g_adv_busy));
}

/* Caller context (it publishes a string). */
static void noteAdvCommand(const uint8_t *pkt, size_t len)
{
    if (len < 3) return;
    uint16_t opcode = static_cast<uint16_t>(pkt[0] | (pkt[1] << 8));
    if (!isAdvOpcode(opcode)) return;
    uint8_t plen = pkt[2];
    if (len < static_cast<size_t>(3) + plen) return;
    const uint8_t *p = &pkt[3];

    /* The extended commands start with the advertising handle; Set Extended
     * Advertising Enable starts with enable, num_sets, then the handle. */
    uint8_t handle = 0xff;
    if (opcode == 0x2035 || opcode == 0x2036 || opcode == 0x2037 || opcode == 0x203c)
        handle = plen >= 1 ? p[0] : 0xff;
    else if (opcode == 0x2039)
        handle = plen >= 3 ? static_cast<uint8_t>((p[0] << 7) | p[2]) : 0xff;

    AdvRec *r = nullptr;
    for (int i = 0; i < g_n_adv; i++)
        if (g_adv[i].opcode == opcode && g_adv[i].handle == handle) { r = &g_adv[i]; break; }
    if (r == nullptr) {
        if (g_n_adv >= kAdvRecs) { tableFull(); return; }
        r = &g_adv[g_n_adv++];
        r->opcode = opcode;
        r->handle = handle;
        r->count = 0;
    }
    r->count++;
    r->len = plen < kAdvBytes ? plen : kAdvBytes;
    memcpy(r->p, p, r->len);
    g_adv_dirty = 1;               /* published by publishTraffic, one caller at a time */
}

/* Completion context: no allocation, no locks. */
static void noteAdvEvent(const uint8_t *buf, size_t len)
{
    if (buf[0] == 0x0e && len >= 6) {
        uint16_t op = static_cast<uint16_t>(buf[3] | (buf[4] << 8));
        if (!isAdvOpcode(op)) return;
        for (int i = 0; i < 8; i++) {
            if (g_adv_st[i].seen && g_adv_st[i].opcode != op) continue;
            g_adv_st[i].opcode = op;
            g_adv_st[i].status = buf[5];
            g_adv_st[i].seen = 1;
            if (buf[5] != 0) g_adv_st[i].fails++;
            g_adv_dirty = 1;
            return;
        }
    } else if (buf[0] == 0x0f && len >= 6) {
        uint16_t op = static_cast<uint16_t>(buf[4] | (buf[5] << 8));
        if (!isAdvOpcode(op) || buf[2] == 0) return;
        for (int i = 0; i < 8; i++) {
            if (g_adv_st[i].seen && g_adv_st[i].opcode != op) continue;
            g_adv_st[i].opcode = op;
            g_adv_st[i].status = buf[2];
            g_adv_st[i].seen = 1;
            g_adv_st[i].fails++;
            g_adv_dirty = 1;
            return;
        }
    } else if (buf[0] == 0x3e && len >= 8 && buf[2] == 0x12) {
        /* Advertising Set Terminated: status, handle, conn handle(2), events */
        for (int i = 0; i < 5; i++) g_adv_term[i] = buf[3 + i];
        g_adv_dirty = 1;
    }
}

/* ---- NearbyInfo capture (read-only) -------------------------------------
 * The iPhone has no IRK on this Mac, so its RPAs cannot be resolved the way
 * the Watch's were. But macOS recognises the phone without one: it joins the
 * phone's identity to a 3-byte auth tag inside the NearbyInfo advert (Apple
 * manufacturer sub-type 0x10). If BroadTek learns identity<->RPA the same way,
 * the proven accept-list substitution reaches the phone too.
 *
 * Step one is only to see the raw material: for each advert, the random
 * address it came from and the bytes of its type-0x10 TLV. Joining those to
 * bluetoothd's own "Addr <identity>, AT <tag>" log lines, by the tag, gives
 * identity<->RPA; whether a stored key reproduces the tag is the probe's
 * question, not the kext's. Addresses and advert payloads are broadcast in
 * the clear; nothing secret is touched here. */
static constexpr int kNearbyRecs = 48;
static constexpr int kNbTlvMax = 14;
struct NearbyRec {
    uint8_t  addr[6];
    uint8_t  tlv[kNbTlvMax];        /* the type-0x10 value, type and len included */
    uint8_t  tlv_len;
    uint32_t count;
    uint32_t seq;                   /* when it was last seen, for eviction */
};
static NearbyRec g_nearby[kNearbyRecs];
static int g_n_nearby;
static uint32_t g_nearby_seq;

/* Find Apple's type-0x10 TLV inside one advert's AD payload. Returns its
 * length (type+len+value, capped) and copies it to out, or 0 if absent. */
static uint8_t findNearbyTlv(const uint8_t *data, size_t dlen, uint8_t *out)
{
    size_t off = 0;
    while (off + 2 <= dlen) {
        uint8_t adlen = data[off];
        if (adlen == 0 || off + 1 + adlen > dlen) break;
        uint8_t adtype = data[off + 1];
        /* Manufacturer specific: 4c 00, then Apple sub-TLVs [type,len,value]. */
        if (adtype == 0xff && adlen >= 4 && data[off + 2] == 0x4c && data[off + 3] == 0x00) {
            size_t s = off + 4;
            size_t end = off + 1 + adlen;
            while (s + 2 <= end) {
                uint8_t t = data[s];
                uint8_t l = data[s + 1];
                if (s + 2 + l > end) break;
                if (t == 0x10) {
                    uint8_t n = (uint8_t)(l + 2);
                    if (n > kNbTlvMax) n = kNbTlvMax;
                    for (uint8_t i = 0; i < n; i++) out[i] = data[s + i];
                    return n;
                }
                s += 2 + l;
            }
        }
        off += 1 + adlen;
    }
    return 0;
}

/* Completion context: no allocation, no locks. */
static void noteNearbyAdvert(uint8_t addr_type, const uint8_t *addr,
                             const uint8_t *data, size_t dlen)
{
    if (addr_type != 0x01) return;                 /* only random addresses */
    if ((addr[5] & 0xc0) != 0x40) return;          /* only RPAs */
    uint8_t tlv[kNbTlvMax];
    uint8_t n = findNearbyTlv(data, dlen, tlv);
    if (n == 0) return;

    for (int i = 0; i < g_n_nearby; i++) {
        bool same = true;
        for (int k = 0; k < 6; k++) if (g_nearby[i].addr[k] != addr[k]) { same = false; break; }
        if (same) {
            g_nearby[i].count++;
            g_nearby[i].seq = ++g_nearby_seq;
            g_nearby_dirty = 1;
            return;
        }
    }

    /* A full table must not lock out the device this exists for. Addresses
     * rotate, so the set of them is unbounded over time and the first 48 seen
     * are no more interesting than the rest: the least recently seen goes. */
    NearbyRec *r;
    if (g_n_nearby < kNearbyRecs) {
        r = &g_nearby[g_n_nearby++];
    } else {
        tableFull();
        r = &g_nearby[0];
        for (int i = 1; i < g_n_nearby; i++)
            if (g_nearby[i].seq < r->seq) r = &g_nearby[i];
    }
    for (int k = 0; k < 6; k++) r->addr[k] = addr[k];
    r->tlv_len = n;
    for (uint8_t k = 0; k < n; k++) r->tlv[k] = tlv[k];
    r->count = 1;
    r->seq = ++g_nearby_seq;
    g_nearby_dirty = 1;
}

/* Walk one LE Meta event's advertising reports, exactly as bt_hci.c does, and
 * record the NearbyInfo tag of each. Completion context. */
static void noteNearby(const uint8_t *buf, size_t len)
{
    if (buf[0] != 0x3e || len < 3) return;
    uint8_t plen = buf[1];
    if ((size_t)plen + 2 > len) return;
    const uint8_t *p = &buf[2];
    uint8_t sub = p[0];
    const uint8_t *q = &p[1];
    size_t avail = (size_t)plen - 1;
    if (avail < 1) return;
    uint8_t nrep = q[0];
    size_t off = 1;

    if (sub == 0x02) {                              /* LE Advertising Report */
        for (uint8_t i = 0; i < nrep; i++) {
            if (off + 9 > avail) break;
            uint8_t dlen = q[off + 8];
            if (off + 9 + dlen + 1 > avail) break;
            noteNearbyAdvert(q[off + 1], &q[off + 2], &q[off + 9], dlen);
            off += 9 + dlen + 1;
        }
    } else if (sub == 0x0d) {                        /* LE Extended Advertising Report */
        for (uint8_t i = 0; i < nrep; i++) {
            if (off + 24 > avail) break;
            uint8_t dlen = q[off + 23];
            if (off + 24 + dlen > avail) break;
            noteNearbyAdvert(q[off + 2], &q[off + 3], &q[off + 24], dlen);
            off += 24 + dlen;
        }
    }
}

static volatile SInt32 g_nearby_busy;

static void publishNearby()
{
    if (!OSCompareAndSwap(0, 1, reinterpret_cast<volatile UInt32 *>(&g_nearby_busy))) {
        g_nearby_dirty = 1;
        return;
    }
    IORegistryEntry *platform = IOService::getPlatform();
    if (platform == nullptr) { g_nearby_busy = 0; return; }

    static char line[3072];
    size_t at = 0;
    for (int i = 0; i < g_n_nearby && at + 48 < sizeof(line); i++) {
        const NearbyRec &r = g_nearby[i];
        at += (size_t)snprintf(line + at, sizeof(line) - at,
                               "%s%02x%02x%02x%02x%02x%02x#%u:", at > 0 ? " " : "",
                               r.addr[0], r.addr[1], r.addr[2], r.addr[3], r.addr[4], r.addr[5],
                               r.count);
        for (uint8_t k = 0; k < r.tlv_len && at + 4 < sizeof(line); k++)
            at += (size_t)snprintf(line + at, sizeof(line) - at, "%02x", r.tlv[k]);
    }
    line[at < sizeof(line) ? at : sizeof(line) - 1] = '\0';

    static const OSSymbol *key;
    if (key == nullptr) key = OSSymbol::withCStringNoCopy("broadtek-nearby");
    if (key != nullptr) {
        if (OSString *v = OSString::withCString(line)) { platform->setProperty(key, v); v->release(); }
    }
    OSCompareAndSwap(1, 0, reinterpret_cast<volatile UInt32 *>(&g_nearby_busy));
}

/* Caller context (it publishes a string). */
static void noteConnTarget(const uint8_t *pkt, size_t len)
{
    noteAdvCommand(pkt, len);
    if (len < 3) return;
    uint16_t opcode = static_cast<uint16_t>(pkt[0] | (pkt[1] << 8));
    uint8_t plen = pkt[2];
    const uint8_t *p = &pkt[3];
    if (len < static_cast<size_t>(3) + plen) return;

    uint8_t type;
    const uint8_t *addr;

    switch (opcode) {
    case 0x2011:                                  /* LE Add To Accept List */
    case 0x2012:                                  /* LE Remove From Accept List */
        if (plen < 7) return;
        type = p[0]; addr = &p[1];
        break;
    case 0x200d:                                  /* LE Create Connection */
        if (plen < 13) return;
        type = p[5]; addr = &p[6];
        break;
    case 0x2043:                                  /* LE Extended Create Connection */
        if (plen < 10) return;
        type = p[2]; addr = &p[3];
        break;
    default:
        return;
    }

    for (int i = 0; i < g_n_targets; i++)
        if (g_targets[i].opcode == opcode && g_targets[i].type == type &&
            memcmp(g_targets[i].addr, addr, 6) == 0) return;
    if (g_n_targets >= kTargets) { tableFull(); return; }
    g_targets[g_n_targets].opcode = opcode;
    g_targets[g_n_targets].type = type;
    memcpy(g_targets[g_n_targets].addr, addr, 6);
    g_n_targets++;

    IORegistryEntry *platform = IOService::getPlatform();
    if (platform == nullptr) return;

    char line[448];
    size_t at = 0;
    for (int i = 0; i < g_n_targets && at + 32 < sizeof(line); i++) {
        const ConnTarget &t = g_targets[i];
        /* An RPA is a random address with 01 in the top two bits. */
        const char *kind = t.type == 0 ? "pub" :
                           ((t.addr[5] & 0xc0) == 0x40 ? "RPA" : "rnd");
        at += static_cast<size_t>(snprintf(line + at, sizeof(line) - at,
                                           "%s%s/%s/%02x:%02x:%02x:%02x:%02x:%02x",
                                           at > 0 ? " " : "", opcodeTag(t.opcode), kind,
                                           t.addr[5], t.addr[4], t.addr[3],
                                           t.addr[2], t.addr[1], t.addr[0]));
    }
    line[at] = '\0';
    platform->setProperty("broadtek-conn-targets", line);
}

static void noteOpcodeQuiet(OpcodeSet &set, uint16_t opcode, volatile SInt32 *dirty)
{
    for (int i = 0; i < set.n; i++)
        if (set.v[i] == opcode) return;
    if (set.n >= kOpcodes) { tableFull(); return; }
    set.v[set.n++] = opcode;
    *dirty = 1;
}

/* The filter below rejected everything bluetoothd sent, and a rejected packet
 * and an uncalled hook look identical in a counter. So every control request
 * is counted before any filtering, and its bmRequestType/bRequest recorded.
 * Whatever bluetoothd's HCI commands look like on this transport, this says
 * so rather than assuming the spec's 0x20/0x00. */
static constexpr int kCtrlTypes = 48;
struct CtrlType {
    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wLength;
    uint64_t count;
};
static CtrlType g_ctrl[kCtrlTypes];
static int g_n_ctrl;
static uint64_t g_ctrl_calls;
static OSNumber *g_n_ctrl_calls;

/* Caller context (it publishes a string). */
static void noteControl(const StandardUSB::DeviceRequest &request)
{
    g_ctrl_calls++;
    if (g_n_ctrl_calls != nullptr) g_n_ctrl_calls->setValue(g_ctrl_calls);

    CtrlType *e = nullptr;
    bool fresh = false;
    for (int i = 0; i < g_n_ctrl; i++)
        if (g_ctrl[i].bmRequestType == request.bmRequestType &&
            g_ctrl[i].bRequest == request.bRequest) { e = &g_ctrl[i]; break; }
    if (e == nullptr) {
        if (g_n_ctrl >= kCtrlTypes) { tableFull(); return; }
        e = &g_ctrl[g_n_ctrl++];
        e->bmRequestType = request.bmRequestType;
        e->bRequest = request.bRequest;
        e->wLength = request.wLength;
        e->count = 0;
        fresh = true;
    }
    e->count++;

    /* Rebuilding the string allocates, and the firmware loader alone sends 253
     * of these in a burst at boot. A new kind of request is worth publishing
     * at once; the counts can lag. */
    if (!fresh && (g_ctrl_calls & 0x3f) != 0) return;

    IORegistryEntry *platform = IOService::getPlatform();
    if (platform == nullptr) return;

    char line[512];
    size_t at = 0;
    for (int i = 0; i < g_n_ctrl && at + 32 < sizeof(line); i++) {
        at += static_cast<size_t>(snprintf(line + at, sizeof(line) - at, "%s%02x/%02x:%llu",
                                           at > 0 ? " " : "", g_ctrl[i].bmRequestType,
                                           g_ctrl[i].bRequest, g_ctrl[i].count));
    }
    line[at] = '\0';
    platform->setProperty("broadtek-ctrl-types", line);
}

static void noteOpcode(OpcodeSet &set, uint16_t opcode)
{
    for (int i = 0; i < set.n; i++)
        if (set.v[i] == opcode) return;           /* each kind once */
    if (set.n >= kOpcodes) { tableFull(); return; }
    set.v[set.n++] = opcode;
    publishOpcodes(set);
}

#define OBSLOG(counter, ...) do {                 \
    if ((counter) < kLogFirst) {                  \
        (counter)++;                              \
        SYSLOG("broadtek", __VA_ARGS__);          \
    } else {                                      \
        DBGLOG("broadtek", __VA_ARGS__);          \
    }                                             \
} while (0)

static mach_vm_address_t g_orig_ctrl_async;
static mach_vm_address_t g_orig_ctrl_sync;
static mach_vm_address_t g_orig_io_async;
static mach_vm_address_t g_orig_uc_ctrl;
static mach_vm_address_t g_orig_dev_req;
static mach_vm_address_t g_orig_fw_dev_async;
static mach_vm_address_t g_orig_fw_dev_sync;
static mach_vm_address_t g_orig_fw_pipe_async;
static mach_vm_address_t g_orig_fw_pipe_sync;

/* ---- helpers -------------------------------------------------------- */

static size_t readMD(IOMemoryDescriptor *md, uint8_t *buf, size_t cap)
{
    if (md == nullptr) return 0;
    size_t len = md->getLength();
    if (len > cap) len = cap;
    if (len == 0) return 0;
    if (md->prepare(kIODirectionNone) != kIOReturnSuccess) return 0;
    size_t got = md->readBytes(0, buf, len);
    md->complete(kIODirectionNone);
    return got;
}

/* Writing an edit back, and checking it landed.
 *
 * This is the one operation in the plugin that had never run on hardware:
 * observe-only and stage `events` both stop short of it. It is also the
 * likeliest thing to fail quietly, because the buffer belongs to a user
 * client and may well not be writable from here.
 *
 * The old version ignored both ways it can fail -- a prepare() that does not
 * succeed, and a writeBytes() that writes fewer bytes than asked -- so an
 * edit that never reached the controller looked exactly like one that did.
 * The core's `substituted` counter does not help: it counts the edit to
 * BroadTek's own stack copy, not what the device received.
 *
 * So the write is read back and compared. Only the first 32 bytes, which is
 * every address this ever substitutes and keeps the buffer small enough for a
 * completion routine's stack. */
static constexpr size_t kVerifyWindow = 32;

static bool writeMD(IOMemoryDescriptor *md, const uint8_t *buf, size_t len)
{
    if (md == nullptr || len == 0) return true;

    if (md->prepare(kIODirectionNone) != kIOReturnSuccess) {
        g_write_noprepare++;
        if (g_n_write_noprepare != nullptr) g_n_write_noprepare->setValue(g_write_noprepare);
        return false;
    }

    IOByteCount wrote = md->writeBytes(0, buf, len);

    uint8_t back[kVerifyWindow];
    size_t window = len < kVerifyWindow ? len : kVerifyWindow;
    IOByteCount got = md->readBytes(0, back, window);

    md->complete(kIODirectionNone);

    if (wrote != len) {
        g_write_short++;
        if (g_n_write_short != nullptr) g_n_write_short->setValue(g_write_short);
        return false;
    }
    if (got != window || memcmp(back, buf, window) != 0) {
        g_write_unverified++;
        if (g_n_write_unverified != nullptr)
            g_n_write_unverified->setValue(g_write_unverified);
        return false;
    }
    return true;
}

static uint32_t writeEvent(IOMemoryDescriptor *md, const uint8_t *buf, size_t len, IOReturn &status)
{
    if (writeMD(md, buf, len)) return static_cast<uint32_t>(len);
    status = kIOReturnIOError;
    return 0;
}

/* Endpoint addresses are local to an interface. Remember the actual pipes
 * returned for the HCI interface, including replacements after a USB reset.
 * The HCI core, like bluetoothd on these machines, uses one controller. */
enum class BtPipe { Event, AclIn, AclOut, Other };
static uint64_t g_controller_generation;
static uint8_t g_hci_interface_number;
static IOUSBHostInterface *g_bt_interface;
static IOUSBHostDevice *g_bt_device;
static IOUSBHostPipe *g_bt_pipes[3];
static mach_vm_address_t g_orig_copy_pipe;

static BtPipe endpointRole(const StandardUSB::EndpointDescriptor *ed)
{
    if (ed != nullptr) {
        uint8_t type = ed->bmAttributes & 3;
        bool input = (ed->bEndpointAddress & 0x80) != 0;
        if (type == kIOUSBEndpointTypeInterrupt && input) return BtPipe::Event;
        if (type == kIOUSBEndpointTypeBulk) return input ? BtPipe::AclIn : BtPipe::AclOut;
    }
    return BtPipe::Other;
}

static void rememberPipe(IOUSBHostInterface *interface, IOUSBHostPipe *pipe)
{
    if (pipe == nullptr) return;
    BtPipe role = endpointRole(pipe->getEndpointDescriptor());
    if (role == BtPipe::Other) return;
    IOUSBHostPipe *old = nullptr;
    IOSimpleLockLock(g_lock);
    auto &slot = g_bt_pipes[static_cast<unsigned>(role)];
    if (interface == g_bt_interface && slot != pipe) {
        pipe->retain();
        old = slot;
        slot = pipe;
    }
    IOSimpleLockUnlock(g_lock);
    if (old != nullptr) old->release();
}

static IOReturn onCopyPipe(IOUSBHostInterface *interface, uint8_t address, IOUSBHostPipe *&pipe)
{
    IOReturn result = FunctionCast(onCopyPipe, g_orig_copy_pipe)(interface, address, pipe);
    if (result == kIOReturnSuccess) rememberPipe(interface, pipe);
    return result;
}

static bool onBluetoothInterface(void *, void *, IOService *service, IONotifier *)
{
    auto *interface = OSDynamicCast(IOUSBHostInterface, service);
    if (interface == nullptr) return true;
    auto *id = interface->getInterfaceDescriptor();
    auto *config = interface->getConfigurationDescriptor();
    auto *device = interface->getDevice();
    if (id == nullptr || config == nullptr || device == nullptr ||
        id->bInterfaceClass != 0xe0 || id->bInterfaceSubClass != 1 || id->bInterfaceProtocol != 1)
        return true;

    /* The SCO interface can have the same class triple. Only the primary
     * HCI interface has the interrupt event and both bulk ACL endpoints. */
    const StandardUSB::EndpointDescriptor *ed = nullptr;
    uint8_t addresses[3] = {};
    while ((ed = StandardUSB::getNextEndpointDescriptor(config, id, ed)) != nullptr) {
        BtPipe role = endpointRole(ed);
        if (role != BtPipe::Other) addresses[static_cast<unsigned>(role)] = ed->bEndpointAddress;
    }
    if (!addresses[0] || !addresses[1] || !addresses[2]) return true;

    auto *vendor = OSDynamicCast(OSNumber, device->getProperty("idVendor"));
    auto *product = OSDynamicCast(OSNumber, device->getProperty("idProduct"));
    const unsigned vid = vendor ? vendor->unsigned32BitValue() : 0;
    const bool rtlReady = g_rtl_ready_allowed && (vid == 0x0a12 || vid == 0x1358) &&
                          product && product->unsigned32BitValue() == 0xc123;
    bool intel = g_intel_allowed && (g_force_intel || (vendor && vendor->unsigned32BitValue() == 0x8087));
    IOSimpleLockLock(g_lock);
    if (g_bt_interface != nullptr) { IOSimpleLockUnlock(g_lock); return true; }
    interface->retain();
    device->retain();
    g_controller_generation++;
    g_hci_interface_number = id->bInterfaceNumber;
    g_bt_interface = interface;
    g_bt_device = device;
    g_intel = intel;
    g_rtl_ready = rtlReady && !intel;
    IOSimpleLockUnlock(g_lock);
    if (auto *platform = IOService::getPlatform()) {
        platform->setProperty("broadtek-hci-bound", kOSBooleanTrue);
        platform->setProperty("broadtek-intel", intel ? kOSBooleanTrue : kOSBooleanFalse);
        platform->setProperty("broadtek-rtl-ready", g_rtl_ready ? kOSBooleanTrue : kOSBooleanFalse);
    }
    for (uint8_t address : addresses) {
        if (auto *pipe = interface->copyPipe(address)) {
            rememberPipe(interface, pipe);
            pipe->release();
        }
    }
    return true;
}

static void forgetIntelClient();

static bool onBluetoothTerminated(void *, void *, IOService *service, IONotifier *)
{
    IOUSBHostPipe *pipes[3];
    IOSimpleLockLock(g_lock);
    if (service != g_bt_interface) { IOSimpleLockUnlock(g_lock); return true; }
    auto *device = g_bt_device;
    g_controller_generation++;
    g_bt_interface = nullptr;
    g_bt_device = nullptr;
    g_intel = g_force_intel;
    g_rtl_ready = false;
    bt_adv_reset(&g_adv_state);
    bt_ctx_init(&g_ctx);
    memcpy(pipes, g_bt_pipes, sizeof(pipes));
    bzero(g_bt_pipes, sizeof(g_bt_pipes));
    IOSimpleLockUnlock(g_lock);
    forgetIntelClient();
    for (auto *pipe : pipes) if (pipe != nullptr) pipe->release();
    if (auto *platform = IOService::getPlatform()) {
        platform->setProperty("broadtek-hci-bound", kOSBooleanFalse);
        platform->setProperty("broadtek-rtl-ready", kOSBooleanFalse);
    }
    device->release();
    service->release();
    return true;
}

static BtPipe bluetoothPipeRole(IOUSBHostPipe *pipe, uint64_t &generation)
{
    if (pipe == nullptr) return BtPipe::Other;
    IOSimpleLockLock(g_lock);
    generation = g_controller_generation;
    BtPipe role = BtPipe::Other;
    for (unsigned i = 0; i < arrsize(g_bt_pipes); i++)
        if (g_bt_pipes[i] == pipe) { role = static_cast<BtPipe>(i); break; }
    IOSimpleLockUnlock(g_lock);
    return role;
}

static bool isBluetoothRequest(IOService *client, const StandardUSB::DeviceRequest &request)
{
    if (client == nullptr || !isHCICommand(request)) return false;
    // A user client can be attached to the interface or to its device.
    IORegistryEntry *entry = client;
    entry->retain();
    bool match = false;
    while (entry != nullptr) {
        IOSimpleLockLock(g_lock);
        match = (entry == g_bt_interface || entry == g_bt_device) &&
                (request.bmRequestType == 0x20 || request.wIndex == g_hci_interface_number);
        IOSimpleLockUnlock(g_lock);
        bool stop = match || entry->metaCast("IOUSBHostInterface") || entry->metaCast("IOUSBHostDevice");
        auto *parent = stop ? nullptr : entry->copyParentEntry(gIOServicePlane);
        entry->release();
        entry = parent;
    }
    return match;
}

/* ---- event completions ----------------------------------------------
 * The buffer is only filled when the transfer completes, so an event hook has
 * to sit in the completion. Each in-flight transfer borrows a slot; if they
 * all run out the transfer is left alone rather than queued or dropped. */

enum SlotKind { kSlotEvent, kSlotAcl, kSlotOwnedBuffer };

struct Slot {
    volatile SInt32 busy;
    IOUSBHostCompletion orig;
    IOMemoryDescriptor *md;
    uint64_t controllerGeneration;
    uint32_t requestLength;
    SlotKind kind;
    IOMemoryDescriptor *owned;      /* released when the transfer completes */
};

static constexpr int kSlots = 32;
static Slot g_slots[kSlots];

static void completionTrampoline(void *owner, void *parameter, IOReturn status, uint32_t bytes)
{
    Slot *s = static_cast<Slot *>(parameter);
    IOUSBHostCompletion orig = s->orig;
    IOMemoryDescriptor *md = s->md;
    uint32_t deliver = bytes;

    /* A command buffer BroadTek supplied: the transfer is done with it. */
    if (s->kind == kSlotOwnedBuffer) {
        IOMemoryDescriptor *owned = s->owned;
        s->owned = nullptr;
        OSDecrementAtomic(&s->busy);
        if (owned != nullptr) owned->release();
        if (orig.action != nullptr)
            orig.action(owner != nullptr ? owner : orig.owner, orig.parameter, status, bytes);
        return;
    }

    IOSimpleLockLock(g_lock);
    bool current = s->controllerGeneration == g_controller_generation;
    IOSimpleLockUnlock(g_lock);
    if (!current) {
        s->md = nullptr;
        OSDecrementAtomic(&s->busy);
        if (orig.action != nullptr) orig.action(orig.owner, orig.parameter, status, bytes);
        return;
    }

    /* ACL data is only ever read. Nothing is rewritten on this pipe, in any
     * stage: the point is to take the IRK out of a bonding exchange, and a
     * bonding exchange must not be disturbed. */
    if (s->kind == kSlotAcl) {
        if (status == kIOReturnSuccess && bytes >= 4 && md != nullptr) {
            uint8_t acl[kMaxACL];
            size_t len = readMD(md, acl, bytes < kMaxACL ? bytes : kMaxACL);
            g_acl_in++;
            if (g_n_acl_in != nullptr) g_n_acl_in->setValue(g_acl_in);
            if (len >= 4) {
                noteL2cap(1, acl, len);
                IOSimpleLockLock(g_lock);
                int got = bt_acl_scan(&g_ctx, acl, len);
                int done = bt_smp_complete(&g_ctx);
                uint32_t pdus = g_ctx.smp_pdus;
                uint32_t bad = g_ctx.smp_truncated + g_ctx.acl_continuations;
                IOSimpleLockUnlock(g_lock);

                if (g_n_smp_pdus != nullptr) g_n_smp_pdus->setValue(pdus);
                if (g_n_irks != nullptr) g_n_irks->setValue(static_cast<uint64_t>(done));
                if (g_n_smp_bad != nullptr) g_n_smp_bad->setValue(bad);
                if (got) {
                    g_irk_dirty = 1;
                    /* The key itself is never logged. */
                    SYSLOG("broadtek", "captured SMP identity material (%d complete)", done);
                }
            }
        }
        s->md = nullptr;
        OSDecrementAtomic(&s->busy);
        if (orig.action != nullptr)
            orig.action(owner != nullptr ? owner : orig.owner, orig.parameter, status, bytes);
        return;
    }

    if (status == kIOReturnSuccess && bytes >= 2 && md != nullptr) {
        uint8_t buf[kMaxHCI];
        size_t room = md->getLength();
        if (room > s->requestLength) room = s->requestLength;
        if (room > kMaxHCI) room = kMaxHCI;
        size_t len = readMD(md, buf, bytes < room ? bytes : room);
        if (len >= 2) {
            bt_verdict v = BT_FORWARD;
            size_t out = len;
            g_evts++;
            if (g_n_evts != nullptr) g_n_evts->setValue(g_evts);
            if (g_n_resolved != nullptr) g_n_resolved->setValue(g_ctx.resolved);
            if (g_n_conn_events != nullptr) g_n_conn_events->setValue(g_ctx.conn_events);
            if (g_n_conn_translated != nullptr)
                g_n_conn_translated->setValue(g_ctx.conn_translated);

            /* Does it even look like an HCI event? */
            if (bytes == len && static_cast<size_t>(buf[1]) + 2 == len) {
                g_evt_shaped++;
                if (g_n_evt_shaped != nullptr) g_n_evt_shaped->setValue(g_evt_shaped);
                noteEventCode(buf[0]);
                noteAdvEvent(buf, len);
                noteNearby(buf, len);

                /* LE (Enhanced) Connection Complete with status 0: a fresh
                 * link on this handle. Drop the handle's old trace so what
                 * follows is this connection alone. */
                if (buf[0] == 0x3e && len >= 6 && (buf[2] == 0x01 || buf[2] == 0x0a) &&
                    buf[3] == 0x00)
                    clearL2capHandle((uint16_t)((buf[4] | (buf[5] << 8)) & 0x0fff));

                /* Consume only a response to an outstanding injected command. */
                bool cc = buf[0] == 0x0e && len >= 6;
                bool refused = buf[0] == 0x0f && len >= 6 && buf[2] != 0;
                if (g_adv_addr && (cc || refused)) {
                    size_t pos = cc ? 3 : 4;
                    uint16_t opcode = static_cast<uint16_t>(buf[pos] | (buf[pos + 1] << 8));
                    uint8_t result = cc ? buf[5] : buf[2];
                    IOSimpleLockLock(g_lock);
                    if (result && isAdvOpcode(opcode)) bt_adv_invalidate(&g_adv_state);
                    bool ours = opcode == 0x2035 && bt_adv_complete(&g_adv_state, result);
                    IOSimpleLockUnlock(g_lock);
                    if (ours) {
                        g_adv_inj_status = result;
                        g_adv_inj_cc++;
                        if (g_n_adv_inj_cc != nullptr) g_n_adv_inj_cc->setValue(g_adv_inj_cc);
                        if (g_n_adv_inj_status != nullptr) g_n_adv_inj_status->setValue(result);
                        uint8_t credits = cc ? buf[2] : buf[3];
                        const uint8_t nop[] = { 0x0e, 3, credits, 0, 0 };
                        deliver = writeEvent(md, nop, sizeof(nop), status);
                        s->md = nullptr;
                        OSDecrementAtomic(&s->busy);
                        if (orig.action != nullptr)
                            orig.action(owner != nullptr ? owner : orig.owner, orig.parameter, status, deliver);
                        return;
                    }
                }

                /* Intel: link-ready reports, and the replies to our own
                 * commands that become them. */
                if (g_intel || g_rtl_ready) {
                    size_t nl = intelOnEvent(buf, len, room);
                    if (nl != 0) {
                        deliver = writeEvent(md, buf, nl, status);
                        s->md = nullptr;
                        OSDecrementAtomic(&s->busy);
                        if (orig.action != nullptr)
                            orig.action(owner != nullptr ? owner : orig.owner, orig.parameter,
                                        status, deliver);
                        return;
                    }
                }

                /* Our own 0x2005's reply (Intel). bluetoothd's command that
                 * triggered it is held until this arrives, so while one is
                 * pending the next 0x2005 reply is ours. */
                if (g_intel_inj_pending > 0 && buf[0] == 0x0e && len >= 6 &&
                    buf[3] == 0x05 && buf[4] == 0x20) {
                    OSDecrementAtomic(&g_intel_inj_pending);
                    buf[1] = 0x03; buf[3] = 0x00; buf[4] = 0x00;   /* keep num_packets */
                    deliver = writeEvent(md, buf, 5, status);
                    s->md = nullptr;
                    OSDecrementAtomic(&s->busy);
                    if (orig.action != nullptr)
                        orig.action(owner != nullptr ? owner : orig.owner, orig.parameter,
                                    status, deliver);
                    return;
                }

                /* Command Complete: num_packets, opcode, status, ...
                 * Command Status:  status, num_packets, opcode, ... */
                if (buf[0] == 0x0e && len >= 5)
                    noteOpcodeQuiet(g_ops_cc, static_cast<uint16_t>(buf[3] | (buf[4] << 8)),
                                    &g_ops_cc_dirty);
                else if (buf[0] == 0x0f && len >= 6)
                    noteOpcodeQuiet(g_ops_cc, static_cast<uint16_t>(buf[4] | (buf[5] << 8)),
                                    &g_ops_cc_dirty);
            } else {
                g_evt_other++;
                if (g_n_evt_other != nullptr) g_n_evt_other->setValue(g_evt_other);
            }
            if (g_rw_events && bytes == len && static_cast<size_t>(buf[1]) + 2 == len) {
                uint8_t original[kMaxHCI];
                memcpy(original, buf, len);
                IOSimpleLockLock(g_lock);
                v = bt_hci_on_event(&g_ctx, buf, len, room, &out);
                IOSimpleLockUnlock(g_lock);

                /* A transfer cannot be un-sent, so an event the host must not
                 * see becomes a Command Complete for the NOP opcode: the
                 * credit update a controller sends when it has nothing to
                 * report. */
                if (v == BT_DROP) {
                    buf[0] = 0x0e; buf[1] = 0x03; buf[2] = 0x01;
                    buf[3] = 0x00; buf[4] = 0x00;
                    out = 5;
                }
                if (out != len || memcmp(original, buf, len) != 0)
                    deliver = writeEvent(md, buf, out, status);
            }
            OBSLOG(g_logged_evt, "event %02x len %zu%s", buf[0], len,
                   v == BT_DROP ? " (held back)" : "");
        }
    }

    s->md = nullptr;
    OSDecrementAtomic(&s->busy);

    if (orig.action != nullptr)
        orig.action(owner != nullptr ? owner : orig.owner, orig.parameter, status, deliver);
}

static IOUSBHostCompletion *borrowSlot(IOUSBHostCompletion *completion, IOMemoryDescriptor *md,
                                       IOUSBHostCompletion *out, SlotKind kind, uint32_t requestLength,
                                       uint64_t generation)
{
    for (int i = 0; i < kSlots; i++) {
        if (OSCompareAndSwap(0, 1, reinterpret_cast<volatile UInt32 *>(&g_slots[i].busy))) {
            g_slots[i].orig = *completion;
            g_slots[i].md = md;
            g_slots[i].controllerGeneration = generation;
            g_slots[i].requestLength = requestLength;
            g_slots[i].kind = kind;
            g_slots[i].owned = nullptr;
            out->owner = completion->owner;
            out->action = completionTrampoline;
            out->parameter = &g_slots[i];
            return out;
        }
    }
    return completion;
}

/* Read an HCI command out of a control request, or say why not. Returns 0
 * unless the whole packet is in hand. */
static size_t readCommand(StandardUSB::DeviceRequest &request, IOMemoryDescriptor *md,
                          uint8_t *buf, size_t cap)
{
    if (!isHCICommand(request)) return 0;

    if (md == nullptr) {
        g_cmd_nobuf++;
        if (g_n_cmd_nobuf != nullptr) g_n_cmd_nobuf->setValue(g_cmd_nobuf);
        return 0;
    }

    if (request.wLength < cap) cap = request.wLength;
    size_t len = readMD(md, buf, cap);
    if (len < 3 || len != 3u + buf[2]) {
        g_cmd_short++;
        if (g_n_cmd_short != nullptr) g_n_cmd_short->setValue(g_cmd_short);
        return 0;
    }
    return len;
}

/* An edited command cannot be written back into the caller's buffer.
 *
 * A command is an OUT transfer, and prepare() marks an outbound descriptor
 * kIOMemoryPreparedReadOnly, after which writeBytes refuses by design. That
 * is why broadtek-write-short counted 510 while the event rewrites worked
 * perfectly: an event descriptor is inbound and writable, a command's is not.
 *
 * So stop trying to edit theirs. The hook owns the arguments it forwards, so
 * it can forward a descriptor of its own holding the edited bytes. Nothing is
 * injected -- this is still the command the host sent, going where it was
 * going, with an address changed, which is what §5a's design asks for.
 *
 * The caller's buffer is left untouched, which is correct for an OUT
 * transfer: nothing reads it back.
 *
 * Lifetime is the catch. A synchronous call can release on return, but an
 * asynchronous one must keep the buffer alive until the transfer completes,
 * so it borrows a completion slot and the trampoline releases it. If no slot
 * is free the substitution is abandoned and the original buffer goes through
 * unedited -- a command that does not get rewritten is a failure to improve
 * things, while a buffer freed underneath a live transfer is a panic. */
static uint64_t g_cmd_rebuffered;
static uint64_t g_cmd_rebuffer_failed;
static OSNumber *g_n_cmd_rebuffered;
static OSNumber *g_n_cmd_rebuffer_failed;

static IOMemoryDescriptor *ownedCopy(const uint8_t *buf, size_t len)
{
    IOBufferMemoryDescriptor *b =
        IOBufferMemoryDescriptor::withBytes(buf, len, kIODirectionOut);
    if (b == nullptr) {
        g_cmd_rebuffer_failed++;
        if (g_n_cmd_rebuffer_failed != nullptr)
            g_n_cmd_rebuffer_failed->setValue(g_cmd_rebuffer_failed);
    }
    return b;
}

static void countRebuffer(bool ok)
{
    if (ok) {
        g_cmd_rebuffered++;
        if (g_n_cmd_rebuffered != nullptr) g_n_cmd_rebuffered->setValue(g_cmd_rebuffered);
    } else {
        g_cmd_rebuffer_failed++;
        if (g_n_cmd_rebuffer_failed != nullptr)
            g_n_cmd_rebuffer_failed->setValue(g_cmd_rebuffer_failed);
    }
}

/* Attach an owned buffer to a wrapped completion so it outlives the transfer.
 * Returns the completion to pass on, and sets *use to the buffer or leaves it
 * alone if no slot was free. */
static IOUSBHostCompletion *holdUntilDone(IOUSBHostCompletion *completion,
                                          IOUSBHostCompletion *out,
                                          IOMemoryDescriptor *owned)
{
    for (int i = 0; i < kSlots; i++) {
        if (OSCompareAndSwap(0, 1, reinterpret_cast<volatile UInt32 *>(&g_slots[i].busy))) {
            g_slots[i].orig = *completion;
            g_slots[i].md = nullptr;
            g_slots[i].kind = kSlotOwnedBuffer;
            g_slots[i].owned = owned;
            out->owner = completion->owner;
            out->action = completionTrampoline;
            out->parameter = &g_slots[i];
            return out;
        }
    }
    return nullptr;                 /* no slot: caller must not substitute */
}

/* ---- hooks ----------------------------------------------------------- */

static IOReturn onUserClientControlRequest(void *that, uint8_t pipeRef,
                                           StandardUSB::DeviceRequest &request,
                                           IOMemoryDescriptor *dataBuffer,
                                           IOUSBHostCompletion *completion,
                                           uint32_t &bytesTransferred, uint32_t timeout);

static IOReturn onControlRequestAsync(IOUSBHostPipe *that, IOService *forClient,
                                      StandardUSB::DeviceRequest &request,
                                      IOMemoryDescriptor *dataBuffer,
                                      IOUSBHostCompletion *completion, uint32_t timeout)
{
    noteControl(request);
    {
        uint8_t buf[kMaxHCI];
        size_t len = isBluetoothRequest(forClient, request)
            ? readCommand(request, dataBuffer, buf, kMaxHCI) : 0;
        if (len >= 3) {
            uint16_t opcode = static_cast<uint16_t>(buf[0] | (buf[1] << 8));
            g_cmds++; g_cmds_pipe++;
            if (g_n_cmds != nullptr) g_n_cmds->setValue(g_cmds);
            if (g_n_cmds_pipe != nullptr) g_n_cmds_pipe->setValue(g_cmds_pipe);
            noteOpcode(g_ops_pipe, opcode);       /* caller context: may allocate */
            noteConnTarget(buf, len);
            if (g_n_substituted != nullptr) g_n_substituted->setValue(g_ctx.substituted);
            if (g_rw_cmds) {
                /* The command keeps its length; only an address inside it can
                 * change, so it is written straight back. */
                IOSimpleLockLock(g_lock);
                bt_hci_on_command(&g_ctx, buf, len);
                IOSimpleLockUnlock(g_lock);
                writeMD(dataBuffer, buf, len);
            }
            OBSLOG(g_logged_cmd, "command %04x len %zu", opcode, len);
        }
    }
    return FunctionCast(onControlRequestAsync, g_orig_ctrl_async)(that, forClient, request,
                                                                 dataBuffer, completion, timeout);
}

static IOReturn onControlRequestSync(IOUSBHostPipe *that, IOService *forClient,
                                     StandardUSB::DeviceRequest &request,
                                     IOMemoryDescriptor *dataBuffer,
                                     uint32_t &bytesTransferred, uint32_t timeout)
{
    noteControl(request);
    {
        uint8_t buf[kMaxHCI];
        size_t len = isBluetoothRequest(forClient, request)
            ? readCommand(request, dataBuffer, buf, kMaxHCI) : 0;
        if (len >= 3) {
            uint16_t opcode = static_cast<uint16_t>(buf[0] | (buf[1] << 8));
            g_cmds++; g_cmds_pipe++;
            if (g_n_cmds != nullptr) g_n_cmds->setValue(g_cmds);
            if (g_n_cmds_pipe != nullptr) g_n_cmds_pipe->setValue(g_cmds_pipe);
            noteOpcode(g_ops_pipe, opcode);
            noteConnTarget(buf, len);
            OBSLOG(g_logged_cmd, "command (sync) %04x len %zu",
                   static_cast<uint16_t>(buf[0] | (buf[1] << 8)), len);
        }
    }
    return FunctionCast(onControlRequestSync, g_orig_ctrl_sync)(that, forClient, request,
                                                               dataBuffer, bytesTransferred, timeout);
}

/* ---- L2CAP trace (read-only) --------------------------------------------
 * iPhone Mirroring's pre-pairing connects to the iPhone, the link encrypts,
 * then nothing happens for ten seconds and it drops: CoreBluetooth never tells
 * rapportd the peripheral connected. The logs do not show the ATT/L2CAP PDUs,
 * so this does: per connection handle and direction, the L2CAP channels that
 * carry traffic and the first PDU byte on each. That says whether the Mac ever
 * sends ATT or an L2CAP CoC request to the phone, and whether the phone answers
 * -- which tells a Mac-side stall from the phone ignoring an unbonded central.
 *
 * Addresses are not involved; this is channel ids and opcodes. Read-only. */
static constexpr int kL2capRecs = 48;
struct L2capRec {
    uint16_t handle;        /* 12-bit ACL connection handle */
    uint8_t  dir;           /* 0 host->controller (OUT), 1 controller->host (IN) */
    uint16_t cid;           /* L2CAP channel */
    uint8_t  firstop;       /* first PDU byte: ATT opcode, SMP/signalling code */
    uint32_t count;
};
static L2capRec g_l2cap[kL2capRecs];
static int g_n_l2cap;
static volatile SInt32 g_l2cap_busy;

/* dir: 0 OUT, 1 IN. No allocation, no locks: safe in either context. */
static void noteL2cap(int dir, const uint8_t *acl, size_t len)
{
    if (len < 8) return;
    uint16_t header = (uint16_t)(acl[0] | (acl[1] << 8));
    uint16_t handle = (uint16_t)(header & 0x0fff);
    uint8_t  pb = (uint8_t)((header >> 12) & 0x03);
    if (pb == 0x01) return;                  /* a continuation has no L2CAP header */

    uint16_t cid = (uint16_t)(acl[6] | (acl[7] << 8));
    uint8_t  firstop = len >= 9 ? acl[8] : 0xff;

    for (int i = 0; i < g_n_l2cap; i++) {
        L2capRec *r = &g_l2cap[i];
        if (r->handle == handle && r->dir == (uint8_t)dir && r->cid == cid &&
            r->firstop == firstop) {
            r->count++;
            g_l2cap_dirty = 1;
            return;
        }
    }
    if (g_n_l2cap >= kL2capRecs) { tableFull(); return; }
    L2capRec *r = &g_l2cap[g_n_l2cap++];
    r->handle = handle;
    r->dir = (uint8_t)dir;
    r->cid = cid;
    r->firstop = firstop;
    r->count = 1;
    g_l2cap_dirty = 1;
}

/* A connection handle is reused: a new link on it is a different device, so its
 * old trace must go. Called on each LE Connection Complete, which is what makes
 * one handle's records belong to exactly one connection -- the only way to tell
 * the iPhone's own ATT from a background GATT session sharing the handle. */
static void clearL2capHandle(uint16_t handle)
{
    int w = 0;
    for (int i = 0; i < g_n_l2cap; i++)
        if (g_l2cap[i].handle != handle) g_l2cap[w++] = g_l2cap[i];
    if (w != g_n_l2cap) { g_n_l2cap = w; g_l2cap_dirty = 1; }
}

static void publishL2cap()
{
    if (!OSCompareAndSwap(0, 1, reinterpret_cast<volatile UInt32 *>(&g_l2cap_busy))) {
        g_l2cap_dirty = 1;
        return;
    }
    IORegistryEntry *platform = IOService::getPlatform();
    if (platform == nullptr) { g_l2cap_busy = 0; return; }

    static char line[1600];
    size_t at = 0;
    for (int i = 0; i < g_n_l2cap && at + 40 < sizeof(line); i++) {
        const L2capRec &r = g_l2cap[i];
        at += (size_t)snprintf(line + at, sizeof(line) - at, "%sh%03x/%s/c%04x/op%02x#%u",
                               at > 0 ? " " : "", r.handle, r.dir ? "in" : "out",
                               r.cid, r.firstop, r.count);
    }
    line[at < sizeof(line) ? at : sizeof(line) - 1] = '\0';

    static const OSSymbol *key;
    if (key == nullptr) key = OSSymbol::withCStringNoCopy("broadtek-l2cap");
    if (key != nullptr) {
        if (OSString *v = OSString::withCString(line)) { platform->setProperty(key, v); v->release(); }
    }
    OSCompareAndSwap(1, 0, reinterpret_cast<volatile UInt32 *>(&g_l2cap_busy));
}

static IOReturn onIOAsync(IOUSBHostPipe *that, IOMemoryDescriptor *dataBuffer,
                          uint32_t dataBufferLength, IOUSBHostCompletion *completion,
                          uint32_t timeout)
{
    IOUSBHostCompletion wrapper;
    IOUSBHostCompletion *use = completion;

    /* Counted before the endpoint test: "io is never called" and "io is called
     * but never for the event pipe" are different faults, and the registry has
     * to be able to tell them apart. */
    g_io_calls++;
    if (g_n_io_calls != nullptr) g_n_io_calls->setValue(g_io_calls);
    uint64_t generation = 0;
    BtPipe role = bluetoothPipeRole(that, generation);
    if (role != BtPipe::Other) noteEndpoint(that->getEndpointDescriptor());

    if (completion != nullptr && role == BtPipe::Event)
        use = borrowSlot(completion, dataBuffer, &wrapper, kSlotEvent, dataBufferLength, generation);
    else if (completion != nullptr && role == BtPipe::AclIn)
        use = borrowSlot(completion, dataBuffer, &wrapper, kSlotAcl, dataBufferLength, generation);
    else if (role == BtPipe::AclOut && dataBuffer != nullptr) {
        /* OUT buffer: filled before the transfer, so it reads here. */
        uint8_t acl[16];
        size_t n = readMD(dataBuffer, acl, sizeof(acl));
        if (n >= 8) noteL2cap(0, acl, n);
    }
    else if (that != nullptr && endpointRole(that->getEndpointDescriptor()) != BtPipe::Other) {
        g_pipes_elsewhere++;
        if (g_n_pipes_elsewhere != nullptr) g_n_pipes_elsewhere->setValue(g_pipes_elsewhere);
    }

    IOReturn result = FunctionCast(onIOAsync, g_orig_io_async)(that, dataBuffer, dataBufferLength,
                                                              use, timeout);
    if (result != kIOReturnSuccess && use == &wrapper) {
        auto *slot = static_cast<Slot *>(wrapper.parameter);
        slot->md = nullptr;
        OSDecrementAtomic(&slot->busy);
    }
    return result;
}

/* Caller context, bluetoothd's own thread: it may block. */
static void advSendSetAddress(void *that, uint8_t pipeRef, const StandardUSB::DeviceRequest &like,
                              uint8_t handle, uint32_t timeout)
{
    uint8_t cmd[10] = { 0x35, 0x20, 7, handle };
    IOSimpleLockLock(g_lock);
    uint64_t ticket = bt_adv_begin(&g_adv_state, handle, g_adv_cache, &cmd[4]);
    IOSimpleLockUnlock(g_lock);
    if (!ticket) return;

    StandardUSB::DeviceRequest req = like;
    req.wLength = sizeof(cmd);
    uint32_t sent = 0;
    IOReturn r = kIOReturnNoMemory;
    if (auto *md = ownedCopy(cmd, sizeof(cmd))) {
        r = FunctionCast(onUserClientControlRequest, g_orig_uc_ctrl)(that, pipeRef, req, md,
                                                                   nullptr, sent, timeout);
        md->release();
    }
    if (r != kIOReturnSuccess || sent != sizeof(cmd)) {
        IOSimpleLockLock(g_lock);
        if (g_adv_state.pending == ticket) g_adv_state.pending = 0;
        IOSimpleLockUnlock(g_lock);
        g_adv_inj_failed++;
        if (g_n_adv_inj_failed != nullptr) g_n_adv_inj_failed->setValue(g_adv_inj_failed);
        return;
    }
    g_adv_injected++;
    if (g_n_adv_injected != nullptr) g_n_adv_injected->setValue(g_adv_injected);

    /* One command at a time, as the controller's credit asks: wait for the
     * reply before bluetoothd's enable goes out. A reply that comes late is
     * still turned into a NOP when it arrives. */
    for (int i = 0; i <= 100; i++) {
        IOSimpleLockLock(g_lock);
        bool waiting = g_adv_state.pending == ticket;
        if (waiting && i == 100) bt_adv_invalidate(&g_adv_state);
        IOSimpleLockUnlock(g_lock);
        if (!waiting) return;
        if (i < 100) IOSleep(1);
    }
    g_adv_inj_timeouts++;
    if (g_n_adv_inj_timeouts != nullptr) g_n_adv_inj_timeouts->setValue(g_adv_inj_timeouts);
}

/* Caller context. Track 0x2005 and 0x2036; before an enable, address the sets. */
static void advBeforeCommand(void *that, uint8_t pipeRef, const StandardUSB::DeviceRequest &request,
                             const uint8_t *buf, size_t len, uint32_t timeout)
{
    if (!g_adv_addr || len < 3) return;
    uint16_t opcode = static_cast<uint16_t>(buf[0] | (buf[1] << 8));
    uint8_t plen = buf[2];
    const uint8_t *p = &buf[3];
    if (len < static_cast<size_t>(3) + plen) return;

    if (opcode != 0x2039) {
        IOSimpleLockLock(g_lock);
        bt_adv_command(&g_adv_state, opcode, p, plen);
        IOSimpleLockUnlock(g_lock);
        return;
    }
    if (plen < 2 || p[0] != 1) return;
    IOLockLock(g_adv_send_lock);
    for (unsigned i = 0; i < p[1] && 6u + 4u * i <= plen; i++)
        advSendSetAddress(that, pipeRef, request, p[2 + 4 * i], timeout);
    IOLockUnlock(g_adv_send_lock);
}

/*
 * Intel, on the HCI stream. Two things a spec-strict controller needs that
 * bluetoothd does not give an unknown chipset:
 *
 *  - LE Set Event Mask (0x2001) without bit 12, LE Extended Advertising
 *    Report. Once scanning is extended (above) that is the only report the
 *    controller sends, and masked off it sends none: nothing nearby is seen.
 *  - A scan with a random own address before any random address is set.
 *    bluetoothd sets one only for advertising, so a legacy LE Set Scan
 *    Parameters (0x200b) asking for a random address after a reset fails.
 *    Before the first such command a static random address (the two top bits
 *    set) goes out in LE Set Random Address (0x2005). bluetoothd's own 0x2005,
 *    when it sends one, replaces it.
 */
static uint8_t g_intel_static[6];
static bool g_intel_have_random;
static uint64_t g_intel_masked, g_intel_rand_sent;
static OSNumber *g_n_intel_masked, *g_n_intel_rand_sent;

static void intelMakeStaticAddress()
{
    /* Per boot, not secret: the address only has to be a valid static one. */
    uint64_t t = mach_absolute_time();
    for (int i = 0; i < 6; i++) {
        t = t * 6364136223846793005ull + 1442695040888963407ull;
        g_intel_static[i] = static_cast<uint8_t>(t >> 56);
    }
    g_intel_static[5] |= 0xc0;
}

/* Caller context, bluetoothd's thread: may block, like advSendSetAddress. */
static void intelSendRandomAddress(void *that, uint8_t pipeRef, const StandardUSB::DeviceRequest &like,
                                   uint32_t timeout)
{
    uint8_t cmd[9] = { 0x05, 0x20, 6 };
    memcpy(&cmd[3], g_intel_static, 6);
    IOMemoryDescriptor *md = ownedCopy(cmd, sizeof(cmd));
    if (md == nullptr) return;

    StandardUSB::DeviceRequest req = like;
    req.wLength = sizeof(cmd);
    uint32_t sent = 0;
    OSIncrementAtomic(&g_intel_inj_pending);
    IOReturn r = FunctionCast(onUserClientControlRequest, g_orig_uc_ctrl)(that, pipeRef, req, md,
                                                                          nullptr, sent, timeout);
    md->release();
    if (r != kIOReturnSuccess) { OSDecrementAtomic(&g_intel_inj_pending); return; }
    g_intel_have_random = true;
    g_intel_rand_sent++;
    if (g_n_intel_rand_sent != nullptr) g_n_intel_rand_sent->setValue(g_intel_rand_sent);
    for (int i = 0; i < 100 && g_intel_inj_pending > 0; i++) IOSleep(1);
}

/* Caller context. Returns true if it changed buf. */
static bool intelBeforeCommand(void *that, uint8_t pipeRef, const StandardUSB::DeviceRequest &request,
                               uint8_t *buf, size_t len, uint32_t timeout)
{
    if (!g_intel || len < 3) return false;
    uint16_t opcode = static_cast<uint16_t>(buf[0] | (buf[1] << 8));
    uint8_t plen = buf[2];
    uint8_t *p = &buf[3];
    if (len < static_cast<size_t>(3) + plen) return false;

    switch (opcode) {
    case 0x0c03:                                    /* HCI Reset */
        g_intel_have_random = false;
        return false;
    case 0x2005:
        g_intel_have_random = true;
        return false;
    case 0x2001:
        /* bit 12 extended advertising report; bits 6 and 11, Data Length
         * Change and PHY Update Complete, are the link-ready reports. */
        if (plen < 8 || ((p[1] & 0x18) == 0x18 && (p[0] & 0x40))) return false;
        p[0] |= 0x40;
        p[1] |= 0x18;
        g_intel_masked++;
        if (g_n_intel_masked != nullptr) g_n_intel_masked->setValue(g_intel_masked);
        return true;
    case 0x200b:
        /* type, interval, window, own address type, filter policy */
        if (plen >= 7 && (p[5] == 0x01 || p[5] == 0x03) && !g_intel_have_random)
            intelSendRandomAddress(that, pipeRef, request, timeout);
        return false;
    case 0x2041:
        /* LE Set Extended Scan Parameters: own address type first */
        if (plen >= 1 && (p[0] == 0x01 || p[0] == 0x03) && !g_intel_have_random)
            intelSendRandomAddress(that, pipeRef, request, timeout);
        return false;
    default:
        return false;
    }
}

/*
 * Intel: the link-ready reports.
 *
 * After an LE connection, bluetoothd holds the link back from every app until
 * the controller has reported the link's PHY (LE PHY Update Complete) and its
 * packet sizes (LE Data Length Change) -- "ready: wait-for-features ... EDL
 * [remote 1, local 1], 2M [remote 1, local 1] PHYReady:no" in its log. It
 * never asks for either; Apple's own chips start both procedures themselves
 * and report them. The AX210 reports neither, so the link sits in
 * "connecting" for the apps: Watch unlock's WirelessProximity connection
 * times out after 10 s on a link that is up and encrypted, and the Watch hangs
 * up.
 *
 * So, once the remote features are in (LE Read Remote Features Complete),
 * BroadTek waits a moment for real reports and, for each one still missing,
 * asks the controller itself and hands the answer to bluetoothd in the form it
 * waits for:
 *   - LE Read PHY (0x2030): its reply becomes LE PHY Update Complete carrying
 *     the link's real PHYs;
 *   - LE Set Data Length (0x2022, 251 octets): the controller negotiates for
 *     real, and its reply becomes LE Data Length Change with the minimum
 *     27 octets / 328 us, which holds on every link; a real Data Length
 *     Change, when the controller sends one, follows and corrects it.
 * The event mask also gains both reports (0x2001 above), so a controller that
 * does send them is passed through untouched and nothing is made up.
 */
enum { kOwnNop = 1, kOwnToPhy = 2, kOwnToDle = 3, kRtlSetPhy, kRtlSetDle };
struct OwnReply { volatile uint8_t used; uint8_t action; uint16_t opcode; uint16_t handle; };
static OwnReply g_own[4];
struct LinkReady {
    uint8_t used, phy, dle, queued;
    uint16_t handle;
    unsigned procedures, requested;
    uint64_t serial;
};
struct ReadyJob { uint16_t handle; uint64_t serial; };
static uint64_t g_link_serial;
static LinkReady g_links[8];
static ReadyJob g_ready_queue[8];
static int g_ready_queued;
static IOSimpleLock *g_intel_lock;
static thread_call_t g_ready_call;
static OSObject *g_uc_client;
static uint8_t g_uc_pipe;
static StandardUSB::DeviceRequest g_uc_req;
static uint32_t g_uc_timeout;
static volatile SInt32 g_own_busy;
static uint64_t g_ready_phy, g_ready_dle, g_ready_real;
static OSNumber *g_n_ready_phy, *g_n_ready_dle, *g_n_ready_real;

static void forgetIntelClient()
{
    if (!g_intel_lock) return;
    IOSimpleLockLock(g_intel_lock);
    auto *old = g_uc_client;
    g_uc_client = nullptr;
    g_ready_queued = 0;
    bzero(g_links, sizeof(g_links));
    g_intel_have_random = false;
    IOSimpleLockUnlock(g_intel_lock);
    if (old) old->release();
}

/* Caller context (bluetoothd's thread). Keeps the client our own commands go
 * through, and holds bluetoothd's command back while one of ours is out, so
 * the controller never has two at once. */
static void intelNoteClient(void *that, uint8_t pipeRef, const StandardUSB::DeviceRequest &request,
                            uint32_t timeout)
{
    for (int i = 0; i < 300 && g_own_busy > 0; i++) IOSleep(1);
    if (g_intel_lock == nullptr) return;
    OSObject *nu = static_cast<OSObject *>(that), *old = nullptr;
    IOSimpleLockLock(g_intel_lock);
    if (g_uc_client != nu) {
        if (nu != nullptr) nu->retain();
        old = g_uc_client;
        g_uc_client = nu;
    }
    g_uc_pipe = pipeRef;
    g_uc_req = request;
    g_uc_timeout = timeout;
    IOSimpleLockUnlock(g_intel_lock);
    if (old != nullptr) old->release();
}

static LinkReady *linkFor(uint16_t h, bool make)
{
    for (auto &l : g_links) if (l.used && l.handle == h) return &l;
    if (!make) return nullptr;
    for (auto &l : g_links) if (!l.used) {
        l = LinkReady{1, 0, 0, 0, h, 0, 0, ++g_link_serial};
        return &l;
    }
    return nullptr;
}

/* Called with g_intel_lock held. A recycled HCI handle has a new serial. */
static bool queueReady(LinkReady *link)
{
    if (!link || !link->procedures || link->queued || g_ready_queued >= 8 || !g_ready_call)
        return false;
    link->queued = 1;
    g_ready_queue[g_ready_queued++] = ReadyJob{link->handle, link->serial};
    return true;
}

/* Thread context. One command of ours, out and answered (or given up on). */
static void intelSendOwn(const uint8_t *cmd, size_t n, uint8_t action, ReadyJob job)
{
    if (n < 3) return;
    IOSimpleLockLock(g_intel_lock);
    LinkReady *link = linkFor(job.handle, false);
    OSObject *client = link && link->serial == job.serial ? g_uc_client : nullptr;
    if (client != nullptr) client->retain();
    uint8_t pipeRef = g_uc_pipe;
    StandardUSB::DeviceRequest req = g_uc_req;
    uint32_t timeout = g_uc_timeout;
    IOSimpleLockUnlock(g_intel_lock);
    if (client == nullptr) return;
    if (!isBluetoothRequest(static_cast<IOService *>(client), req)) { client->release(); return; }

    OwnReply *slot = nullptr;
    for (auto &o : g_own) if (!o.used) { slot = &o; break; }
    IOMemoryDescriptor *md = slot != nullptr ? ownedCopy(cmd, n) : nullptr;
    if (md == nullptr) { client->release(); return; }

    slot->action = action;
    slot->opcode = static_cast<uint16_t>(cmd[0] | (cmd[1] << 8));
    slot->handle = job.handle;
    OSSynchronizeIO();
    slot->used = 1;
    OSIncrementAtomic(&g_own_busy);

    req.wLength = static_cast<uint16_t>(n);
    uint32_t sent = 0;
    IOReturn r = FunctionCast(onUserClientControlRequest, g_orig_uc_ctrl)(client, pipeRef, req, md,
                                                                          nullptr, sent, timeout);
    md->release();
    if (r != kIOReturnSuccess) slot->used = 0;
    for (int i = 0; i < 300 && slot->used; i++) IOSleep(1);
    slot->used = 0;                 /* a late reply passes as it is */
    OSDecrementAtomic(&g_own_busy);
    client->release();
}

static void intelReadyWork(thread_call_param_t, thread_call_param_t)
{
    IOSleep(200);                   /* time for a controller to report by itself */
    for (;;) {
        IOSimpleLockLock(g_intel_lock);
        if (g_ready_queued == 0) { IOSimpleLockUnlock(g_intel_lock); return; }
        ReadyJob job = g_ready_queue[--g_ready_queued];
        uint16_t h = job.handle;
        LinkReady *l = linkFor(h, false);
        if (l && l->serial != job.serial) l = nullptr;
        const bool rtl = g_rtl_ready;
        bool needPhy = l && !l->phy && (l->procedures & BT_LINK_PHY);
        bool needDle = l && !l->dle && (l->procedures & BT_LINK_DLE);
        if (l) {
            l->queued = 0;
            if (rtl) {
                needPhy = needPhy && !(l->requested & BT_LINK_PHY);
                // Wait for the controller's PHY event before starting a second
                // link-layer control procedure on the same connection.
                needDle = needDle && !(l->requested & BT_LINK_DLE) &&
                          (!(l->procedures & BT_LINK_PHY) || l->phy);
                if (needPhy) l->requested |= BT_LINK_PHY;
                if (needDle) l->requested |= BT_LINK_DLE;
            }
        }
        IOSimpleLockUnlock(g_intel_lock);

        if (rtl) {
            // Request real controller events for this link. Leave the PHY
            // choice to the controller and pass its actual values to macOS.
            uint8_t cmd[10];
            if (needPhy) {
                size_t n = bt_link_request(BT_LINK_PHY, h, cmd);
                intelSendOwn(cmd, n, kRtlSetPhy, job);
            }
            if (needDle) {
                size_t n = bt_link_request(BT_LINK_DLE, h, cmd);
                intelSendOwn(cmd, n, kRtlSetDle, job);
            }
            continue;
        }
        if (needPhy) {
            uint8_t cmd[5] = { 0x30, 0x20, 2, static_cast<uint8_t>(h), static_cast<uint8_t>(h >> 8) };
            intelSendOwn(cmd, sizeof(cmd), kOwnToPhy, job);
        }
        if (needDle) {
            uint8_t cmd[9] = { 0x22, 0x20, 6, static_cast<uint8_t>(h), static_cast<uint8_t>(h >> 8),
                               0xfb, 0x00, 0x48, 0x08 };            /* 251 octets, 2120 us */
            intelSendOwn(cmd, sizeof(cmd), kOwnToDle, job);
        }
    }
}

static size_t intelOnEvent(uint8_t *buf, size_t len, size_t room)
{
    /* The replies to our own commands. */
    if ((buf[0] == 0x0e && len >= 6) || (buf[0] == 0x0f && len >= 6)) {
        bool cc = buf[0] == 0x0e;
        uint16_t op = cc ? static_cast<uint16_t>(buf[3] | (buf[4] << 8))
                         : static_cast<uint16_t>(buf[4] | (buf[5] << 8));
        uint8_t credits = cc ? buf[2] : buf[3];
        uint8_t st = cc ? buf[5] : buf[2];
        for (auto &o : g_own) {
            if (!o.used || o.opcode != op) continue;
            uint16_t h = o.handle;
            uint8_t action = o.action;
            o.used = 0;
            if (action == kRtlSetPhy && g_rtl_phy_status) g_rtl_phy_status->setValue(st);
            if (action == kRtlSetDle && g_rtl_dle_status) g_rtl_dle_status->setValue(st);
            if (action == kOwnToPhy && cc && st == 0 && len >= 10 &&
                (buf[6] | (buf[7] << 8)) == h && room >= 8) {
                /* Read PHY: status, handle(2), tx, rx */
                uint8_t tx = buf[8], rx = buf[9];
                const uint8_t ev[8] = { 0x3e, 6, 0x0c, 0x00, static_cast<uint8_t>(h),
                                        static_cast<uint8_t>(h >> 8), tx, rx };
                memcpy(buf, ev, sizeof(ev));
                g_ready_phy++;
                if (g_n_ready_phy != nullptr) g_n_ready_phy->setValue(g_ready_phy);
                return sizeof(ev);
            }
            if (action == kOwnToDle && cc && st == 0 && len >= 8 &&
                (buf[6] | (buf[7] << 8)) == h && room >= 13) {
                const uint8_t ev[13] = { 0x3e, 11, 0x07, static_cast<uint8_t>(h), static_cast<uint8_t>(h >> 8),
                                         27, 0, 0x48, 0x01, 27, 0, 0x48, 0x01 };
                memcpy(buf, ev, sizeof(ev));
                g_ready_dle++;
                if (g_n_ready_dle != nullptr) g_n_ready_dle->setValue(g_ready_dle);
                return sizeof(ev);
            }
            const uint8_t nop[5] = { 0x0e, 3, credits, 0x00, 0x00 };
            memcpy(buf, nop, sizeof(nop));
            return sizeof(nop);
        }
        return 0;
    }

    if (buf[0] == 0x05 && len >= 6 && buf[2] == 0) {
        uint16_t h = static_cast<uint16_t>((buf[3] | (buf[4] << 8)) & 0x0fff);
        IOSimpleLockLock(g_intel_lock);
        if (LinkReady *l = linkFor(h, false)) l->used = 0;
        IOSimpleLockUnlock(g_intel_lock);
        return 0;
    }
    if (buf[0] != 0x3e || len < 6) return 0;
    uint8_t sub = buf[2];
    if (sub == 0x01 || sub == 0x0a || sub == 0x29) {       /* a new link */
        if (buf[3] != 0) return 0;
        uint16_t h = static_cast<uint16_t>((buf[4] | (buf[5] << 8)) & 0x0fff);
        IOSimpleLockLock(g_intel_lock);
        if (LinkReady *l = linkFor(h, false)) *l = LinkReady{1, 0, 0, 0, h, 0, 0, ++g_link_serial};
        else linkFor(h, true);
        IOSimpleLockUnlock(g_intel_lock);
    } else if (sub == 0x0c || sub == 0x07) {               /* a real report */
        uint16_t h = static_cast<uint16_t>(((sub == 0x0c ? buf[4] | (buf[5] << 8)
                                                         : buf[3] | (buf[4] << 8))) & 0x0fff);
        if ((sub == 0x0c && (len < 8 || buf[3] != 0)) || (sub == 0x07 && len < 13)) return 0;
        IOSimpleLockLock(g_intel_lock);
        bool go = false;
        if (LinkReady *l = linkFor(h, false)) {
            if (sub == 0x0c) {
                l->phy = 1;
                if (g_rtl_ready && (l->procedures & BT_LINK_DLE) && !(l->requested & BT_LINK_DLE) && !l->dle)
                    go = queueReady(l);
            } else {
                l->dle = 1;
            }
        }
        IOSimpleLockUnlock(g_intel_lock);
        if (go) thread_call_enter(g_ready_call);
        if (g_rtl_ready) {
            OSNumber *counter = sub == 0x0c ? g_rtl_phy_events : g_rtl_dle_events;
            if (counter) counter->addValue(1);
        }
        g_ready_real++;
        if (g_n_ready_real != nullptr) g_n_ready_real->setValue(g_ready_real);
    } else if (sub == 0x04 && buf[3] == 0x00) {            /* remote features in */
        uint16_t h = static_cast<uint16_t>((buf[4] | (buf[5] << 8)) & 0x0fff);
        bool go = false;
        IOSimpleLockLock(g_intel_lock);
        LinkReady *l = linkFor(h, !g_rtl_ready);
        if (l) l->procedures = g_rtl_ready ? bt_link_procedures(buf, len) : BT_LINK_PHY | BT_LINK_DLE;
        if (l && (!l->phy || !l->dle)) go = queueReady(l);
        IOSimpleLockUnlock(g_intel_lock);
        if (go && g_ready_call != nullptr) thread_call_enter(g_ready_call);
    }
    return 0;
}

/* bluetoothd does HCI from userspace: it opens the dongle's interface through
 * AppleUSBHostInterfaceUserClient and sends each command as a control request.
 * That never reaches IOUSBHostPipe::controlRequest — the first hardware run
 * saw 253 commands there and every one was the in-kernel Realtek firmware
 * loader (opcodes 0x1001 and the 0xfc.. vendor patch commands), with no HCI
 * Reset and no LE command among them.
 *
 * AppleUSBHostUserClient::ControlRequest is the one entry point every user
 * client funnels through: the device and interface subclasses override only
 * internalControlRequest, which this calls. Hooking the base therefore catches
 * both without having to guess which one bluetoothd holds. */
static IOReturn onUserClientControlRequest(void *that, uint8_t pipeRef,
                                           StandardUSB::DeviceRequest &request,
                                           IOMemoryDescriptor *dataBuffer,
                                           IOUSBHostCompletion *completion,
                                           uint32_t &bytesTransferred, uint32_t timeout)
{
    /* What actually gets forwarded. A buffer of BroadTek's own is substituted
     * only if the core changed something; otherwise the caller's own goes
     * through untouched. */
    IOMemoryDescriptor *send = dataBuffer;
    IOMemoryDescriptor *owned = nullptr;
    IOUSBHostCompletion *pass = completion;
    IOUSBHostCompletion wrapper;

    noteControl(request);
    {
        uint8_t buf[kMaxHCI];
        size_t len = isBluetoothRequest(static_cast<IOService *>(that), request)
            ? readCommand(request, dataBuffer, buf, kMaxHCI) : 0;
        if (len >= 3) {
            uint16_t opcode = static_cast<uint16_t>(buf[0] | (buf[1] << 8));
            g_cmds++; g_cmds_uc++;
            if (g_n_cmds != nullptr) g_n_cmds->setValue(g_cmds);
            if (g_n_cmds_uc != nullptr) g_n_cmds_uc->setValue(g_cmds_uc);
            noteOpcode(g_ops_uc, opcode);
            noteConnTarget(buf, len);
            advBeforeCommand(that, pipeRef, request, buf, len, timeout);
            bool setupEdited = g_rtl_ready ? bt_link_enable_reports(buf, len) :
                intelBeforeCommand(that, pipeRef, request, buf, len, timeout);
            if (g_intel || g_rtl_ready) intelNoteClient(that, pipeRef, request, timeout);

            if (g_rw_cmds || setupEdited) {
                /* A digest rather than a second 260-byte buffer on the stack.
                 * Any edit must be noticed, not only an address swap. */
                uint32_t before = 0;
                for (size_t i = 0; i < len; i++) before = before * 31u + buf[i];

                if (g_rw_cmds) {
                    IOSimpleLockLock(g_lock);
                    bt_hci_on_command(&g_ctx, buf, len);
                    IOSimpleLockUnlock(g_lock);
                    if (g_n_substituted != nullptr) g_n_substituted->setValue(g_ctx.substituted);
                }

                uint32_t after = 0;
                for (size_t i = 0; i < len; i++) after = after * 31u + buf[i];

                if (after != before || setupEdited) {
                    owned = ownedCopy(buf, len);
                    if (owned != nullptr) {
                        if (completion != nullptr) {
                            IOUSBHostCompletion *w = holdUntilDone(completion, &wrapper, owned);
                            if (w != nullptr) {
                                pass = w;
                                send = owned;
                                countRebuffer(true);
                            } else {
                                /* No slot free. Forwarding a buffer nothing
                                 * will release is worse than not rewriting. */
                                owned->release();
                                owned = nullptr;
                                countRebuffer(false);
                            }
                        } else {
                            send = owned;       /* synchronous: released below */
                            countRebuffer(true);
                        }
                    }
                }
            }
            OBSLOG(g_logged_cmd, "command (uc) %04x len %zu", opcode, len);
        }
    }

    IOReturn r = FunctionCast(onUserClientControlRequest, g_orig_uc_ctrl)(that, pipeRef, request,
                                                                          send, pass,
                                                                          bytesTransferred, timeout);

    /* A synchronous call is finished with the buffer on return. An
     * asynchronous one gave it to a slot, and the trampoline releases it. */
    if (owned != nullptr && pass == &wrapper && r != kIOReturnSuccess) {
        auto *slot = static_cast<Slot *>(wrapper.parameter);
        slot->owned = nullptr;
        OSDecrementAtomic(&slot->busy);
    }
    if (owned != nullptr && (pass == completion || r != kIOReturnSuccess)) owned->release();

    return r;
}

/* Every control transfer to the device converges here, whoever started it:
 * the five IOUSBHostDevice::deviceRequest overloads, the four on
 * IOUSBHostInterface, and the framework and user clients above them all end
 * up in this one function, which then goes to internalDeviceRequestGated.
 * That gated call is why the public IOUSBHostPipe::controlRequest entry sees
 * none of it -- and why the user-client hook above saw nothing either, since
 * bluetoothd's client reaches the device, not a pipe.
 *
 * The buffer arrives either as a descriptor (the user-client paths) or as a
 * plain kernel pointer (the in-kernel ones); exactly one is set. */
static IOReturn onDeviceRequest(void *that, IOService *forClient,
                                StandardUSB::DeviceRequest &request,
                                void *rawBuffer, IOMemoryDescriptor *descriptor,
                                uint32_t &bytesTransferred,
                                IOUSBHostCompletion *completion, uint32_t timeout)
{
    noteControl(request);
    if ((descriptor != nullptr || rawBuffer != nullptr) &&
        isBluetoothRequest(static_cast<IOService *>(that), request)) {
        uint8_t buf[kMaxHCI];
        size_t len = 0;

        if (descriptor != nullptr) {
            len = readMD(descriptor, buf, kMaxHCI);
        } else {
            len = request.wLength < kMaxHCI ? request.wLength : kMaxHCI;
            for (size_t i = 0; i < len; i++) buf[i] = static_cast<uint8_t *>(rawBuffer)[i];
        }

        if (len >= 3) {
            uint16_t opcode = static_cast<uint16_t>(buf[0] | (buf[1] << 8));
            g_cmds++; g_cmds_dev++;
            if (g_n_cmds != nullptr) g_n_cmds->setValue(g_cmds);
            if (g_n_cmds_dev != nullptr) g_n_cmds_dev->setValue(g_cmds_dev);
            noteOpcode(g_ops_dev, opcode);
            noteConnTarget(buf, len);

            /* Who sent it matters as much as what: the firmware loader and
             * bluetoothd both come through here. */
            noteClient(forClient);

            if (g_rw_cmds) {
                IOSimpleLockLock(g_lock);
                bt_hci_on_command(&g_ctx, buf, len);
                IOSimpleLockUnlock(g_lock);
                if (descriptor != nullptr) writeMD(descriptor, buf, len);
                else for (size_t i = 0; i < len; i++) static_cast<uint8_t *>(rawBuffer)[i] = buf[i];
            }
            OBSLOG(g_logged_cmd, "command (dev) %04x len %zu", opcode, len);
        }
    }
    return FunctionCast(onDeviceRequest, g_orig_dev_req)(that, forClient, request, rawBuffer,
                                                         descriptor, bytesTransferred,
                                                         completion, timeout);
}

/* The IOUSBHost framework client, which has two live instances on this
 * machine while both legacy clients have none. Its four control entry points
 * are hooked together: whether a reply comes back through a completion or by
 * reference makes no difference to reading the command on the way out. */
static void observeFrameworkCommand(void *client, StandardUSB::DeviceRequest &request,
                                    IOMemoryDescriptor *dataBuffer, const char *tag)
{
    noteControl(request);
    if (!isBluetoothRequest(static_cast<IOService *>(client), request)) return;
    uint8_t buf[kMaxHCI];
    size_t len = readCommand(request, dataBuffer, buf, kMaxHCI);
    if (len < 3) return;

    uint16_t opcode = static_cast<uint16_t>(buf[0] | (buf[1] << 8));
    g_cmds++; g_cmds_fw++;
    if (g_n_cmds != nullptr) g_n_cmds->setValue(g_cmds);
    if (g_n_cmds_fw != nullptr) g_n_cmds_fw->setValue(g_cmds_fw);
    noteOpcode(g_ops_fw, opcode);
    noteConnTarget(buf, len);

    if (g_rw_cmds) {
        IOSimpleLockLock(g_lock);
        bt_hci_on_command(&g_ctx, buf, len);
        IOSimpleLockUnlock(g_lock);
        writeMD(dataBuffer, buf, len);
    }
    OBSLOG(g_logged_cmd, "command (%s) %04x len %zu", tag, opcode, len);
}

static IOReturn onFwDeviceRequestAsync(void *that, StandardUSB::DeviceRequest &request,
                                       IOMemoryDescriptor *dataBuffer,
                                       IOUSBHostCompletion *completion, uint32_t timeout)
{
    observeFrameworkCommand(that, request, dataBuffer, "fw dev");
    return FunctionCast(onFwDeviceRequestAsync, g_orig_fw_dev_async)(that, request, dataBuffer,
                                                                     completion, timeout);
}

static IOReturn onFwDeviceRequestSync(void *that, StandardUSB::DeviceRequest &request,
                                      IOMemoryDescriptor *dataBuffer,
                                      uint32_t &bytesTransferred, uint32_t timeout)
{
    observeFrameworkCommand(that, request, dataBuffer, "fw dev sync");
    return FunctionCast(onFwDeviceRequestSync, g_orig_fw_dev_sync)(that, request, dataBuffer,
                                                                   bytesTransferred, timeout);
}

static IOReturn onFwPipeControlAsync(void *that, uint8_t pipeRef,
                                     StandardUSB::DeviceRequest &request,
                                     IOMemoryDescriptor *dataBuffer,
                                     IOUSBHostCompletion *completion, uint32_t timeout)
{
    observeFrameworkCommand(that, request, dataBuffer, "fw pipe");
    return FunctionCast(onFwPipeControlAsync, g_orig_fw_pipe_async)(that, pipeRef, request,
                                                                    dataBuffer, completion,
                                                                    timeout);
}

static IOReturn onFwPipeControlSync(void *that, uint8_t pipeRef,
                                    StandardUSB::DeviceRequest &request,
                                    IOMemoryDescriptor *dataBuffer,
                                    uint32_t &bytesTransferred, uint32_t timeout)
{
    observeFrameworkCommand(that, request, dataBuffer, "fw pipe sync");
    return FunctionCast(onFwPipeControlSync, g_orig_fw_pipe_sync)(that, pipeRef, request,
                                                                  dataBuffer, bytesTransferred,
                                                                  timeout);
}

/* ---- setup ----------------------------------------------------------- */

/* IOUSBHostFamily is a boot kext: it is already loaded by the time a Lilu
 * plugin starts, which is what KextInfo's "Loaded" flag is for. Routing
 * against KernelID instead resolves nothing — the first build tried that and
 * installed 0 of 3 hooks. */
static const char *kextPathUSB =
    "/System/Library/Extensions/IOUSBHostFamily.kext/Contents/MacOS/IOUSBHostFamily";
static KernelPatcher::KextInfo kextUSB {
    "com.apple.iokit.IOUSBHostFamily", &kextPathUSB, 1, {true}, {},
    KernelPatcher::KextInfo::Unloaded
};

static void mark(const char *key, bool value)
{
    IORegistryEntry *platform = IOService::getPlatform();
    if (platform != nullptr) platform->setProperty(key, value);
}

static void markString(const char *key, const char *value)
{
    IORegistryEntry *platform = IOService::getPlatform();
    if (platform != nullptr) platform->setProperty(key, value);
}

/* Userspace handing a key down. Caller context, under the same lock the HCI
 * paths use, because the resolving list is shared with them. */
bool brtkInstallKey(const uint8_t *blob, size_t len)
{
    if (blob == nullptr || len < 1 + 6 + 16 || g_lock == nullptr) return false;

    IOSimpleLockLock(g_lock);
    int ok = bt_rl_install(&g_ctx, blob[0], &blob[1], &blob[7]);
    IOSimpleLockUnlock(g_lock);

    if (ok) {
        g_keys_installed++;
        publishStatus();
    }
    return ok != 0;
}

/* Userspace handing down an address instead of a key. The iPhone has no IRK
 * on this machine, so nothing here can derive its private address; what it can
 * do is use one it is told. Caller context, under the HCI paths' lock. */
bool brtkInstallMap(const uint8_t *blob, size_t len)
{
    if (blob == nullptr || len < 1 + 6 + 6 || g_lock == nullptr) return false;

    IOSimpleLockLock(g_lock);
    int ok = bt_rl_install_map(&g_ctx, blob[0], &blob[1], &blob[7]);
    IOSimpleLockUnlock(g_lock);

    if (ok) {
        g_maps_installed++;
        if (g_n_maps_installed != nullptr) g_n_maps_installed->setValue(g_maps_installed);
        publishStatus();
    }
    return ok != 0;
}

int brtkPromoteCaptures()
{
    if (g_lock == nullptr) return 0;

    IOSimpleLockLock(g_lock);
    int n = bt_promote_captures(&g_ctx);
    IOSimpleLockUnlock(g_lock);

    if (n > 0) {
        g_keys_installed += static_cast<uint64_t>(n);
        publishStatus();
    }
    return n;
}

/*
 * The advertising gate in bluetoothd, docs/LE_ADVERTISING.md: three capability
 * functions decide from the chipset number that there is no extended or multiple
 * advertising, and the Realtek's number is 0. Each now returns what a passing
 * chipset gets. The chipset itself stays 0, so the USB transport is unchanged.
 *
 * The bytes are from macOS 26.7.2 (25G332), checked to occur once each in the
 * x86_64 slice. On any other build nothing matches and nothing changes.
 */
#define BTD_CHIPSET_HEAD 0x8b,0x8f,0x34,0x03,0x00,0x00,0x81,0xf9,0xd0,0x07,0x00,0x00,0x72,0x14, \
    0x81,0xf9,0x9f,0x0f,0x00,0x00,0x77,0x21,0xb0,0x01,0x81,0xf9,0xd0,0x07,0x00,0x00,0x74,0x0c, \
    0xeb,0x13,0x8d,0x51,0xed,0xb0,0x01,0x83,0xfa,0x07,0x72,0x09,0x81,0xf9,0x70,0x17,0x00,0x00,0x0f
#define BTD_RETURN_ONE 0xb0,0x01,0x5d,0xc3
static const uint8_t g_ext_find[]  = { BTD_CHIPSET_HEAD, 0x94, 0xc0, 0x5d };
static const uint8_t g_ext_repl[]  = { BTD_RETURN_ONE, 0x00,0x00,0x81,0xf9,0xd0,0x07,0x00,0x00,0x72,0x14,
    0x81,0xf9,0x9f,0x0f,0x00,0x00,0x77,0x21,0xb0,0x01,0x81,0xf9,0xd0,0x07,0x00,0x00,0x74,0x0c,
    0xeb,0x13,0x8d,0x51,0xed,0xb0,0x01,0x83,0xfa,0x07,0x72,0x09,0x81,0xf9,0x70,0x17,0x00,0x00,0x0f,
    0x94, 0xc0, 0x5d };
static const uint8_t g_multi_find[] = { BTD_CHIPSET_HEAD, 0x93, 0xc0, 0x5d };
static const uint8_t g_multi_repl[] = { BTD_RETURN_ONE, 0x00,0x00,0x81,0xf9,0xd0,0x07,0x00,0x00,0x72,0x14,
    0x81,0xf9,0x9f,0x0f,0x00,0x00,0x77,0x21,0xb0,0x01,0x81,0xf9,0xd0,0x07,0x00,0x00,0x74,0x0c,
    0xeb,0x13,0x8d,0x51,0xed,0xb0,0x01,0x83,0xfa,0x07,0x72,0x09,0x81,0xf9,0x70,0x17,0x00,0x00,0x0f,
    0x93, 0xc0, 0x5d };
static const uint8_t g_count_find[] = { 0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,
    0x83,0xec,0x18,0x48,0x89,0xfb,0x48,0x8b,0x05,0x51,0xc6,0x74,0x00,0x48 };
static const uint8_t g_count_repl[] = { 0x55,0x48,0x89,0xe5,0xb0,0x04,0x5d,0xc3,0x41,0x55,0x41,0x54,0x53,0x48,
    0x83,0xec,0x18,0x48,0x89,0xfb,0x48,0x8b,0x05,0x51,0xc6,0x74,0x00,0x48 };
static_assert(sizeof(g_ext_find) == sizeof(g_ext_repl) && sizeof(g_multi_find) == sizeof(g_multi_repl) &&
              sizeof(g_count_find) == sizeof(g_count_repl), "a patch changes length");

static UserPatcher::BinaryModPatch g_btd_patches[] = {
    { CPU_TYPE_X86_64, 0, g_ext_find, g_ext_repl, sizeof(g_ext_find), 0, 1, UserPatcher::FileSegment::SegmentTextText, 1 },
    { CPU_TYPE_X86_64, 0, g_multi_find, g_multi_repl, sizeof(g_multi_find), 0, 1, UserPatcher::FileSegment::SegmentTextText, 1 },
    { CPU_TYPE_X86_64, 0, g_count_find, g_count_repl, sizeof(g_count_find), 0, 1, UserPatcher::FileSegment::SegmentTextText, 1 },
};
/*
 * Intel controllers (AX200, AX201, AX210, AX211: USB vendor 0x8087) follow the
 * spec where the Realtek did not: once bluetoothd has used an extended
 * advertising command, they refuse the legacy scan and create-connection
 * commands. bluetoothd keeps using those because it also asks the chipset
 * whether it supports extended scanning (vtable slot 0x9a8) and whether to use
 * LE Extended Create Connection (0x2043), and an unknown chipset gets "no" to
 * both. So on Intel two more answers change:
 *
 *  - the extended-scan query returns 1, like the advertising ones above;
 *  - the setter of the "use 0x2043" flag stores 1 whatever it is given
 *    (movb %dil -> movb $1, same length and the same rip displacement).
 *
 * Both from macOS 26.7.3 (25G342), each checked to occur once in the file.
 * Not applied to Realtek, where the legacy commands work and these were never
 * tried.
 */
static const uint8_t g_xscan_find[] = { 0x55,0x48,0x89,0xe5,0x8b,0x8f,0x34,0x03,0x00,0x00,0x85,0xc9,
    0x74,0x12,0x81,0xf9,0xcf,0x07,0x00,0x00,0x77,0x15,0x8d,0x51,0xed,0xb0,0x01,0x83,0xfa,0x07,
    0x72,0x09,0x81,0xf9,0x70,0x17,0x00,0x00,0x0f,0x94,0xc0,0x5d,0xc3,0x81,0xf9,0x9f,0x0f,0x00,
    0x00,0x77,0x0c,0xb0 };
static const uint8_t g_xscan_repl[] = { 0x55,0x48,0x89,0xe5,0xb0,0x01,0x5d,0xc3,0x00,0x00,0x85,0xc9,
    0x74,0x12,0x81,0xf9,0xcf,0x07,0x00,0x00,0x77,0x15,0x8d,0x51,0xed,0xb0,0x01,0x83,0xfa,0x07,
    0x72,0x09,0x81,0xf9,0x70,0x17,0x00,0x00,0x0f,0x94,0xc0,0x5d,0xc3,0x81,0xf9,0x9f,0x0f,0x00,
    0x00,0x77,0x0c,0xb0 };
static const uint8_t g_xconn_find[] = { 0x55,0x48,0x89,0xe5,0x40,0x88,0x3d,0xcc,0x5f,0x92,0x00,0x5d,0xc3,
    0x55,0x48,0x89,0xe5,0x40,0x88,0x3d };
static const uint8_t g_xconn_repl[] = { 0x55,0x48,0x89,0xe5,0xc6,0x05,0xcc,0x5f,0x92,0x00,0x01,0x5d,0xc3,
    0x55,0x48,0x89,0xe5,0x40,0x88,0x3d };
static_assert(sizeof(g_xscan_find) == sizeof(g_xscan_repl) && sizeof(g_xconn_find) == sizeof(g_xconn_repl),
              "a patch changes length");

static UserPatcher::BinaryModPatch g_btd_intel_patches[] = {
    { CPU_TYPE_X86_64, 0, g_xscan_find, g_xscan_repl, sizeof(g_xscan_find), 0, 1, UserPatcher::FileSegment::SegmentTextText, 1 },
    { CPU_TYPE_X86_64, 0, g_xconn_find, g_xconn_repl, sizeof(g_xconn_find), 0, 1, UserPatcher::FileSegment::SegmentTextText, 1 },
};

static UserPatcher::BinaryModInfo g_btd_mod { "/usr/sbin/bluetoothd", g_btd_patches, arrsize(g_btd_patches) };
static UserPatcher::ProcInfo g_btd_procs[2] = {
    { "/usr/sbin/bluetoothd", sizeof("/usr/sbin/bluetoothd") - 1, 1, UserPatcher::ProcInfo::MatchExact },
    { "/", 1, 2, UserPatcher::ProcInfo::MatchPrefix },
};

/*
 * The bluetoothd patch, applied as the kernel validates each code page.
 *
 * Lilu's onProcLoad never reaches bluetoothd here (Tahoe 25G342, Lilu 1.7.3):
 * with a catch-all entry beside the bluetoothd one, broadtek-proc-loads never
 * appeared at all, so Lilu reported no process launch whatsoever. The same
 * page-validation route is how BlueToolFixup and other plugins patch daemons:
 * cs_validate_page() runs on every page of a signed binary as it is paged in,
 * after the signature check, and the page is still ours to change then. Routed
 * with a long jump so it can share the function with another plugin's route.
 */
static mach_vm_address_t g_orig_cs_validate;
static OSNumber *g_n_btd_pages;

static void onCsValidatePage(vnode_t vp, memory_object_t pager, memory_object_offset_t offset,
                             const void *data, int *validated, int *tainted, int *nx)
{
    FunctionCast(onCsValidatePage, g_orig_cs_validate)(vp, pager, offset, data, validated, tainted, nx);

    char path[64];
    int len = sizeof(path);

    if (!vp || !data || vn_getpath(vp, path, &len) != 0 || strcmp(path, "/usr/sbin/bluetoothd") != 0)
        return;
    for (const auto &patch : g_btd_patches) {
        if (KernelPatcher::findAndReplace(const_cast<void *>(data), PAGE_SIZE,
                                          patch.find, patch.size, patch.replace, patch.size)) {
            if (g_n_btd_pages)
                g_n_btd_pages->addValue(1);
        }
    }
    if (!g_intel) return;
    for (const auto &patch : g_btd_intel_patches) {
        if (KernelPatcher::findAndReplace(const_cast<void *>(data), PAGE_SIZE,
                                          patch.find, patch.size, patch.replace, patch.size)) {
            if (g_n_intel_pages)
                g_n_intel_pages->addValue(1);
        }
    }
}

static IOReturn onPowerMessage(void *, void *, UInt32 message, IOService *, void *, vm_size_t)
{
    if (message == kIOMessageSystemWillSleep || message == kIOMessageSystemHasPoweredOn) {
        IOSimpleLockLock(g_lock);
        bt_adv_invalidate(&g_adv_state);
        IOSimpleLockUnlock(g_lock);
    }
    return kIOReturnSuccess;
}

void BRTK::init()
{
    /* An experiment that shares nothing with the HCI path: it hides USB nodes
     * from bluetoothd's dongle test so the LE advertising stack is allowed to
     * start (docs/LE_ADVERTISING.md). Off unless -broadtekclass. */
    bool observe = checkKernelArgument("-broadtekobserve");
    if (!observe) brtkUsbClassInit();

    g_rw_cmds = !observe && checkKernelArgument("-broadtekrw");
    g_rw_events = !observe && (g_rw_cmds || checkKernelArgument("-broadtekrwev"));
    /* On by default from 0.2.0 (AirDrop and Handoff need it); -broadtekadvaddroff
     * turns it off. -broadtekadvaddr is still accepted. */
    g_adv_addr = !observe && !checkKernelArgument("-broadtekadvaddroff");
    g_lock = IOSimpleLockAlloc();
    if (g_lock == nullptr) return;
    if (g_adv_addr) {
        g_adv_send_lock = IOLockAlloc();
        g_adv_addr = g_adv_send_lock != nullptr;
        if (g_adv_addr && checkKernelArgument("-broadtekadvcache"))
            g_adv_cache = registerSleepWakeInterest(onPowerMessage, nullptr) != nullptr;
    }
    bt_ctx_init(&g_ctx);
    for (int i = 0; i < kSlots; i++) g_slots[i].busy = 0;

    const char *stage = g_rw_cmds ? "full" : (g_rw_events ? "events" : "off");
    SYSLOG("broadtek", "started, build %s (rewriting %s)", BROADTEK_BUILD, stage);
    mark("broadtek-started", true);
    markString("broadtek-build", BROADTEK_BUILD);
    markString("broadtek-rewriting", stage);
    mark("broadtek-adv-addr", g_adv_addr);
    mark("broadtek-adv-cache", g_adv_cache);

    /* Intel: on when an Intel Bluetooth device shows up, or always with
     * -broadtekintel; -broadtekinteloff keeps it off. */
    bool intelAllowed = !observe && !checkKernelArgument("-broadtekinteloff");
    bool rtlRequested = !observe && checkKernelArgument("-broadtekrtlready");
    if (intelAllowed || rtlRequested) {
        intelMakeStaticAddress();
        g_n_intel_pages      = makeCounter("broadtek-intel-pages");
        g_n_intel_masked     = makeCounter("broadtek-intel-event-mask");
        g_n_intel_rand_sent  = makeCounter("broadtek-intel-random-addr");
        g_n_ready_phy        = makeCounter("broadtek-intel-ready-phy");
        g_n_ready_dle        = makeCounter("broadtek-intel-ready-dle");
        g_n_ready_real       = makeCounter("broadtek-intel-ready-real");
        g_intel_lock = IOSimpleLockAlloc();
        if (g_intel_lock != nullptr && !checkKernelArgument("-broadtekreadyoff"))
            g_ready_call = thread_call_allocate(intelReadyWork, nullptr);
        g_intel_allowed = intelAllowed && g_intel_lock != nullptr;
        g_rtl_ready_allowed = rtlRequested && g_ready_call != nullptr;
        if (g_rtl_ready_allowed) {
            g_rtl_phy_events = makeCounter("broadtek-rtl-phy-events");
            g_rtl_dle_events = makeCounter("broadtek-rtl-dle-events");
            g_rtl_phy_status = makeCounter("broadtek-rtl-phy-status");
            g_rtl_dle_status = makeCounter("broadtek-rtl-dle-status");
            // 256 means no controller response has been observed yet.
            if (g_rtl_phy_status) g_rtl_phy_status->setValue(256);
            if (g_rtl_dle_status) g_rtl_dle_status->setValue(256);
        }
        g_force_intel = g_intel_allowed && checkKernelArgument("-broadtekintel");
        g_intel = g_force_intel;
    }
    g_n_adv_injected     = makeCounter("broadtek-adv-injected");
    g_n_adv_inj_timeouts = makeCounter("broadtek-adv-inj-timeouts");
    g_n_adv_inj_failed   = makeCounter("broadtek-adv-inj-failed");
    g_n_adv_inj_cc       = makeCounter("broadtek-adv-inj-replies");
    g_n_adv_inj_status   = makeCounter("broadtek-adv-inj-status");
    g_n_maps_installed   = makeCounter("broadtek-maps-installed");
    g_n_hooks     = makeCounter("broadtek-hooks");
    g_n_cmds      = makeCounter("broadtek-commands");
    g_n_cmds_pipe = makeCounter("broadtek-cmds-pipe");
    g_n_cmds_uc   = makeCounter("broadtek-cmds-uc");
    g_n_cmds_dev  = makeCounter("broadtek-cmds-dev");
    g_n_cmds_fw   = makeCounter("broadtek-cmds-fw");
    g_n_ctrl_calls = makeCounter("broadtek-ctrl-calls");
    g_n_evts      = makeCounter("broadtek-events");
    g_n_io_calls  = makeCounter("broadtek-io-calls");
    g_n_acl_in    = makeCounter("broadtek-acl-in");
    g_n_cmd_nobuf = makeCounter("broadtek-cmd-nobuf");
    g_n_keys_installed = makeCounter("broadtek-keys-installed");
    g_n_rl_entries = makeCounter("broadtek-resolving-list");
    g_n_resolved    = makeCounter("broadtek-resolved");
    g_n_substituted = makeCounter("broadtek-substituted");
    g_n_conn_events     = makeCounter("broadtek-conn-events");
    g_n_conn_translated = makeCounter("broadtek-conn-translated");
    g_n_write_noprepare  = makeCounter("broadtek-write-noprepare");
    g_n_write_short      = makeCounter("broadtek-write-short");
    g_n_cmd_rebuffered      = makeCounter("broadtek-cmd-rebuffered");
    g_n_cmd_rebuffer_failed = makeCounter("broadtek-cmd-rebuffer-failed");
    g_n_write_unverified = makeCounter("broadtek-write-unverified");
    g_n_tables_full = makeCounter("broadtek-tables-full");
    g_n_pipes_elsewhere = makeCounter("broadtek-pipes-elsewhere");
    g_n_cmd_short = makeCounter("broadtek-cmd-short");
    g_n_smp_pdus  = makeCounter("broadtek-smp-pdus");
    g_n_irks      = makeCounter("broadtek-irks");
    g_n_smp_bad   = makeCounter("broadtek-smp-unparsed");
    g_n_evt_shaped = makeCounter("broadtek-evt-hci-shaped");
    g_n_evt_other  = makeCounter("broadtek-evt-not-hci");
    g_n_btd_pages  = makeCounter("broadtek-btd-pages-patched");
    mark("broadtek-hci-bound", false);
    IOService::addMatchingNotification(gIOTerminatedNotification, IOService::serviceMatching("IOUSBHostInterface"),
                                       onBluetoothTerminated, nullptr, nullptr, 0);
    IOService::addMatchingNotification(gIOMatchedNotification, IOService::serviceMatching("IOUSBHostInterface"),
                                       onBluetoothInterface, nullptr, nullptr, 0);
    publishStatus();

    if (!observe && !checkKernelArgument("-broadtekadvoff")) {
        /* Two registrations in one: bluetoothd, which carries the patches, and
         * every process (prefix "/"), which carries none and only counts. If
         * the first count stays at 0 while the second grows, Lilu runs its
         * exec hook but does not match bluetoothd by this path; if both stay
         * at 0, the hook never runs on this macOS at all. */
        lilu.onProcLoadForce(g_btd_procs, 2,
            [](void *, UserPatcher &, vm_map_t, const char *path, size_t len) {
                static volatile uint32_t all, btd;
                IORegistryEntry *platform = IOService::getPlatform();
                uint32_t n = __sync_add_and_fetch(&all, 1);
                bool is_btd = path != nullptr && len == 20 && memcmp(path, "/usr/sbin/bluetoothd", 20) == 0;

                if (platform == nullptr) return;
                if (is_btd) {
                    platform->setProperty("broadtek-btd-loads", static_cast<uint64_t>(__sync_add_and_fetch(&btd, 1)), 32);
                    platform->setProperty("broadtek-btd-path", path);
                }
                if (n <= 8 || (n & 15) == 0)
                    platform->setProperty("broadtek-proc-loads", static_cast<uint64_t>(n), 32);
            }, nullptr, &g_btd_mod, 1);
        mark("broadtek-btd-registered", true);
        SYSLOG("broadtek", "advertising patch for bluetoothd registered");
    }

    lilu.onPatcherLoadForce([](void *user, KernelPatcher &patcher) {
        static_cast<BRTK *>(user)->onPatcherLoad(patcher);
    }, this);

    lilu.onKextLoadForce(&kextUSB, 1,
        [](void *user, KernelPatcher &patcher, size_t id, mach_vm_address_t slide, size_t size) {
            static_cast<BRTK *>(user)->onKextLoad(patcher, id, slide, size);
        }, this);
}

/* Both callbacks come here; whichever arrives first and resolves the symbols
 * wins, and the other does nothing. */
void BRTK::installRoutes(KernelPatcher &patcher, size_t id,
                         mach_vm_address_t start, size_t size, const char *via)
{
    if (g_lock == nullptr) {
        SYSLOG("broadtek", "no lock, staying out of the way");
        return;
    }
    if (g_routed > 0) return;

    KernelPatcher::RouteRequest routes[] = {
        { "__ZN18IOUSBHostInterface13copyPipeGatedEhRP13IOUSBHostPipe", onCopyPipe, g_orig_copy_pipe },
        { "__ZN13IOUSBHostPipe14controlRequestEP9IOServiceRN11StandardUSB13DeviceRequestEP18IOMemoryDescriptorP19IOUSBHostCompletionj",
          onControlRequestAsync, g_orig_ctrl_async },
        { "__ZN13IOUSBHostPipe14controlRequestEP9IOServiceRN11StandardUSB13DeviceRequestEP18IOMemoryDescriptorRjj",
          onControlRequestSync, g_orig_ctrl_sync },
        { "__ZN13IOUSBHostPipe2ioEP18IOMemoryDescriptorjP19IOUSBHostCompletionj",
          onIOAsync, g_orig_io_async },
        { "__ZN22AppleUSBHostUserClient14ControlRequestEhRN11StandardUSB13DeviceRequestE"
          "P18IOMemoryDescriptorP19IOUSBHostCompletionRjj",
          onUserClientControlRequest, g_orig_uc_ctrl },
        { "__ZN15IOUSBHostDevice21internalDeviceRequestEP9IOServiceRN11StandardUSB13DeviceRequestE"
          "PvP18IOMemoryDescriptorRjP19IOUSBHostCompletionj",
          onDeviceRequest, g_orig_dev_req },
        { "__ZN36AppleUSBHostFrameworkInterfaceClient13deviceRequestERN11StandardUSB13DeviceRequestE"
          "P18IOMemoryDescriptorP19IOUSBHostCompletionj",
          onFwDeviceRequestAsync, g_orig_fw_dev_async },
        { "__ZN36AppleUSBHostFrameworkInterfaceClient13deviceRequestERN11StandardUSB13DeviceRequestE"
          "P18IOMemoryDescriptorRjj",
          onFwDeviceRequestSync, g_orig_fw_dev_sync },
        { "__ZN36AppleUSBHostFrameworkInterfaceClient22hostPipeControlRequestEhRN11StandardUSB"
          "13DeviceRequestEP18IOMemoryDescriptorP19IOUSBHostCompletionj",
          onFwPipeControlAsync, g_orig_fw_pipe_async },
        { "__ZN36AppleUSBHostFrameworkInterfaceClient22hostPipeControlRequestEhRN11StandardUSB"
          "13DeviceRequestEP18IOMemoryDescriptorRjj",
          onFwPipeControlSync, g_orig_fw_pipe_sync },
    };

    /* One at a time: a symbol that cannot be found must not take the others
     * down with it. */
    for (auto &r : routes) {
        if (patcher.routeMultiple(id, &r, 1, start, size)) {
            g_routed++;
        } else {
            SYSLOG("broadtek", "cannot route %s", r.symbol);
            patcher.clearError();
        }
    }

    IORegistryEntry *platform = IOService::getPlatform();
    if (platform != nullptr && g_routed > 0) platform->setProperty("broadtek-via", via);
    publishStatus();
    SYSLOG("broadtek", "%d/%d hooks in place via %s, rewriting %s",
           g_routed, static_cast<int>(arrsize(routes)), via,
           g_rw_cmds ? "full" : (g_rw_events ? "events only" : "off (observing)"));
}

void BRTK::onKextLoad(KernelPatcher &patcher, size_t id, mach_vm_address_t slide, size_t size)
{
    mark("broadtek-kext-seen", true);
    if (id != kextUSB.loadIndex) return;
    installRoutes(patcher, id, slide, size, "kext");
}

void BRTK::onPatcherLoad(KernelPatcher &patcher)
{
    mark("broadtek-patcher-ran", true);
    if (!checkKernelArgument("-broadtekobserve") && !checkKernelArgument("-broadtekadvoff") && !g_orig_cs_validate) {
        KernelPatcher::RouteRequest route {"_cs_validate_page", onCsValidatePage, g_orig_cs_validate};

        if (patcher.routeMultipleLong(KernelPatcher::KernelID, &route, 1))
            mark("broadtek-btd-pagehook", true);
        else {
            mark("broadtek-btd-pagehook", false);
            patcher.clearError();
        }
    }
    /* Only as a fallback: if the kext callback has already routed, this does
     * nothing. */
    installRoutes(patcher, KernelPatcher::KernelID, 0, 0, "kernel");
}
