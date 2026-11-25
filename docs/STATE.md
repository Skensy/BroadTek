# State of the work

Everything needed to pick BroadTek up from cold: what it is, what has been
established, what is built, what is still open, and the rules the work follows.
Current as of 2026-10-15, commit `8714cce`.

Read [DESIGN.md](DESIGN.md) next for the protocol detail.

## 1. What this is

A Lilu plugin that gives a Bluetooth controller without LL Privacy the ability
to resolve the private addresses Apple devices advertise with, so macOS can
connect to them.

Two things fail on the test machine without it:

- **Instant Hotspot** — picking the iPhone in the Wi-Fi menu does nothing;
  sharingd logs `BLE client connect timeout` after about 10 s.
- **Apple Watch approval** — "Check your watch" appears on the Mac, nothing
  ever arrives on the watch.

Both are the same failure. Apple devices advertise from a resolvable private
address (RPA) that changes every 15 minutes. macOS stores the device's identity
resolving key (IRK), puts its *identity* address on the controller's Filter
Accept List, and expects the controller to match incoming RPAs against the key.
A controller without LL Privacy cannot, so nothing ever matches.

BroadTek is a separate project from AirPort_RTW89 (the Wi-Fi driver in
`~/Developer/AirPort_RTW89`) and may one day ship as a PlugIn inside it. Right
now they are independent; BroadTek has no GitHub remote and has never been
pushed.

## 2. The machine

Everything is developed and tested on one machine, a hackintosh:

| | |
|---|---|
| Laptop | ASUS TUF A15 FA507NU, Ryzen 7 7735HS, ~28 GB usable RAM |
| macOS | Sequoia 15.8.1 (24H32), x86_64 |
| Graphics | **no acceleration** (no driver for the Radeon 680M) — WindowServer is software-rendered |
| Bluetooth | RTL8852BE combo card, USB `13D3:3571`, firmware by RealtekBluetoothFirmware.kext |
| Wi-Fi | the same card, driven by AirPort_RTW89 |
| Bootloader | OpenCore 1.0.8, `csr-active-config` `0xA03` (unsigned kexts allowed) |
| Lilu | 1.7.3, already in the EFI |
| EFI | **internal**, UUID `<EFI partition UUID>`, FAT32, 96 MB, **only ~12 MB free**. The device name moves: it has been `disk1s1` and `disk0s1` on different boots, so always name it by UUID |

There is also a USB rescue stick with its own OpenCore; it is usually not
plugged in. `efi/find_efi.sh` lists both and stops if it cannot tell them
apart, so pass the UUID above when in doubt.

No GPU acceleration has two consequences that matter: **build gently**
(`nice -n 20 make -j4` at most — `-j16` once starved WindowServer and froze the
machine) and avoid the `-v` boot-arg unless diagnosing, because verbose boot is
very slow.

## 3. What was established, and what it rules out

The evidence is in `~/Developer/AirPort_RTW89/logs/bt.txt` and
`bt-restart.txt` (git-ignored, captured 2026-10-15 during a Bluetooth restart).

### The controller lacks LL Privacy

```
Stack.HCI  LE local supported features BD 5F 66 3C 01 00 00 00
```

Byte 0 is `0xBD` = `1011 1101`: bit 6 (LL Privacy) is clear. Bit 12 (extended
advertising) is set, so extended advertising reports have to be handled too.

### macOS gives up on the connection

```
Server.Core  MUC - enhancedFilterAcceptListSupported? 0 chipset 0x1, 284
Server.LE.Security  address Public 20:00:00:00:00:03 Incorrect IRK <private>
```

**Correction, 2026-10-16:** `20:00:00:00:00:03` is not the iPhone. Today's
log has it by name -- `Nm 'My AirPods', BDA 20:00:00:00:00:03` --
and the iPhone is `64:00:00:00:00:01` (`Nm 'My iPhone'`, model
iPhone18,4). So the `Incorrect IRK` line above was about the AirPods, not the
phone. The conclusion that private addresses are not being resolved still
stands on the rest of §3, but it was never demonstrated on the iPhone, and
anything reasoning from that address should be re-checked.

### The obvious route is a dead end — do not retry it

The original plan was to intercept Apple's Broadcom vendor command
`BD_VSC_LE_META_ADD_IRK_TO_LIST`, keep the IRKs and answer success. **This
cannot work.** bluetoothd classifies the dongle as `THIRD_PARTY_DONGLE`
(chipset 0) and answers every Broadcom VSC *itself*:

```
14:22:16.846192  Server.Core  Sending: BD_VSC_LE_META_ADD_IRK_TO_LIST
14:22:16.846192  Server.Core  VSC BD_VSC_LE_META_ADD_IRK_TO_LIST, not supported
```

Note the identical timestamps: the command never reaches the controller, so
nothing below bluetoothd can see those IRKs. The log reads as if it was sent,
which is exactly why this is worth writing down.

### The standard route is a dead end too — and this is the important one

Stage `events` was run on 2026-10-15 (build `72ecbfb`). The features rewrite
**works**, proven not by BroadTek's own counters but by bluetoothd's log:

```
Stack.HCI  LE local supported features FD 5F 66 3C 00 00 00 00
```

`FD`, where the controller sends `BD`. Bit 6 set, read and logged by
bluetoothd. §4 step 1 does exactly what it was designed to do.

It changes nothing. No resolving-list command was sent: `broadtek-opcodes-cc`
and `broadtek-opcodes-uc` both hold 32 opcodes and neither contains `2027`,
`2029`, `202a`, `202d` or `204e`. The reason is three lines in the same log:

```
Server.Core  MUC - enhancedFilterAcceptListSupported enabled? 0
Server.Core  MUC - enhancedFilterAcceptListSupported? 0 chipset 0x0, 284
Server.Core  LE Feature Vendor List is disabled (hw N)
```

and, from `GetControllerInfo`, `Chip THIRD_PARTY_DONGLE`, followed by
`THIRD_PARTY_DONGLE supports the following VSCs` with **nothing after it**.

**macOS decides whether it can resolve private addresses from its chipset
classification, not from the controller's HCI features.** Both of its
mechanisms are gated the same way:

| Mechanism | Gate | Result |
|---|---|---|
| Apple's IRK vendor list (`BD_VSC_LE_META_ADD_IRK_TO_LIST`) | `LE Feature Vendor List is disabled (hw N)`, and the VSC list is empty | never sent to the controller (§3) |
| The standard HCI resolving list (`0x2027`...) | `enhancedFilterAcceptListSupported? 0 chipset ...` | never sent either |

So the premise the whole design rests on -- report LL Privacy and bluetoothd
will hand the IRKs over with standard HCI commands -- **is disproven.** It
will not, because it never asks the controller what it can do. A
`THIRD_PARTY_DONGLE` gets neither route regardless of what its features say.

bluetoothd instead does what §1 describes: `Server.LE.Connection  MUC -
Adding device "..." to connection filter accept list ... waiting for
controller to complete.` It puts the identity address on the accept list and
waits, which is the original failure.

No regression from the rewrite: AirPods stayed connected
(`DvNm 'My AirPods'`) and scanning kept working throughout.

One caveat on the §3 baseline. The features now read `FD 5F 66 3C 00 00 00 00`
where §3 recorded `BD 5F 66 3C 01 00 00 00`: byte 0 is BroadTek's edit, but
byte 4 going `01` to `00` is **not** -- the rewrite touches feature byte 0 and
nothing else. That byte differs because the §3 capture was taken during a
Bluetooth restart and this one after a cold boot with freshly loaded firmware.
Worth knowing before treating the §3 bytes as exact.

### Taking the IRK off the wire instead

Written 2026-10-16, tested in userspace, **not yet seen on hardware.**

macOS will not hand an IRK to this controller, for the reasons just given. It
does not have to. When two LE devices bond, the key distribution phase sends
SMP Identity Information (the IRK, 16 octets) and Identity Address
Information (the identity it belongs to) over L2CAP channel `0x0006`.
Link-layer encryption is applied by the controller, *below* HCI, so on the USB
bus those PDUs are plaintext ACL data.

And BroadTek is already on that pipe. The eighth run proves it:
`broadtek-endpoints = "81:int:1099 82:bulk:99 83:int:1 02:bulk:98"`, and that
table is built inside `onIOAsync`, so bulk transfers cross the same hooked
`IOUSBHostPipe::io` as the interrupt endpoint. Reading ACL needed no new hook,
only a wider predicate (`isAclInPipe`) and a completion that knows which pipe
its slot came from.

Why §3's gate does not block this: **macOS never has to know resolution
happened.** With an IRK in hand BroadTek resolves the RPA itself, substitutes
it into the accept-list add bluetoothd already sends -- which its own log
shows it sending, `MUC - Adding device "..." to connection filter accept list
... waiting for controller to complete` -- and rewrites Connection Complete
back to the identity address. `enhancedFilterAcceptListSupported`, the empty
VSC list and `chipset 0x0` are irrelevant to that. The chipset gate stops
macOS *delegating* resolution. It does not stop BroadTek doing it unasked.

Only the IN direction is read: the key that resolves the peer's private
address is the peer's, and the one the Mac sends out is its own.

#### The key is never published

An IRK is a cryptographic key, and `ioreg -l` needs no privileges, so a key
put into the IO registry would be readable by every process on the machine.
The keys stay inside the kext: not in the registry, not in the log, not in
`dmesg`. What is published is how many captures exist and which identity
addresses they belong to. An identity address is not a secret -- bluetoothd
prints them, and this document quotes one.

#### Capture is not use

`bt_acl_scan()` only reads, in every stage including `full`: a bonding
exchange is the last thing to disturb. A captured IRK goes into
`bt_ctx.smp[]`, which nothing else reads, and is deliberately *not* put into
the resolving list, so capture can be confirmed before anything depends on it.
A test asserts the resolving list stays empty. Promoting a capture is the next
step and a separate one.

#### What the test covers, and one invariant

The spec's own sample IRK is fed through a real ACL/L2CAP/SMP frame, and the
captured key is then checked to resolve the spec's RPA: capturing bytes is
worth nothing unless they are the right bytes in the right order. SMP sends
the IRK least significant octet first, which is already the order `ah()`
wants, so no byte swap is involved. Also covered: identity address capture,
two devices on two handles, a re-bond not filling the table, the wrong L2CAP
channel, continuations, truncated PDUs, lying ACL and L2CAP lengths, and runts
at every header boundary. Removing the ACL length guard fails two of them, so
they have teeth.

The invariant: the kext reads only the **first 64 bytes** of an ACL packet. A
completion routine's stack cannot hold more -- the compiler rejects a frame
over 1024 bytes, and a first version with a 1 KB buffer was rejected for
exactly that. 64 is enough because an identity PDU is 25 bytes including both
headers, so anything longer is not SMP. A test asserts both halves: an
identity PDU parses inside the prefix, and a 600-byte frame seen only as a
prefix yields nothing and reads nothing beyond what it was handed.

#### What it needs from the hardware

Bonding happens once, so the IRKs of devices paired before this build went in
have already gone past. One fresh bond per device is needed, with BroadTek
loaded:

1. `sudo efi/install.sh --stage off <uuid>`, then restart. Capture is
   read-only and needs no rewriting stage.
2. Forget the iPhone on the Mac, and pair it again.
3. `ioreg -l | grep -i broadtek`. `broadtek-acl-in` climbing says the ACL pipe
   is being read at all, `broadtek-smp-pdus` says SMP traffic was recognised,
   and `broadtek-irks` reaching 1 is the result.

If `acl-in` climbs but `smp-pdus` stays at 0, bonding either did not happen or
did not take this path. If `smp-unparsed` climbs instead, the PDUs are
arriving fragmented after all and reassembly is needed.

The Watch is heavier: its bond is bound up with Auto Unlock, and whether it
can be re-established without disturbing that is unknown. Start with the
iPhone and Instant Hotspot -- one device, one re-pair, a clear pass or fail.

### What the hardware said about capturing it (2026-10-16)

Build `d8932a8`, stage `off`.

**Established: the ACL path works.** `broadtek-acl-in = 170`, so ACL transfers
really are being read through the hooked `IOUSBHostPipe::io`, with
`broadtek-endpoints = "81:int:1259 82:bulk:171 83:int:1 02:bulk:160"`. The
plumbing the capture depends on is confirmed on hardware.

**Not established: anything about capture.** `broadtek-smp-pdus = 0`,
`broadtek-irks = 0`, `broadtek-smp-unparsed = 0` -- but **no bond was
attempted during this run.** The machine was restarted and the counters read;
nothing was forgotten and re-paired. Key distribution happens once, at
bonding, so a run without a bond cannot show an IRK and says nothing either
way about the parser. The real test has not been performed yet.

An earlier version of this section drew a conclusion from that zero. It was
wrong to, and the mistake is the same one §7 records twice already: a counter
reading zero was taken as evidence about the mechanism when it was evidence
about the conditions.

#### A hypothesis the run did raise

bluetoothd describes both target devices this way:

```
Nm 'My iPhone'   DvF 0x1B080 < SameAccount Hidden BLEPaired CloudPaired ... >
Nm 'My Watch'   DvF 0x1B080 < SameAccount Hidden BLEPaired CloudPaired ... >
```

`BLEPaired` **and** `CloudPaired`. One reading is that the LE bond was synced
through iCloud rather than negotiated over the air, in which case the IRK
never crosses the USB bus here and re-pairing would not put it there. That
would close this route for Apple devices on the same account and leave reading
the key from the local store as the only option -- `CloudPaired` implies the
key is held locally either way.

It is a hypothesis, not a finding. What settles it is doing the test:

1. Forget the iPhone and pair it again. **This turned out not to be
   available:** the device is not in the Bluetooth pane or anywhere in
   Settings to unpair, which is what the `Hidden` flag means.
   `system_profiler SPBluetoothDataType` does list it (`My iPhone`,
   `64:00:00:00:00:01`, "Not Connected"), so the Mac holds a bond for it, but
   there is no user path to break and remake that bond.
2. So the hypothesis stands unrefuted and the route is unexercisable for the
   devices that matter. If no bond can be triggered, no key exchange can be
   watched, whatever the parser is capable of.

A manual bond with a **non-Apple** LE peripheral is the cheaper half of the
test and worth doing first: it has no iCloud path, so it must distribute keys
over SMP. It proves the parser end to end on hardware, with an IRK BroadTek
has no use for. Not every peripheral sends one -- only those using privacy --
so `smp-pdus` climbing while `irks` stays at 0 would mean the parser is
reading SMP and that device simply has no IRK to give.

One instrumentation gap to close before the next ACL run: when an ACL packet
is longer than the 64-byte prefix the kext reads, `bt_acl_scan()` returns
without counting anything, so "no SMP arrived" and "a large packet was stepped
over" look identical. An SMP identity PDU is 25 bytes and would have fitted,
so it does not affect this result, but the next run should not have to assume
that.

### The experiment nobody has run yet (2026-10-16)

Every opcode list collected so far -- `broadtek-opcodes-cc`, `-uc`, `-dev` --
contains bluetoothd's startup sequence and nothing else. Checked explicitly,
not one of these has ever appeared:

| Opcode | | Seen |
|---|---|---|
| `0x200b`/`0x200c` | LE Set Scan Parameters / Enable | no |
| `0x200d`/`0x2043` | LE (Extended) Create Connection | no |
| `0x200e` | LE Create Connection Cancel | no |
| `0x2011`/`0x2012` | LE Add / Remove Device To Filter Accept List | no |

Which means **the moment this project exists to fix has never been observed
with the hooks in place.** Nobody tried Instant Hotspot while BroadTek was
loaded. Every run restarted the machine and read the counters, so every run
captured initialisation and stopped there. The §3 evidence that macOS puts an
identity address on the accept list and waits comes from logs captured before
any of this instrumentation existed.

`broadtek-conn-targets` now publishes the address carried by each of
`0x2011`, `0x2012`, `0x200d` and `0x2043`, with its type classified, the first
time each distinct one is seen. It is recorded in every stage, including `off`,
because it is observation.

The type is the point:

| What appears | Means |
|---|---|
| `fal+/pub/64:00:00:00:00:01` | macOS is asking the controller to match an identity address. The §3 failure, now observed directly, and the substitution in §4 step 4 is exactly the fix. |
| `fal+/RPA/...` | **bluetoothd resolved the peer itself and is handing the controller a usable address.** No IRK is needed from anywhere, and most of this project's difficulty evaporates. |
| `fal+/rnd/...` | a static random address: neither case, worth looking at closely. |
| nothing at all | bluetoothd does not use the accept list for this, and §3's reading of the older logs needs revisiting. |

It needs the build that carries it (`c35b275` or later) installed and a
restart, but no pairing and no rewriting stage. Then: try Instant Hotspot,
and read `broadtek-conn-targets`. It should have been the first thing tried.

### The tables were full, and said nothing (2026-10-16)

This is the answer to why no scan or connection command had ever been seen,
and it was BroadTek's own doing both times.

**First, a wrong diagnosis, corrected by the counters added to chase it.** The
previous note claimed several hundred commands were being dropped unread, from
`broadtek-ctrl-types` showing 743 requests of type `21/00` against
`broadtek-cmds-uc = 251`. The next run reported `broadtek-cmd-nobuf = 0` and
`broadtek-cmd-short = 0`, so nothing was being dropped that way, and the
arithmetic explains itself:

```
cmds-uc 41 + cmds-dev 42 + cmds-pipe 42 = 125 = broadtek-commands
ctrl-types 21/00 = 123                       = 41 x 3
```

Every bluetoothd command passes all three hooked functions, and
`noteControl()` counts it at each. 743 against 251 was a triple-counted total
compared with a single-hook counter. There was never a gap. The `readCommand()`
refactor those counters came with is worth keeping, but the bug it was built
for did not exist.

**The real cause: `kOpcodes` was 32, and `broadtek-opcodes-uc` held exactly
32.** The table was full. `noteOpcode()` returned silently when full, so once
bluetoothd's startup sequence had filled it, every later opcode was discarded
-- and scanning and connecting happen after startup. The commands were never
missing. They could not fit, and nothing said so.

`broadtek-ctrl-types` was full at 12 of 12 the same way, which is why `a1/84`
and `21/01` traded places between runs depending on which arrived first.

That is a silent `return` at a limit, the third time this project has been
misled by one: first `-broadtekrw` compiling `DBGLOG` to nothing, then the
`bmRequestType` filter, now this. The pattern is always the same -- a
condition that discards without recording -- and it has cost more rounds than
every real bug put together.

So the limits are raised (opcodes 32 to 192, request types 12 to 24, event
codes 16 to 64, endpoints and connection targets 8 to 16) and, more
importantly, **a full table now reports itself**:

| Property | Means |
|---|---|
| `broadtek-tables-full` | some table refused an entry. **If this is not 0, no list is complete.** |
| `broadtek-keys-installed` | keys handed down from userspace, or promoted |
| `broadtek-resolving-list` | identities currently held with a key |
| `broadtek-resolved` | advertisements whose RPA matched a stored IRK |
| `broadtek-substituted` | commands where an address was actually replaced |
| `broadtek-conn-events` | LE Connection Complete events seen |
| `broadtek-conn-translated` | ... whose peer address was put back to the identity |
| `broadtek-write-noprepare` | an edit whose descriptor could not be prepared |
| `broadtek-write-short` | an edit the descriptor refused to take (read-only) |
| `broadtek-cmd-rebuffered` | commands forwarded in a descriptor of our own |
| `broadtek-cmd-rebuffer-failed` | edits abandoned for want of a slot or memory |
| `broadtek-pipes-elsewhere` | IN transfers on another device's pipes, not read |

Everything concluded from an opcode list before this fix covered at most the
first 32 distinct opcodes of a boot. That includes §3's reasoning about which
commands bluetoothd does and does not send, which should be re-checked rather
than trusted.

One more consequence of the reading that found this: it was taken within
seconds of login (`broadtek-events = 42`, `broadtek-event-codes = <0e>` --
Command Complete only, no LE Meta at all, `broadtek-acl-in = 0`). The counters
want a minute of settling and an actual Instant Hotspot attempt before they
mean anything.

### The failure, finally observed (2026-10-16)

Build `ceee2f6`, stage `off`, with an actual Instant Hotspot attempt and the
opcode cap lifted. `broadtek-conn-targets`:

```
LE Add Device To Filter Accept List   pub  8c:00:00:00:00:02   My Watch
LE Create Connection                  pub  00:00:00:00:00:00   (ignored: accept-list policy)
LE Add Device To Filter Accept List   pub  64:00:00:00:00:01   My iPhone
LE Remove Device From Accept List     pub  8c:00:00:00:00:02
LE Remove Device From Accept List     pub  64:00:00:00:00:01
```

This is §1's description, directly observed for the first time. macOS puts the
**identity** addresses of the Watch and the iPhone on the Filter Accept List,
with address type `0x00`, public. It then issues LE Create Connection with an
all-zero peer address, meaning the accept list decides who to connect to. Both
peers advertise from resolvable private addresses, nothing matches, and the
removals are the cancellation after about ten seconds. `0x200e` LE Create
Connection Cancel is in the opcode list too.

`broadtek-opcodes-uc` went from 32 opcodes to **61** the moment the cap was
lifted, and all eight LE connection commands appeared at once: `0x2005`,
`0x200b`, `0x200c`, `0x200d`, `0x200e`, `0x2010`, `0x2011`, `0x2012`. They had
been sent all along.

**The address type settles the open question.** It is `pub`, not an RPA. So
bluetoothd does not resolve the peer host-side before asking the controller --
the hoped-for shortcut, where macOS would hand over an already-resolved
address and no key would be needed, is not what happens. BroadTek genuinely
needs an IRK, and §4 step 4 (substitute the current RPA into the accept-list
add) is precisely the right fix, now confirmed against the real command stream
rather than deduced from logs.

### Two measurement faults in the same run

`broadtek-tables-full = 22`: `broadtek-ctrl-types` was full again at 24 of 24,
because this run had more USB devices active than any before. Raised to 48.
The counter did its job -- that is what it is for.

More seriously, `broadtek-acl-in = 21357` was mostly **not Bluetooth**.
`broadtek-endpoints` shows why:

```
81:int:1588  82:bulk:240  02:bulk:171        <- the dongle
83:int:1  87:int:3  85:bulk:1222  86:bulk:19946  88:bulk:81  89:bulk:215
04:bulk:1947  05:bulk:27075  06:bulk:205  07:bulk:257        <- something else
```

`isAclInPipe()` tested only for "bulk IN", which matches that on every USB
device, so about 21100 of those transfers were another device's and each one
was wrapped in a completion and parsed for an SMP header. `isEventPipe()` had
the same flaw, noted in §7 when it was only a theoretical worry; here it was
real, and `broadtek-evt-not-hci = 2` was that device's interrupt endpoint.

The clean discriminator would be the parent device's vendor and product ID.
`IOUSBHostIOSource` keeps exactly that in a `tPacketFilterMetadata`, but the
member is protected with no accessor, so reading it means trusting a struct
offset in a completion path -- the same objection that ruled out
`internalDeviceRequestGated`, and declined for the same reason.

So the endpoint address discriminates instead: `0x81` for events, `0x82` for
ACL IN. The Bluetooth USB transport fixes endpoint types but not addresses, so
this is convention rather than guarantee; it holds for this dongle, and the
other device here uses `0x83`-`0x89` and `0x04`-`0x07`, so they separate
cleanly. `broadtek-pipes-elsewhere` now counts the transfers this test
rejects, which keeps the assumption checkable: if the dongle ever used
different addresses, that counter would be large while the HCI counters sat at
zero.

### Where macOS keeps the LE keys (2026-10-16)

Investigated because, with the devices iCloud-paired and `conn-targets`
confirming macOS asks for identity addresses, reading the key locally is the
only route left to an IRK.

`strings /usr/sbin/bluetoothd` names both stores:

| | |
|---|---|
| `/Library/Bluetooth/com.apple.MobileBluetooth.ledevices.paired.db` | SQLite, the LE paired-device database |
| `...ledevices.other.db` | unpaired devices seen |
| the keychain, access group `com.apple.bluetooth` | where link keys go, local **and** "synced" |

The synced keychain explains `CloudPaired` directly -- bluetoothd logs
`Adding keys %d to synced keychain for device ... tagged as a watch`, which is
iCloud Keychain. That is how a bond arrives on this Mac without an SMP
exchange, and why §3's capture route was never going to see one.

**The database is the promising one.** It is `0640 root:wheel` and, crucially,
carries **no SIP `restricted` flag** (`ls -lO` shows `-` for every file in
`/Library/Bluetooth`). So root can read it with no entitlement and no SIP
argument. It has a 1.7 MB `-wal` alongside it, so anything reading it must be
WAL-aware or accept a stale snapshot.

**The keychain is the fallback and is harder.** bluetoothd holds
`keychain-access-groups = [apple, com.apple.bluetooth]`, an Apple-private
entitlement a third-party binary cannot normally claim. On *this* machine it
might anyway: the boot-args carry `amfi_get_out_of_my_way=0x1` and
`amfi=0x80`, which turn off AMFI entitlement enforcement, so a self-signed
helper claiming that group could plausibly be allowed. That is specific to
this machine's configuration and not something to design around if the
database works.

#### Can a kext read it? No, and it should not try

- The keychain lives in userspace behind securityd. A kext cannot call
  Security.framework at all.
- The database could in principle be read through vnode APIs, but parsing
  SQLite in the kernel -- with a WAL to interpret -- is the wrong place for
  that work by a wide margin.

So the shape is a **userspace helper running as root** that reads the key and
hands it down:

```
helper (root)  --reads-->  ledevices.paired.db  (or the keychain)
helper         --writes-->  BroadTek user client  -->  bt_ctx.rl[]
```

BroadTek has no user client today: its `IOKitPersonalities` matches
`IOResources` with `IOClass` `BroadTek` and exposes no external methods. One
is new work, and it needs care of its own:

- the user client must be restricted to root, or any process could install an
  IRK and have the controller connect to a device of its choosing;
- it must be write-only for keys. Nothing may read one back out, which is the
  same rule §3 already sets for the registry and the log;
- the existing `bt_smp.c` capture path stays useful as the other way in, and
  both should feed the same promote step rather than two.

#### The one thing still unknown

Whether the IRK is actually *in* that database, or whether the database holds
only metadata and every key lives in the keychain. bluetoothd's strings talk
about writing link keys to the keychain, which is suggestive but not decisive;
LE identity keys may be kept differently from classic link keys.

#### Answered: no keys in the database, but something better might be

```sql
CREATE TABLE PairedDevices(Uuid TEXT, Name TEXT, NameOrigin INT, Address TEXT,
    ResolvedAddress TEXT, LastSeenTime INT, LastConnectionTime INT,
    GATTServiceChangeConfig INT, Tags TEXT, iCloudIdentifier TEXT);
CREATE TABLE CustomProperties (Uuid TEXT, JSON TEXT, UNIQUE(Uuid));
```

No IRK column, no key material of any kind. As a source of keys the database
is out, and the keychain is the only one -- with the entitlement problem above.

But `Address` and `ResolvedAddress` are **separate columns**, which raises a
possibility that would make the key question irrelevant. If `ResolvedAddress`
holds a peer's current RPA against its identity `Address`, then macOS is
already keeping the mapping BroadTek needs, and BroadTek does not need to
resolve anything:

- no IRK, from any source: no keychain entitlement, no AMFI dependency, no
  bonding exchange to catch;
- no `ah()` and no AES on this path at all;
- the data is two addresses, neither secret, in a root-readable file with no
  SIP flag.

BroadTek would need only the identity-to-RPA pair, which is exactly what
`bt_rl_entry` already holds (`id_addr` and `rpa`), fed by the same promote
step the capture path would use. The resolving list and `ah()` stay for the
case where BroadTek does have a key and sees a report first; this route simply
skips ahead of them.

**Checked, and it is not that.** Both rows carry the same value in both
columns:

```
My Watch  | Public 8C:00:00:00:00:02 | Public 8C:00:00:00:00:02 |
My iPhone  | Public 64:00:00:00:00:01 | Public 64:00:00:00:00:01 |
```

`ResolvedAddress` means "the identity this resolved to", which for a public
identity address is itself. No private address is recorded anywhere in the
database, and `Tags` is empty for both. The shortcut does not exist: this
database holds neither keys nor the identity-to-RPA mapping.

### Every route to an IRK, and what is left

| Route | Status |
|---|---|
| macOS hands it over via HCI, after faking LL Privacy | **Closed.** Gated on chipset classification, not features (§3) |
| Capture it from a bonding exchange on the ACL link | **Unexercisable.** The devices are `CloudPaired` and `Hidden`; there is no way to make them re-bond, so no exchange ever happens here |
| Read it from `ledevices.paired.db` | **Closed.** No key column, and no RPA either |
| Read it from the keychain, access group `com.apple.bluetooth` | **The only one left** |

The keychain route needs the `keychain-access-groups` entitlement naming
`com.apple.bluetooth`, which is Apple-private and not something a third-party
binary can normally claim. The one reason it is not already closed: this
machine boots with `amfi_get_out_of_my_way=0x1` and `amfi=0x80`, so AMFI
entitlement enforcement is off and a self-signed helper claiming that group
may be allowed. That is a property of this machine's configuration, not of
macOS.

Testing it is small and self-contained: a helper claiming the entitlement,
signed locally, run as root, querying the access group and reporting **how
many items it can see and what shape they are** -- never the key bytes, the
same rule that governs the registry and the log. If `SecItemCopyMatching`
returns the bonded devices' keys, the route is open and the architecture is
the one above: root helper, user client, `bt_ctx.rl[]`. If AMFI still refuses,
or the items come back empty, then **no route to an IRK exists on this stack**,
and that is the answer to write down rather than another thing to try.

Worth keeping in view: everything below the key is finished and proven. The
transport is mapped in both directions, the event rewrites work on hardware
(§3, the features rewrite read back by bluetoothd), the failure is observed
directly, and `src/core` is complete and tested for what to do once an IRK
exists. The project is one input short, and that input has exactly one
remaining source.

### The keychain is readable, and only the Watch is in it (2026-10-16)

`make probe && ./build/out/keychain_probe`. The entitlement was accepted and
the query succeeded -- **the last route is open.** It did not even need root:
AMFI entitlement enforcement is off on this machine, so a locally signed
binary claiming `keychain-access-groups` is honoured, and the item's own
access control allowed the user.

Access group `com.apple.bluetooth` holds exactly one LE bond:

```
acct    Public 8C:00:00:00:00:02      My Watch
svce    BluetoothLESync
v_Data  <254 bytes>  text             starts '<' or '{', so parseable
sync    1
vwht    PCS-MasterKey
agrp    com.apple.bluetooth
```

`sync = 1` with the `PCS-MasterKey` view is `CloudPaired` confirmed a third
way: this bond arrived over iCloud Keychain. The other five items in the group
are `com.apple.systemdefault` keys, nothing to do with Bluetooth.

**The iPhone has no entry.** Asked by exact service name rather than by
listing a group, `BluetoothLESync` in `com.apple.bluetooth` returns the Watch
and nothing else -- yet `conn-targets` shows macOS putting the iPhone's
identity address on the accept list, so it believes it has a key for it
somewhere. Where is unknown.

That inverts which feature to attempt first:

| Target | Key source | Status |
|---|---|---|
| **Apple Watch notifications** | the keychain item above | **a source exists** |
| Instant Hotspot (iPhone) | none found | no key anywhere yet |

The Watch was previously set aside as the harder of the two, on the grounds
that its bond is tangled up with Auto Unlock and re-pairing it looked risky.
That reasoning was about *capturing* a key over the air. Reading one needs no
re-pairing at all, so the Watch is now the one with a reachable key and the
iPhone is the one without.

#### A boundary worth writing down

The entitlement also claims the `apple` access group, and querying it once
returned 119 items: Apple ID authentication tokens and other credentials with
nothing to do with Bluetooth. A tool for finding one device's identity key has
no business enumerating someone's credential store. The probe now queries
`com.apple.bluetooth` only, and anything outside it must be asked for by exact
service name. That is a rule about the tool, not an accident of how it was
written.

The probe prints counts, attribute names and byte *lengths*. It never prints
key material: every `CFData` goes to `CFDataGetLength`, and the only thing
read out of a blob is its container format, which is not a secret. An IRK in a
scrollback buffer would be a worse outcome than never having read it.

#### The field, found

Parsing the blob -- key names and sizes only, no value of any kind printed --
gives a property list with a single entry:

```
dict, 1 entry
  Remote IRK: data, 16 bytes
```

**`Remote IRK`, sixteen bytes.** That is the input this project has been one
short of since it started, and both halves are in the same keychain item: the
identity address is the item's `acct` attribute (`Public 8C:00:00:00:00:02`)
and the key is the plist's only field. Together they are exactly a
`bt_rl_entry`: `id_addr` and `irk`.

So the recipe, for whoever writes the helper:

| | |
|---|---|
| class | `kSecClassGenericPassword` |
| access group | `com.apple.bluetooth` |
| service | `BluetoothLESync` |
| account | `Public <identity address>` |
| synchronizable | must be `kSecAttrSynchronizableAny`, since the bond is iCloud-synced |
| value | a property list whose `Remote IRK` is the 16-byte key |

No byte of it has been printed, and the helper must keep it that way: read it,
pass it down, never render it.

#### What is next, concretely

1. ~~Parse the blob.~~ Done: `Remote IRK`, 16 bytes, recipe above.
2. Give BroadTek a user client to receive one (§3, "Can a kext read it"):
   root-only, write-only for keys, feeding the same promote step
   `bt_smp.c`'s captures would use.
3. Then stage `full`, where the accept-list substitution finally has an IRK to
   work with, and the test is whether a watch notification arrives.
4. Separately, find where the iPhone's key lives, by exact service name. If it
   does not exist locally at all, Instant Hotspot may be out of reach even
   though watch notifications are not.

### Getting the key in (2026-10-16)

Three pieces, of which only the first is proven.

**1. The core takes a key from outside, and the whole chain works.**
`bt_rl_install()` puts an identity and an IRK into the resolving list,
replacing any entry for the same identity so a re-install refreshes rather
than fills it. `bt_promote_captures()` moves finished ACL-side captures the
same way, which keeps capture and use as two separate decisions.

The userspace test runs the project's entire thesis end to end: install a key
that never came from HCI, feed in an advertising report, watch it resolve to
the identity, then send the accept-list add macOS really sends
(`broadtek-conn-targets`, above) and check the address it carries has become
the private one. Also covered: re-installing one identity uses one entry, a
full list refuses rather than overflows, a capture is not used until promoted,
promoting twice does not duplicate, and half a capture does not promote.

**2. `BroadTekConfig`, a property write rather than a user client.**
`setProperties` on a second personality, root-only, with the key never stored
as a property -- publishing it would put a cryptographic key in the IO
registry, which any process can read.

A single one-way message needs no session, no external-method dispatch and no
user-client class in the Info.plist, and `setProperties` is write-only by
nature: data in, status out, and no call exists that could return a key. The
properties it accepts:

| Property | Value |
|---|---|
| `broadtek-install-irk` | 23 bytes: address type (1), identity address (6), IRK (16), all least significant byte first |
| `broadtek-promote-captures` | true: promote whatever `bt_smp.c` has captured |

and two new counters, `broadtek-keys-installed` and
`broadtek-resolving-list`, which report numbers and never key material.

**3. `tools/irk_install.c`.** Runs the §3 query, parses `Remote IRK` out of
the plist, converts the account string to an address in HCI order, and writes
the blob. Run as non-root it already gets as far as the kext:

```
1 bond(s) in the keychain
  8c:00:00:00:00:02 (public): BroadTekConfig not found: is the kext loaded?
```

So the read, the parse and the address conversion all work. Only the handover
is untested, because the kext carrying `BroadTekConfig` has not been loaded
yet. The key is never printed: `printf` only ever sees the address and its
type, and the key bytes are wiped from the stack after use.

#### Two things to know before trusting it

**Byte order is the one open question, and it could not be settled by
reading.** SMP sends an IRK least significant octet first, which is the order
`ah()` wants, so the capture path needs no conversion. Whether the keychain
stores it the same way is unknown. The helper installs it as stored and takes
`--reverse-irk` to install it the other way. A wrong order is harmless -- the
key simply never matches any advertisement -- so the test is whether reports
start resolving, and if they do not, the other order is the first thing to
try.

**The key lives only in kernel memory.** Nothing is written to disk, which is
deliberate, so `irk_install` has to be run again after every restart. A
LaunchDaemon would do it unattended, and that is worth having only once the
handover is known to work.

#### The handover works (2026-10-16)

Build `899b718`, stage `full`. `irk_install` reported `installed`, and the
registry agrees: `broadtek-keys-installed = 1`,
`broadtek-resolving-list = 1`, `broadtek-rewriting = "full"`,
`broadtek-tables-full = 0`. So the last untested link -- userspace reading a
key out of the keychain and `BroadTekConfig` accepting it -- is closed.
`BroadTekConfig` matched, started and registered as intended, and the
privilege check let a `sudo` caller through.

What is **not** yet known is whether the key is any good.
`broadtek-conn-targets` still read
`fal+/pub/8c:00:00:00:00:02 conn/pub/00:00:00:00:00:00 fal-/pub/...` -- the
identity address, unsubstituted. Three different things produce that, and
nothing published could tell them apart:

1. the entries were recorded before `irk_install` ran, and no accept-list add
   has happened since;
2. the Watch has not advertised, so there is no RPA to substitute;
3. the IRK is stored in the opposite byte order, matches nothing, and never
   will.

That is the same shape of blindness as every other round in §7: a zero that
could mean three things. So two counters now exist, in the core where the
logic is:

| Property | Means |
|---|---|
| `broadtek-resolved` | advertisements whose RPA matched a stored IRK |
| `broadtek-substituted` | commands where an address was actually replaced |

`resolved` is the one that settles byte order, and it is counted on a match
regardless of whether the report is rewritten -- the match is what says the
key is right. `substituted` then says whether the accept-list edit followed.

| resolved | substituted | Reading |
|---|---|---|
| 0 | 0 | the key matches nothing: try `--reverse-irk` |
| >0 | 0 | the key is right; no accept-list add has happened since, so trigger one |
| >0 | >0 | **the chain works end to end** |

The tests cover both counters, including that a deliberately corrupted key
resolves nothing, substitutes nothing, and leaves the identity address in
place.

One thing the registry already shows about resolution: `res_enabled` will
never be set, because it is set by `LE Set Address Resolution Enable` and
macOS never sends the resolving-list commands (§3). That does not matter.
`resolve_addr()` records the sighting before testing `res_enabled`, so the
identity-to-RPA mapping is learned either way, and the substitution only
needs the mapping. The report itself going out unrewritten is harmless --
macOS was not expecting resolution anyway.

#### How to try it

```bash
make kext irk-install
sudo efi/install.sh --stage full <uuid>     # the substitution needs commands
# restart
sudo build/out/irk_install
ioreg -l | grep '"broadtek-'
```

`broadtek-keys-installed = 1` and `broadtek-resolving-list = 1` mean the key
is in -- both confirmed on hardware. Then **trigger a watch approval** and
read `broadtek-resolved` and `broadtek-substituted` against the table above
before anything else, because they say which of three situations you are in.
The Watch is the device to use: it is the one whose key is in the keychain.

Stage `full` is needed because the accept-list substitution edits a command,
which also exercises the one write that has never run on this hardware: into
a control transfer's memory descriptor. If an address never changes, suspect
that write before suspecting the key.

### The key is right; the edit may not be reaching the controller (2026-10-16)

Build `9ba83fe`, stage `full`, key installed, a watch approval triggered. The
Mac said "check your watch" and nothing arrived -- the §1 symptom, unchanged.
But two counters moved, and they rule a great deal out:

| | |
|---|---|
| `broadtek-resolved` | **24** |
| `broadtek-substituted` | **12** |

**`resolved = 24` settles the byte order.** The keychain stores `Remote IRK`
in the same order `ah()` wants, so no conversion and no `--reverse-irk`: 24
advertisements from the Watch were matched against the key read out of the
keychain. Everything from §3 onwards -- the keychain query, the plist parse,
the address conversion, `BroadTekConfig`, `bt_rl_install`, `ah()` -- is
working on real traffic from a real device.

`substituted = 12` says the core replaced an address in twelve commands.

`broadtek-conn-targets` still reading `fal+/pub/8c:00:00:00:00:02` is not a
contradiction: `noteConnTarget()` records the address as it *arrives*, before
the edit, which is what makes it a record of what macOS asked for.

#### What `substituted` does not prove

It counts the core editing **BroadTek's own stack copy** of the command. What
the controller receives depends on `writeMD()` putting that copy back into the
user client's memory descriptor -- and that is the one operation in the plugin
that had never run on hardware. Observe-only never reached it, and stage
`events` does not use it either.

The old `writeMD()` ignored both ways it can fail:

```c
if (md->prepare(kIODirectionNone) != kIOReturnSuccess) return;   /* silent */
md->writeBytes(0, buf, len);                                     /* result discarded */
```

A buffer belonging to a user client may well not be writable from a hook, and
if it is not, the command goes to the controller with the identity address
still in it -- which produces exactly the observed symptom, with
`substituted` climbing all the while. That is the fourth silent failure of
this shape in this project, and the previous note already said to suspect this
write before suspecting the key. It is now instrumented rather than suspected:

| Property | Means |
|---|---|
| `broadtek-write-fail` | `prepare()` failed, or fewer bytes were written than asked |
| `broadtek-write-unverified` | the write returned success and a read-back disagreed |

The write is read back and compared -- the first 32 bytes, which covers every
address substituted and keeps the buffer inside a completion routine's stack
budget.

| write-fail | write-unverified | Reading |
|---|---|---|
| 0 | 0 | the edit reached the device, and the problem is further on |
| >0 | 0 | the descriptor cannot be prepared or written from here |
| 0 | >0 | the write is accepted and discarded: the buffer is a copy, or read-only |

The last two both mean the same thing for the design: a command cannot be
edited in place on this path, and the substitution needs another mechanism --
which would be a serious finding, since §5a's no-injection design rests on
being able to edit commands in flight.

If instead both are 0, the edit is landing and the next things to look at are
whether the substituted RPA was still current when the controller used it
(they rotate about every 15 minutes, §8), and whether the connection-complete
translation fires -- there is no counter for that yet.

### Why the edit never arrived, and the fix (2026-10-16)

Build `ada47b3`, stage `full`, key installed, watch approval triggered. The
prompt still did not arrive, and the new counters named the reason:

| | |
|---|---|
| `broadtek-resolved` | 17 |
| `broadtek-substituted` | 6 |
| **`broadtek-write-fail`** | **510** |
| `broadtek-write-unverified` | 0 |

So the write is **refused**, not accepted-and-discarded. `writeMD()` could not
put the edited command back into the caller's descriptor, 510 times, while the
core went on reporting substitutions it had made to its own copy.

**The mechanism.** A command is an OUT transfer. `prepare()` on an outbound
descriptor marks it `kIOMemoryPreparedReadOnly`, and `writeBytes()` then
refuses by design. That is not a bug to work around in `writeMD`; it is what
the API is for.

And it explains the asymmetry that had been sitting in plain sight the whole
time: **the event rewrites worked** -- bluetoothd logged the features byte as
`FD` where the controller sends `BD` (§3) -- because an event descriptor is
inbound and writable. Commands were never going to work the same way. Stage
`events` succeeded and stage `full` could not, for a reason with nothing to do
with keys or addresses.

**The fix: stop editing someone else's buffer.** The hook owns the arguments
it forwards, so it can forward a descriptor of its own:

```
host's command  ->  hook reads it, edits a copy, forwards OUR descriptor
                    (the host's buffer is never touched, which is correct
                     for an OUT transfer: nothing reads it back)
```

This is still not injection, and §5a's design holds: the same command the host
sent, going where it was going, with one address changed. Only the container
is BroadTek's.

Lifetime is the part to get right, and the conservative choice is taken
throughout:

- a **synchronous** call is finished with the buffer when it returns, so it is
  released there;
- an **asynchronous** one must outlive the call, so it borrows a completion
  slot and the trampoline releases it when the transfer completes;
- if **no slot is free**, the substitution is abandoned and the host's
  original buffer goes through unedited. A command that misses its rewrite is
  a missed improvement; a buffer freed under a live transfer is a panic.

A digest of the command is taken before and after the core runs, so a
descriptor is only substituted when something actually changed -- any change,
not just an address swap.

New counters, and the old one split so the mechanism is named rather than
inferred:

| Property | Means |
|---|---|
| `broadtek-write-noprepare` | `prepare()` refused |
| `broadtek-write-short` | `prepare()` worked and `writeBytes` refused -- the read-only case above |
| `broadtek-cmd-rebuffered` | commands forwarded in a descriptor of BroadTek's own |
| `broadtek-cmd-rebuffer-failed` | an edit abandoned: no slot free, or no memory |

What to expect next run: `cmd-rebuffered` climbing, `write-short` still
climbing (the other hooks still attempt the old write and still fail
harmlessly), and the real question being whether a watch prompt arrives.

`IOBufferMemoryDescriptor::withBytes` is exported by the kernel and by the
boot collection, under a KPI the kext already declares, so it resolves at
load -- checked, because an unresolved symbol would stop the kext loading at
all.

### The substituted address reaches the controller (2026-10-16)

Build `4525f75`, stage `full`, key installed, watch approval triggered. The
prompt still did not arrive, but the substitution demonstrably works now:

```
broadtek-conn-targets = "... fal+/pub/8c:00:00:00:00:02 ...
                         fal+/RPA/5a:00:00:00:00:04
                         fal-/RPA/5a:00:00:00:00:04"
```

`0x5a` is `01` in its top two bits: a resolvable private address, on the
Filter Accept List. `broadtek-cmd-rebuffered = 2`,
`broadtek-cmd-rebuffer-failed = 0`.

Why that RPA entry is proof rather than a restatement: `noteConnTarget()`
records an address *before* the core edits it, so the `pub` entries are what
macOS asked for. The `RPA` entry was recorded by a **downstream** hook --
`IOUSBHostDevice::internalDeviceRequest`, below the user client -- seeing
BroadTek's substituted buffer on its way to the device. The edit genuinely
propagates now.

`broadtek-write-short = 698` is the other hooks still attempting the old
in-place write and still failing, which is expected and harmless: they are
downstream of the substitution, so the command they see is already correct.

So the chain is: key read from the keychain, 9 advertisements resolved with
it, the accept-list add rewritten to the current private address, and that
rewrite reaching the controller. Everything §4 describes up to step 4 works
on hardware.

#### The last link has no counter yet

Step 5 is the connection-complete translation: when a link forms against a
private address, the host has to be told it connected to the identity it
asked for, or it does not recognise the device. Nothing published says
whether that ever happens, and `fal-/RPA/...` only says the attempt ended.

Two counters, in the core:

| Property | Means |
|---|---|
| `broadtek-conn-events` | LE Connection Complete seen, plain or enhanced |
| `broadtek-conn-translated` | ... whose peer address was put back to the identity |

| conn-events | conn-translated | Reading |
|---|---|---|
| 0 | 0 | **no link ever formed.** The accept list is right, so the question moves to whether the Watch advertises *connectably* at all -- the advertisements being resolved may be non-connectable NearbyInfo broadcasts, which an accept-list connection can never latch onto |
| >0 | 0 | a link formed and the peer address was not put back: macOS sees a device it did not ask for |
| >0 | >0 | the whole of §4 works, and the prompt not arriving is above Bluetooth |

The first row is the one to expect, and it would point somewhere specific
rather than at BroadTek: a stale RPA (they rotate about every 15 minutes, and
nothing re-adds the accept-list entry when they do -- §8), or a peer that is
simply not offering connectable advertising when macOS tries.

### It works. The LE link establishes (2026-10-16)

Build `0ee360c`, stage `full`, the Watch's key installed from the keychain.
`broadtek-conn-events = 1`, `broadtek-conn-translated = 1` -- and macOS says
the rest in its own words:

```
Server.LE.Connection  Outgoing LE Connection complete for handle ...
                      address Public 8C:00:00:00:00:02 with status 0
Server.LE.Connection  Outgoing connection to device "My Watch"
                      ... was successful with connection interval 24 active=1
Server.GATT           Gatt device connected on transport 2 ... with status 0
Server.LE.Security    Encryption is now enabled for device "My Watch"
Stack.TimeLine.Device Le topology - ... state: CONNECTED, enc-state: ON
```

**The failure this project exists to fix is fixed.** §1 described macOS storing
an IRK, putting the identity address on the accept list, and nothing ever
matching. There is now an encrypted, GATT-ready LE link to the Apple Watch,
and macOS sees it at `Public 8C:00:00:00:00:02`, the identity it asked for.
Every step of §4 is confirmed on hardware:

| Step | |
|---|---|
| 1. Report LL Privacy | works, and turned out to be unnecessary (§3) |
| 2. Hold the IRK | from the keychain, not from HCI |
| 3. Resolve RPAs in reports | 13 resolved with the keychain's key |
| 4. Substitute the RPA on the accept list | `fal+/RPA/7c:00:00:00:00:06`, reaching the controller |
| 5. Put the identity back in Connection Complete | 1 translated; macOS logs the identity |

The whole of `src/core` is now exercised by real traffic, and the transport
layer under it is proven in both directions.

### What still does not happen, and why it is not Bluetooth

The approval prompt does not arrive on the watch. The reason is in sharingd's
log, not bluetoothd's:

```
<SFAutoUnlockDevice: ... name:My Watch,
                     unlockEnabled:YES canUnlockDevice:NO>
```

Auto Unlock is enabled for the Watch and macOS has decided it **cannot**
unlock, which is a judgement made above the Bluetooth link -- the link is up
and encrypted while that line is being printed.

The reason was found by static analysis (§8, "What sets `canUnlockDevice`"),
and it is **not** AWDL and **not** a hardware wall. `sharingd` sets
`canUnlockDevice` from `-[SDAutoUnlockAKSManager
deviceEnabledAsKeyForIDSDeviceID:]`, which for a watch returns YES only if an
escrow secret or a remote LTK is stored for it. There is none: the Watch has
never been **enrolled as an unlock key** on this Mac. The earlier guesses --
AWDL ranging, a board-id allow-list, an absent Secure Enclave -- are all ruled
out (`SFDeviceSupportsAutoUnlock` is unconditionally true, this MacBookPro16,2
has a T2/SEP, and AWDL is not consulted in this path). The fix is enrollment,
not driver work: turn on the Apple Watch unlock toggle in System Settings,
which itself needs the LE link BroadTek now provides. `awdl0` exists and
reports `status: inactive`, but that governs completing an unlock, not the
prompt.

So the §9 pass criterion for test case 2 cannot be met on this machine, for a
reason outside BroadTek's scope. That should be recorded as the answer rather
than chased here.

### Instant Hotspot is the better demonstration now

Instant Hotspot needs the LE link, which works, and does not need ranging. It
is blocked by one thing only: **the iPhone's IRK is not in the keychain where
the Watch's is.** `BluetoothLESync` in `com.apple.bluetooth` holds exactly one
item, for the Watch, which makes sense -- that group is where an Auto Unlock
device's key goes.

**Looked up, and it is not there.** By exact account, in both groups:

```
Public 64:00:00:00:00:01  in com.apple.bluetooth   no match
Public 64:00:00:00:00:01  in apple                 no match
Public 8C:00:00:00:00:02  in com.apple.bluetooth   1 item, Remote IRK, 16 bytes
```

A note on reading those: a query that matches nothing returns `errSecParam`
(-50) on this system rather than `errSecItemNotFound`. The control is that
`BluetoothGlobal` in the *same valid group* also returns -50 while
`BluetoothLESync` returns success, so -50 here means no match and not a
malformed query.

Combined with the earlier enumeration of `com.apple.bluetooth` -- one generic
password, for the Watch, plus five `com.apple.systemdefault` keys and some
certificates -- **the iPhone's LE key is not in the Bluetooth keychain at
all**, and a targeted lookup in `apple` does not find it either.

So Instant Hotspot has no reachable key, and therefore no route, unless the
phone's bond is held somewhere not yet identified. Worth noting what macOS
does anyway: it puts the iPhone's identity address on the accept list
(`conn-targets`), which it would do whether or not it holds a key for it. The
accept-list add is not evidence that a key exists.

### Hunting the iPhone's key (2026-10-16)

Searched by **exact account** across seven access groups --
`com.apple.bluetooth`, `com.apple.rapport`, `com.apple.sharing`,
`com.apple.identityservices`, `com.apple.coreduetd`, `com.apple.wifi`,
`apple` -- and under five identifiers: the address as the Watch's entry
spells it, bare and lower-case, the phone's `CBDevice` UUID, and its IDS
identifier. No group was ever listed; each query names one account and can
match only that device.

One hit:

```
00000000-0000-0000-0000-000000000003  in com.apple.rapport
    service RPIdentity-SameAccountDevice   61 bytes, opaque
```

That service holds **nine** same-account device identities, all exactly 61
bytes -- the iPhone, the Watch, and seven others. A fixed-format blob, not a
property list.

**It is not an IRK, and the Watch proves it.** The Watch has *both* a Rapport
identity and the `BluetoothLESync` bond whose `Remote IRK` demonstrably
resolves its advertisements. So the question "do Rapport identities contain
Bluetooth IRKs" has a control: does the Watch's known key appear anywhere
inside its own 61-byte Rapport blob?

```
Watch IRK inside its 61-byte Rapport identity: no
```

Checked at every offset, forwards and byte-reversed, printing neither input.
Rapport keeps device identity keys for Continuity; Bluetooth IRKs live in
`BluetoothLESync`, and the iPhone has no entry there.

So **the iPhone's IRK is not stored on this Mac** anywhere that has been
looked, and the places looked now include every group a bond could plausibly
sit in. The likely reason is that the phone never needed one: macOS resolves
it host-side from the NearbyInfo address token, which the log shows it doing
(`Identities resolve added: Addr 6431358b6666, AT 9ec024, ... matches 1`), and
an accept-list add for its identity is not evidence of a key (§3).

#### The route that needs no key at all

If macOS already knows the phone's current private address, BroadTek does not
need to derive it -- it needs to be told it. `bt_rl_entry` holds `id_addr` and
`rpa` as separate fields, and `substitute()` only reads `have_rpa`: a mapping
handed down from userspace would work exactly as a resolved one does, with no
IRK, no `ah()` and no key handling anywhere.

Where such a mapping might be recorded, unopened so far:

```bash
sudo sqlite3 'file:/Library/Bluetooth/com.apple.MobileBluetooth.ledevices.other.db?mode=ro' .schema
```

`ledevices.other.db` is the companion to the paired database -- 110 KB with a
2 MB write-ahead log, so it is written constantly, which is what a record of
devices currently being seen would look like. If it carries a private address
against an identity, the iPhone becomes reachable through a channel that is
already built: `broadtek-install-irk` would simply gain a sibling that
installs a mapping instead of a key, and everything downstream is proven.

**Opened, and it holds no resolutions.** Same schema as the paired table, and
382 rows of sightings:

```
382 rows
--- rows where the two differ (a resolution) ---     (none)
--- anything mentioning the iPhone ---               (none)
```

`ResolvedAddress` is only ever a copy of `Address` here too, so macOS does not
persist its host-side resolutions, and the iPhone is not in this table at all
(it is in `PairedDevices`). Among 382 rows, had resolutions been written, some
would have shown. The `mode=ro` read may miss the newest commits in the 2 MB
WAL, but not every one of 382 rows.

So there is no key for the phone and no stored mapping either.

#### The one route left for the iPhone, and why it is unattractive

macOS resolves the phone in memory and logs the result:

```
Identities resolve added: Addr 6431358b6666, AT 9ec024, ... matches 1
```

That is identity ↔ **NearbyInfo address token**. The token also travels in the
advertisement itself, beside the private address, where BroadTek already sees
it (`nbIAT <9e c0 24>` in the same log lines, from the raw report BroadTek
reads). So:

```
log:            identity 64:00:00:00:00:01  <->  AT 9e c0 24
advertisement:  RPA 7c:40:...              <->  AT 9e c0 24
                therefore identity <-> RPA, with no key at all
```

Mechanically sound, and it needs nothing Apple has not already published to a
log. The objections are what matter:

- **The token rotates with the address.** Later in the same session the phone
  advertised `nbIAT <f9 b5 0a>`. A mapping learned once goes stale, so a
  helper would have to *tail* the unified log continuously and push fresh
  pairs down.
- **It makes the driver depend on log text** -- a format Apple owns, with
  fields that are `<private>` in some builds and not others. That is a far
  weaker foundation than `ah()` over a stored key, which is a documented
  calculation on a documented input.
- It would also need a new install path for mappings and the token matching in
  `bt_hci.c`, where today nothing parses advertising data payloads.

It is a real route, not a dead end, and it is the only one left for the phone.
Whether a driver that works by reading the operating system's log output is
worth building is a judgement rather than a technical question.

### Where the project actually stands (2026-10-16)

**The objective is met.** BroadTek does the thing it was built to do, verified
against macOS's own log: a controller with no LL Privacy now opens encrypted
LE connections to a device advertising from a resolvable private address, and
macOS sees the identity address it asked for. Every step of §4 is confirmed on
real traffic, `src/core` is complete and tested, and the transport is mapped
and proven in both directions.

**Neither user-facing feature completes, and neither reason is BroadTek's.**

| Feature | State |
|---|---|
| Apple Watch approval | BroadTek's part is done and proven on hardware (§8, 2026-10-17): the Watch was enrolled over BroadTek's link, `canUnlockDevice` flipped to YES, and the unlock session connects + encrypts to the Watch's identity address. The prompt still does not appear, blocked above Bluetooth on AWDL ranging (`withinRange:false`, `awdl0` inactive) -- AirPort_RTW89's job, not BroadTek's |
| Instant Hotspot | needs no ranging, so the link would be enough; but the iPhone's IRK is not in the keychain, so there is no key to resolve with |

That is an honest finish rather than a failure. The hard, uncertain part --
can a host-side driver make macOS resolve private addresses on hardware that
cannot -- is answered yes, and demonstrated. What remains are two external
dependencies, one in the Wi-Fi driver and one in where Apple chooses to store
a key.

#### The daemon (2026-10-16)

The key lives only in kernel memory, deliberately, so it has to be put back
after every boot. That is now automatic:

```bash
make irk-install
sudo tools/install_daemon.sh          # and `remove` to take it out
```

It copies the helper to `/usr/local/libexec/broadtek-irk-install` -- so the
daemon does not depend on the build directory surviving -- **re-signs it
there**, because the keychain entitlement travels with the signature rather
than the file, and bootstraps
`/Library/LaunchDaemons/org.franciumos.broadtek.irk.plist`.

A daemon and not an agent, because the kext takes a key only from root. That
check is confirmed working on hardware: run as the user, the install comes
back `0xe00002c1`, `kIOReturnNotPrivileged`.

`--wait` exists for it. Two things can be missing in the first seconds after
boot -- the keychain may not be readable yet and `BroadTekConfig` may not have
registered -- so the helper retries every 5 seconds for 5 minutes and exits as
soon as a key is in. Bounded, because a daemon that retries forever is one
nobody notices has failed. Its log is `/var/log/broadtek-irk.log` and holds
addresses and statuses only; no key is ever printed.

#### What sets `canUnlockDevice` (answered, 2026-10-17)

Static analysis of `sharingd` and `Sharing.framework`, no reboot. The result
is the good one: **none of the three feared gates is the cause.** It is an
**enrollment** state, fixable without any driver work.

The trail:

- `SFAutoUnlockDevice.canUnlockDevice` is a plain ivar accessor. The value is
  computed in `sharingd` where each device record is built, inside
  `-[... onqueue_autoUnlockEligibleDevices:deviceType:]` (imp `0x1000349de`).
  The setter call at `0x1000363b3` takes its BOOL straight from
  `[[SDAutoUnlockAKSManager sharedManager]
  deviceEnabledAsKeyForIDSDeviceID:device.uniqueIDOverride]`.
- `-[SDAutoUnlockAKSManager deviceEnabledAsKeyForIDSDeviceID:]` (imp
  `0x1000c7023`) is short and decisive:
  1. `if ([self escrowSecretExistsForDevice:id])` → **YES**.
  2. else if `deviceTypeForDeviceID:id == 2` (a watch):
     `return [self remoteLTKForDeviceID:id] != nil`.
  3. else → **NO**.
  So for the watch, `canUnlockDevice` is YES **iff** an escrow secret or a
  remote long-term key (LTK) is stored for it. Nothing else is consulted: no
  board-id, no model string, no Secure Enclave probe, no AWDL/ranging check.
- The machine-capability gate is open anyway: `+[SFAutoUnlockManager
  autoUnlockSupported]` tail-calls `SFDeviceSupportsAutoUnlock`, which is
  `mov eax, 1; ret` -- unconditional. macOS already considers this Mac
  Auto-Unlock-capable.
- And the Secure Enclave is present, contrary to the earlier guess: this is a
  **MacBookPro16,2** (2020 13-inch Intel, T2). `AppleSEPManager` and
  `AppleKeyStore` are both in the registry. SEP was never the problem.

So `canUnlockDevice:NO` means exactly one thing: **the Watch has never been
enrolled as an unlock key on this Mac, so no LTK/escrow secret exists for it.**
The feared board-id/model allow-list and the SEP requirement are both ruled
out, and the AWDL check is not in this path at all.

How to decide `canUnlockDevice`, concretely: enroll the Watch as an unlock
key. In System Settings -> Touch ID & Password, turn on **"Apple Watch"**
(use your Apple Watch to unlock apps and your Mac). That enrollment stores the
LTK, after which `deviceEnabledAsKeyForIDSDeviceID:` returns YES and the device
record comes back `canUnlockDevice:YES`.

The one place this still touches BroadTek: enrollment performs a BLE handshake
with the Watch, and the Watch advertises from an RPA -- the very link that only
comes up now that BroadTek resolves it. So enrollment may have been
unreachable before for the same reason the prompt was, and is worth retrying
with the key installed and stage `full`. Ranging over AWDL is a separate
matter: it is not in the `canUnlockDevice` path, so it is not what blocks the
prompt. It governs *completing* an unlock, which the user already treats as
out of scope (a prompt that arrives but does not finish the unlock is still a
pass).

#### The enrollment and the unlock attempt, on hardware (2026-10-17)

All of the above was then confirmed on hardware, and the picture is complete.

Enrollment: turning on the Apple Watch unlock toggle in System Settings
succeeded over BroadTek's LE link. `sharingd` logged the whole exchange --
`Received SDUnlockSetupCreateRecord`, `Saved watch ID to pairing record`,
`Storing Session Key`, `Added keychain item`, `Enabled for device` -- and the
setup metric came back `"success" : true`, `errorCode 0`,
`macNewLTKSyncStatus 3`, `watchNewLTKSyncStatus 3`, `deviceModel "Watch7,13"`.
The LTK the eligibility check looks for is now stored. This is the step that
had never completed before, and it completed only because the LE link to the
Watch's RPA now comes up.

The gate cleared: a subsequent approval attempt (`sudo`) ran straight past
eligibility -- `AutoUnlocking: no -> yes`, `Creating BLE connection for
unlock`, `Sending SDAutoUnlockAuthPromptRequest`. It reaches that code only
when `canUnlockDevice` is YES. So enrollment flipped it, exactly as the static
read predicted.

The LE link is perfect, and BroadTek is the reason. bluetoothd's own log for
the unlock session:

```
Added to Filter Accept List ... Public 8C:00:00:00:00:02 status:0
Outgoing LE Connection complete ... Public 8C:00:00:00:00:02 with status 0
Gatt device connected on transport 2 ... status 0
Encryption is now enabled for device "My Watch"
```

The proof it is BroadTek's doing: the connection *completed to the identity
address* `8C:00:00:00:00:02`, which the Watch never advertises -- it only
advertises as an RPA. A controller without privacy could not complete a
Filter-Accept-List connection to that identity. It completed because BroadTek
substituted the live RPA (`43:00:00:00:00:07`) into the accept-list command and
translated the connection-complete back (`conn-translated` climbed with
`conn-events`, one-for-one). bluetoothd sees the identity address throughout
*because* the translation is correct. This is the project's objective, met and
visible in macOS's own logs.

What still blocks the prompt is above Bluetooth, and it is ranging. Five
seconds after the encrypted link is up, `sharingd` tears it down
(`WirelessProximity Disconnect ... process sharingd ... subscribed
characteristics: (null)`), and the attempt metric reads `withinRange : false`
with `awdlBringUpTime : 0` and `awdlRangingTime : 0` -- ranging was never
brought up. The NearbyInfo records for the attempt carry the `Ranging` device
flag explicitly (`DF 0x30 < PeerMe Ranging >`, `DF 0x20 < Ranging >`,
`Ranging key exists`), and the flow launches a `WirelessProximity` scan with
`range: 0`. The Watch shows nothing because the proximity/ranging handshake
never completes, and it never completes because `awdl0` is inactive and
AirPort_RTW89 does not implement ranging. The earlier doubt -- whether ranging
gates the prompt itself or only the completion -- is settled by this: the
prompt flow runs over a working link and still stalls on ranging.

So the boundary first drawn in §2 is now proven, not inferred. BroadTek's part
-- the LE link to an RPA-advertising Apple device -- is finished and correct.
The remaining gap is AWDL ranging, which is AirPort_RTW89's job.

One small thing worth a glance if the prompt is ever chased further: `sharingd`
consults an `AUEnableAuthPrompts` default (and `AUAllowOldWatchAuthPrompts`)
and found no value for either, so it used the built-ins. If a future build
wants to force the on-watch *prompt* rather than silent proximity unlock, that
is the preference to look at -- but it does not change the ranging dependency.

#### If the work continues
1. **AWDL ranging in AirPort_RTW89** is the single remaining blocker for the
   Watch prompt, and it is now on the critical path, not a nice-to-have: the
   approval flow runs over BroadTek's link and stalls waiting on ranging. This
   is the other session's work and is far larger than anything here. Once
   `awdl0` can range, retry the approval -- nothing on the BroadTek side needs
   to change.
2. **The iPhone's key**, if Instant Hotspot is the goal. It is not where the
   Watch's is. Anything further means looking outside the Bluetooth access
   group, which §3 rules out by listing and allows only by exact query.

### Where this leaves the approach

What is solid, and stays solid whatever happens next: the transport is fully
mapped, and both directions are proven editable on hardware. Events are read
and rewritten (§7, sixth and eighth runs), commands are seen (`cmds-uc = 182`
after the filter fix), and `src/core` is unchanged and tested. None of that
work is wasted by this finding.

What is blocked: BroadTek cannot get an IRK. Without one it cannot run `ah()`,
so it cannot resolve anything, and steps 2 through 5 have nothing to work on.
The options, in the order they seem worth trying:

1. **Change the classification instead of the features.** If bluetoothd
   believed this were a supported chipset it would send the IRK VSC, and
   BroadTek could take the IRKs straight out of it -- the original plan, whose
   blocker is now identified rather than mysterious. The open question is what
   drives the classification. It is not the HCI manufacturer: that already
   reads as Apple (`VID: 0x004C`) and the dongle is still
   `THIRD_PARTY_DONGLE`. That points at the USB vendor and product ID or a
   hardcoded list, and `13D3:3571` is also what `RealtekBluetoothFirmware`
   matches on to load the firmware, so spoofing it in the descriptors would
   break the firmware load. Needs investigation before it is worth any code.
2. **Get the IRK from outside HCI.** If the IRK for a paired device can be
   read from the system's own pairing store and handed to the kext, macOS
   never needs to cooperate: BroadTek resolves the RPA, substitutes the
   address on the accept list, and translates the connection complete, all of
   which the transport now demonstrably supports. The IRKs are not in
   `/Library/Preferences/com.apple.Bluetooth.plist` (217 bytes, no pairing
   data) or any obvious plist; on Sequoia they are keychain-protected, so this
   needs deciding with the user before going near it.
3. **Accept the limit.** If neither route opens, Instant Hotspot and watch
   approval cannot be delivered on this stack, and that should be written down
   as the answer rather than left as an open item.

Until one of those opens, stage `events` has no benefit: it makes bluetoothd
believe something untrue and buys nothing. Go back to `--stage off`, or
uninstall.

### Also established### Also established

- bluetoothd is the HCI host. It finds the dongle itself (`[bm3_usb]` lines),
  opens its USB interface through `AppleUSBHostInterfaceUserClient`, and does
  HCI from userspace. IOBluetoothFamily is **not** in the HCI path.
- The unified log carries **no kernel lines at all** on this machine
  (`log show --last boot --predicate 'processID == 0'` → 0 lines). Kernel
  output must be read with `sudo dmesg`.

## 4. The approach

Since the Broadcom route is closed, use the standard one: make the controller
look like it has LL Privacy, and do the work in the host.

1. Set LL Privacy (bit 6) in the Command Complete for LE Read Local Supported
   Features, and the resolving-list bits in Read Local Supported Commands, so
   bluetoothd hands the IRKs over with standard HCI commands.
2. Answer the resolving-list commands locally and keep their state:
   `0x2027`–`0x202E` and `0x204E`.
3. Resolve RPAs in LE Advertising Report (`0x02`) and LE Extended Advertising
   Report (`0x0D`) with `ah()` (AES-128), and report the identity address with
   type `0x02`/`0x03`.
4. When bluetoothd puts an identity address with a known IRK on the Filter
   Accept List, rewrite that address to the device's current RPA in flight.
   Same for a direct LE (Extended) Create Connection.
5. In LE (Enhanced) Connection Complete, put the identity address back so
   macOS sees the device it asked for.

## 5. Code map

| Path | What | Status |
|---|---|---|
| `src/core/bt_core.h` | the core interface, HCI byte order throughout | done |
| `src/core/bt_aes.c` | AES-128 and `bt_rpa_matches()` (the `ah()` check) | done, tested |
| `src/core/bt_hci.c` | resolving list, command and event rewriting | done, tested |
| `src/core/bt_smp.c` | reads an IRK out of a bonding exchange on the ACL link | done, tested, **unseen on hardware** |
| `tools/keychain_probe.c` | diagnostic: is bluetoothd's keychain readable here | done, run, answered yes |
| `tools/irk_install.c` | reads the IRK from the keychain and hands it to the kext | done, working on hardware |
| `tools/org.franciumos.broadtek.irk.plist` | LaunchDaemon: puts the key back after each boot | done |
| `tools/install_daemon.sh` | installs that daemon | done |
| `src/plugin/kern_config.cpp` | `BroadTekConfig`: the one way a key gets in | builds, **unloaded** |
| `tests/hosttest.c` | userspace test of the above | passing |
| `tests/configure_test.py` | test of the config.plist editing | passing |
| `src/plugin/kern_broadtek.cpp` | the Lilu plugin: hooks and logging | **builds, unverified on hardware** |
| `src/plugin/kern_start.cpp` | Lilu plugin entry point and boot-args | done |
| `efi/install.sh`, `uninstall.sh`, `configure.py` | EFI install, modelled on AirPort_RTW89's | done, tested against a synthetic config |

`src/core` is freestanding C with no kernel or libc dependency, so the same
objects build into the kext and the userspace test. Keep it that way: it is the
only part that can be tested without rebooting.

### 5a. Why nothing is ever injected

The core never creates a packet, in either direction. That was not the first
design, and the reason is worth keeping: a USB transfer that has already been
handed up cannot be un-sent, and there is no supported way to push a command
down the control pipe or an event up the event pipe from a hook without owning
the pipe's state machine.

Both things BroadTek owes the host turn out to be edits to packets that already
exist:

- **A reply to a command BroadTek answers itself.** The controller does not
  know these commands, so it answers them with a Command Complete carrying
  "Unknown HCI Command". That event is the reply — `answer()` records what the
  reply should say when the command goes past, and the Command Complete's
  contents are replaced on the way back. If the controller refuses with a
  Command Status instead, the event is dropped so the promised Command
  Complete stands.
- **An accept-list entry for a current RPA.** Rather than sending an add of its
  own, BroadTek rewrites the address inside the host's own LE Add To Accept
  List. A command's length therefore never changes; only a Command Complete
  can grow, and only within the buffer it arrived in.

A consequence: there is no `BT_LOCAL` verdict and no `bt_take_injection()`
any more. A command is always forwarded; an event is forwarded or dropped. The
one place a packet is still fabricated is the kext, which turns a dropped event
into a Command Complete for the NOP opcode, because the transfer it arrived on
has to be completed with something.

The core is covered for the FIPS-197 AES vector, the Bluetooth spec's own
`ah()` sample vector (Vol 3 Part H D.7), the features and supported-commands
rewrites, resolving-list overflow, truncated and lying advertising reports, and
the full sequence report → accept-list update → connection complete.

## 6. Build and install

```bash
nice -n 20 make -j4 kext     # runs both tests first; never skip them
make test                    # the tests alone
make kext DEBUG=0            # the quiet build (see the logging note below)
sudo efi/install.sh          # the user runs this, never the assistant
```

`install.sh` takes `--stage off|events|full`, which picks the boot-arg and
removes the other stages' arguments, so moving between them is just another
install:

| Stage | Boot-arg | What it may change |
|---|---|---|
| `off` (default) | none | nothing: the hooks only report |
| `events` | `-broadtekrwev` | events only, which in practice is the features rewrite alone |
| `full` | `-broadtekrw` | commands as well |

Stage `events` is safe by construction rather than by care: no command ever
reaches the core, so the resolving list stays empty and no pending reply is
registered, which leaves report resolution, connection translation and
Command Complete substitution all inert. The features rewrite is what is
left.

`install.sh` copies the kext next to Lilu, puts its entry after Lilu's in
`config.plist`, adds `-broadtekdbg` plus the chosen stage's argument, and keeps
the previous config as `EFI/OC/config.plist.pre-broadtek`. `sudo efi/uninstall.sh` reverses both.
Changes take effect at the next restart.

Reading the result:

```bash
ioreg -l | grep -i broadtek     # no sudo, survives everything
sudo dmesg | grep -i broadtek   # only useful in the first ~2 minutes after boot
```

**Not `log show`** — see §3. And note the `dmesg` limit: `kern.msgbuf` is
131072 bytes on this machine (the `msgbuf=1048576` boot-arg does not take
effect) and wraps within about three minutes of boot, so anything logged while
Lilu starts its plugins is gone long before anyone can read it. That is why
the same facts are published to the IO registry:

| Property on `IOPlatformExpertDevice` | Meaning |
|---|---|
| `broadtek-started` | the plugin's init ran at all |
| `broadtek-build` | the commit the running kext was built from (`git describe`) |
| `broadtek-patcher-ran` | Lilu's patcher callback fired |
| `broadtek-kext-seen` | Lilu's IOUSBHostFamily callback fired |
| `broadtek-via` | `kext` or `kernel`, whichever resolved the symbols |
| `broadtek-rewriting` | `off`, `events` or `full` |
| `broadtek-hooks` | how many of the nine routes took |
| `broadtek-commands` | HCI commands the hooks have seen, all paths |
| `broadtek-cmds-dev` | ... of those, through `IOUSBHostDevice::internalDeviceRequest` |
| `broadtek-cmd-clients` | the classes that started those control transfers |
| `broadtek-cmds-pipe` | ... of those, through `IOUSBHostPipe::controlRequest` |
| `broadtek-cmds-uc` | ... of those, through a user client |
| `broadtek-events` | HCI events the hooks have seen |
| `broadtek-io-calls` | every `IOUSBHostPipe::io`, event pipe or not |
| `broadtek-acl-in` | ACL transfers read from the peer |
| `broadtek-cmd-nobuf` | HCI-typed control requests with no data buffer |
| `broadtek-cmd-short` | ... with a buffer that yielded fewer than 3 bytes |
| `broadtek-tables-full` | a table refused an entry; if not 0, no list above is complete |
| `broadtek-smp-pdus` | SMP PDUs seen on L2CAP channel `0x0006` |
| `broadtek-irks` | captures holding both an IRK and its identity address |
| `broadtek-irk-ids` | the identity addresses those captures belong to |
| `broadtek-conn-targets` | addresses bluetoothd asks the controller to connect to, with type |
| `broadtek-smp-unparsed` | SMP PDUs skipped: truncated, or an ACL continuation |
| `broadtek-endpoints` | every endpoint seen, `addr:type:count` |
| `broadtek-event-codes` | distinct HCI event codes seen on the interrupt pipe |
| `broadtek-evt-hci-shaped` | interrupt transfers shaped like an HCI event |
| `broadtek-evt-not-hci` | interrupt transfers that were not |
| `broadtek-cmds-fw` | ... of those, through the IOUSBHost framework client |
| `broadtek-opcodes-fw` | distinct opcodes on the framework-client path |
| `broadtek-opcodes-cc` | opcodes read back out of Command Complete and Command Status |
| `broadtek-ctrl-calls` | control requests seen at any hook, before filtering |
| `broadtek-ctrl-types` | distinct `bmRequestType/bRequest` pairs, with counts |
| `broadtek-opcodes` | distinct opcodes on the pipe path, little-endian pairs |
| `broadtek-opcodes-uc` | distinct opcodes on the user-client path |

### Two traps worth remembering

**Check `broadtek-build` before reading anything else.** Both projects on this
machine have an `efi/install.sh`, and AirPort_RTW89's is the one that runs if
the shell happens to be sitting in `~/Developer/AirPort_RTW89` — which it
usually is. The symptom is perfect: BroadTek still loads, every property is
there, and the counters read exactly what they read on the previous boot,
because the kext that booted is the previous kext. It cost one reboot before
`broadtek-build` existed to say so. Install with the absolute path:

```bash
sudo efi/install.sh <EFI partition UUID>
```

`install.sh` prints the build it copied; that string and `broadtek-build` have
to match after the restart.

### A trap worth remembering

Lilu compiles `DBGLOG` to nothing unless `DEBUG` is defined. The first build
installed had every per-packet log line silently compiled out, which looked
exactly like the hooks never firing. The Makefile now passes `-DDEBUG=1` by
default, and the first 64 commands and 64 events go through `SYSLOG`, which is
never compiled out, before falling back to `DBGLOG`.

### Instant Hotspot from the Wi-Fi menu, with no key and no helper (2026-11-25)

HONOR HLYL-WFQ9, RTL8822CU, macOS Tahoe 26.7.1, iPhone 16 Pro Max (identity
`c4:f7:c1:92:93:7a`, public). Picking the iPhone in the Wi-Fi menu timed out
after ~22 s with `RPErrorDomain -71143`: bluetoothd put the iPhone's public
identity on the accept list (`fal+/pub/c4:f7:c1:92:93:7a`), the phone
advertises from an RPA, and there is no IRK for it on this Mac (above).

**First proof, with the log join.** `tools/map_install` joins the kext's
air-side (RPA <-> NearbyInfo tag) to bluetoothd's `Identities resolve added:
Addr ..., AT ...` line. It had never run on this machine, and it had a bug that
would have stopped it here: it guessed the identity's type from the top bits,
and `c4:...` looks static-random, so the mapping went in as type 1 and never
matched bluetoothd's type-0 add. It now takes the type from
`broadtek-conn-targets`. With it running, at 10:32 the click went through:
Mac-initiated LE link, `com.sharing.hotspot.request` sent over BLE, `Request
complete: success`, association to the hotspot in 7.0 s. At 10:36, with the
phone's hotspot on 2.4 GHz ("Maximize Compatibility"), `en0` got
`172.20.10.2` and traffic flowed through the phone.

**Then without the log: guess and prove, in the kext (0.3.2).** bluetoothd
encrypts the link with the identity's bond key ~160 ms after it connects
(`Encryption is now enabled for device ... Public C4:F7:C1:92:93:7A`). Only
the real device holds that key, so the controller's Encryption Change is a
proof of who is at the other end. `src/core/bt_hci.c`, "Guessing and
proving":

- candidates: every RPA advertising Apple NearbyInfo, with RSSI, connectable
  flag, first/last heard;
- on bluetoothd's accept-list add for an identity with no key and no proven
  address, the likeliest candidate is **added beside** the identity (an
  injected 0x2011, its Command Complete turned into a NOP) -- never in its
  place, so a non-Apple peripheral keeps its own entry;
- the link that forms is reported as the identity; Encryption Change success
  proves the address, failure or a disconnect before encryption rejects it,
  and at the next accept-list connection attempt the rejected guess is swapped
  for the next candidate;
- a proven address is then substituted like a resolved one; when it stops
  advertising (rotation) the candidate that appeared as it went quiet is tried
  first;
- an identity that fails three guesses and never proves one is left alone.

Counters: `broadtek-guessing`, `-guesses`, `-guess-proven`,
`-guess-rejected`, `-guess-stale`, `-guess-cmds-sent/-failed/-timeouts`,
`-guess-last-status`. Off with `-broadtekguessoff`; needs `-broadtekrw`.

**Still open:** a hotspot on 5 GHz (channel 44) associates but DHCP never
starts on `en0`; that is AirPort_RTW88, not Bluetooth.

## 7. Where it stands

- Phase 0 is **observe-only**: without `-broadtekrw` the plugin changes
  nothing, it only reports what its hooks see. This is deliberate. The hook
  points are a deduction from the logs, not an observation.
- The kext loads. `kextstat` shows `org.franciumos.BroadTek (0.1.0)` linked
  against Lilu (57) and IOUSBHostFamily (38).
- First hardware result, 2026-10-15: `broadtek-started = Yes` but
  `broadtek-hooks = 0`. The plugin ran and installed nothing. The cause was
  routing against `KernelPatcher::KernelID`: IOUSBHostFamily is a boot kext,
  already loaded before a Lilu plugin starts, and its symbols do not resolve
  that way. It is now requested as a `KextInfo` with the `Loaded` flag and
  routed from the kext callback.

- **Second result, same day: `broadtek-hooks = 3`, `broadtek-via = "kext"`,
  `broadtek-commands = 1`.** The hooks are in, and HCI commands really do
  travel through `IOUSBHostPipe::controlRequest`, which is the assumption the
  whole design rests on. The count was published only every 32nd command, so
  "1" was the value at the first publish, not a total; the counters are now
  pre-created OSNumbers updated in place, so they read live, and the distinct
  opcodes seen are published too.

- **Third result, same day, and it overturns the second one.**
  `broadtek-commands = 253`, `broadtek-hooks = 3`, `broadtek-via = "kext"`,
  and the opcode list reads `<01106dfc61fc20fc>` — four distinct opcodes:

  | Opcode | OGF | What |
  |---|---|---|
  | `0x1001` | 0x04 | Read Local Version Information |
  | `0xfc6d` | 0x3f | Realtek vendor |
  | `0xfc61` | 0x3f | Realtek vendor |
  | `0xfc20` | 0x3f | Realtek vendor: download firmware patch |

  All 253 commands are the in-kernel firmware upload, not bluetoothd. OGF
  0x3f is vendor-specific, `0xfc20` is the patch-download command repeated for
  each chunk, and `RealtekBluetoothFirmware (1.0.2)` is loaded and linked
  against IOUSBHostFamily. Decisive is what is *missing*: no HCI Reset
  (`0x0c03`), which bluetoothd always sends, and no LE command (OGF 0x08) at
  all. bluetoothd's HCI traffic does not pass through
  `IOUSBHostPipe::controlRequest`.

  So the second result's conclusion — "HCI commands really do travel through
  `IOUSBHostPipe::controlRequest`" — was right about the function and wrong
  about whose commands they were. The hook was watching the firmware loader.
  Worth remembering: a counter climbing is not evidence the traffic is the
  traffic you want. The opcodes were what settled it.

- **`broadtek-events = 0` was not evidence of anything.** The reply-path
  rework (§5a) removed the event hook's counter increment, so the number could
  not move whatever the event path did. Restored.

- **Fourth result, build `7725180`: the event path works.**
  `broadtek-hooks = 4`, `broadtek-events = 1043`, `broadtek-io-calls = 1237`,
  `broadtek-cmds-pipe = 253`, `broadtek-cmds-uc = 0`.

  Events really do arrive through `IOUSBHostPipe::io` on the interrupt IN
  endpoint, and `isEventPipe()` matches them: 1043 of 1237 io calls were the
  event pipe. This is the first confirmed half of the design, and the larger
  half. Steps 1, 3 and 5 of §4 are all event-path work: the features rewrite,
  resolving RPAs in advertising reports, and putting the identity back into
  Connection Complete.

  `broadtek-cmds-uc = 0` says bluetoothd does not use
  `AppleUSBHostUserClient::ControlRequest`. The live IOKit instance counts say
  why, and rule out the other guesses at the same time:

  | Class | Instances | |
  |---|---|---|
  | `AppleUSBLegacyInterfaceUserClient` | 0 | the IOUSBLib path is unused |
  | `AppleUSBLegacyDeviceUserClient` | 0 | likewise |
  | `AppleUSBHostFrameworkInterfaceClient` | 2 | the framework path is in use |
  | `IOUSBHostPipe` | 1 | only the interrupt pipe is still open |

  One pipe object alive, and the event hook is on it. The firmware loader's
  control pipe is gone, which fits its 253 commands all having arrived at boot.

- **Where the commands actually go.** Every function in IOUSBHostFamily taking
  a `StandardUSB::DeviceRequest&` was enumerated from the kernel collection.
  All of them converge on one function: the five
  `IOUSBHostDevice::deviceRequest` overloads, the four on
  `IOUSBHostInterface`, and the framework and user clients above them.

  ```
  IOUSBHostDevice::internalDeviceRequest(IOService *forClient,
      StandardUSB::DeviceRequest&, void *raw, IOMemoryDescriptor*,
      uint32_t &transferred, IOUSBHostCompletion*, uint32_t timeout)
  ```

  It goes on to `internalDeviceRequestGated`, and that gated hop is why the
  public `IOUSBHostPipe::controlRequest` entry never saw bluetoothd: its
  client talks to the *device*, and the device does not call the public pipe
  entry. The firmware loader calls the pipe directly, which is why that hook
  caught it and nothing else.

  `internalDeviceRequest` is now hooked. It is a chokepoint rather than a
  guess: whatever starts a control transfer to this device goes through it.
  Its `forClient` argument also says who, so the distinct callers are
  published as `broadtek-cmd-clients`.

- **Fifth result, build `d2d3680`: the chokepoint is hooked and bluetoothd is
  not in it.** `broadtek-hooks = 5`, `broadtek-cmds-dev = 253`,
  `broadtek-opcodes-dev = <01106dfc61fc20fc>`,
  `broadtek-cmd-clients = "IOUSBHostInterface"`, `broadtek-commands = 506`.

  506 is 253 twice: the same transfers passed `internalDeviceRequest` and then
  `IOUSBHostPipe::controlRequest`, so the firmware loader goes device then
  pipe, and both hooks counted it. The opcodes are the same four as before and
  the only caller is `IOUSBHostInterface`.

  So every control transfer this machine makes to the dongle is the firmware
  upload. **bluetoothd's HCI commands do not pass through IOUSBHostFamily's
  control path at all** -- not the pipe entry, not the user client, and not
  the chokepoint every in-kernel control transfer must cross.

- **Correction to the fourth result.** "The event path works" was claimed on
  `broadtek-events = 1043`, and that was premature: `isEventPipe()` tests only
  for an interrupt IN endpoint, with no check of *which device*, so it matches
  any interrupt endpoint on the machine. There are two USB devices here, the
  dongle and a webcam (the keyboard and trackpad are I2C), which makes the
  dongle the likely source but does not establish it. The count proved that
  something sends interrupt IN transfers, not that BroadTek was reading HCI.

  Two things now settle it, and both read out of the registry:

  | Property | Means |
  |---|---|
  | `broadtek-endpoints` | every endpoint seen, as `addr:type:count` |
  | `broadtek-event-codes` | distinct first bytes where the packet had HCI shape |
  | `broadtek-evt-hci-shaped` | interrupt IN transfers where `len == 2 + plen` |
  | `broadtek-evt-not-hci` | ... and where it did not |

  HCI events are a code, a parameter length, and exactly that many bytes, so
  the shape test is cheap and hard to pass by accident. Event codes `0e`
  (Command Complete), `3e` (LE Meta) or `05` (Disconnection Complete) are
  unmistakable. If instead the shaped count stays near zero, those interrupt
  transfers were the webcam and the event path is still unfound.

  `broadtek-endpoints` answers the other half. 3082 io calls against 2877
  events leaves 205 on other endpoints, and naming them says whether
  bluetoothd is moving HCI over a bulk endpoint -- which would explain the
  silent control path, and is the first thing the numbers have actually
  pointed at.

- **Sixth result, build `82dbcb8`: the event path is proven, not assumed.**
  `broadtek-evt-hci-shaped = 1307`, `broadtek-evt-not-hci = 0`. Every single
  interrupt IN transfer had exact HCI event shape, and the distinct codes were
  `<140e053e0f031b1d0b0c2313170608>`:

  | | | | |
  |---|---|---|---|
  | `03` Connection Complete | `05` Disconnection Complete | `06` Authentication Complete | `08` Encryption Change |
  | `0b` Read Remote Supported Features | `0c` Read Remote Version Info | `0e` **Command Complete** | `0f` **Command Status** |
  | `13` Number Of Completed Packets | `14` Mode Change | `17` Link Key Request | `1b` Max Slots Change |
  | `1d` Connection Packet Type Changed | `23` Read Remote Extended Features | `3e` **LE Meta Event** | |

  Fifteen valid HCI event codes, nothing that is not one, including the LE Meta
  events BroadTek exists to rewrite. This is bluetoothd's live HCI event
  stream and BroadTek is reading it. Steps 1, 3 and 5 of §4 rest on evidence
  now.

  `broadtek-endpoints = "81:int:1308 82:bulk:95 83:int:1 02:bulk:94"` accounts
  for the rest: `0x81` interrupt IN is the event endpoint, `0x82` and `0x02`
  are ACL in and out (95 and 94, paired, so real links were up), and
  `0x83` with a single transfer is the webcam. No control endpoint appears,
  which is expected: the endpoint table is filled from `IOUSBHostPipe::io`,
  and control transfers do not go through it.

- **And the command path is still missing, but now it is a contradiction.**
  This boot was warm, so the firmware was already loaded and only one command
  was seen at all (`broadtek-opcodes-dev = <0110>`, Read Local Version
  Information, counted twice as before). Yet Command Complete and Command
  Status are both arriving in the event stream, and bluetoothd brought up real
  links. Commands are certainly being sent; none of the five hooks sees them.

  `IOUSBHostDevice::internalDeviceRequestGated` would be the next level down,
  and it is deliberately **not** hooked: its
  `IOUSBHostPipe::tInternalControlRequestParameters` argument is declared in
  MacKernelSDK but never defined, so hooking it means guessing a struct layout
  in a live USB path. Not worth a panic.

  Instead, two cheaper things. `AppleUSBHostFrameworkInterfaceClient` has two
  live instances while both legacy clients have none, so all four of its
  control entry points are hooked (`deviceRequest` and
  `hostPipeControlRequest`, each async and sync) and counted as
  `broadtek-cmds-fw` with opcodes in `broadtek-opcodes-fw`. Nine hooks now.

  And the replies are mined for what the commands were: a Command Complete
  carries the opcode it answers, so `broadtek-opcodes-cc` lists what
  bluetoothd is sending **whether or not the command path is ever found**.
  That list is worth having for its own sake -- it says whether bluetoothd
  sends `1002` and `2003` at all, which is what the features rewrite depends
  on.

- **Seventh result, build `3ee05a9`: bluetoothd's whole command inventory,
  read out of the replies.** `broadtek-opcodes-cc` carried 32 opcodes, which
  is bluetoothd's complete HCI startup sequence:

  `0c03` Reset, `1001`/`1002`/`1003`/`1004`/`1005`/`1009` the local
  information reads, `0c01` Set Event Mask, `2001` LE Set Event Mask,
  **`2003` LE Read Local Supported Features**, `0c6d` Write LE Host Support,
  `200f` LE Read Filter Accept List Size, `201c` LE Read Supported States,
  `2024`/`202f` the data-length reads, `2031` LE Set Default PHY, `2060` LE
  Read Transmit Power, `2074` LE Set Host Feature, plus the BR/EDR writes and
  the four Realtek vendor commands.

  The two opcodes the features rewrite targets, **`1002` Read Local Supported
  Commands and `2003` LE Read Local Supported Features, are both in that
  list**, and their Command Completes come back on the event path BroadTek
  already reads and can already edit. Step 1 of §4 needs nothing further from
  the transport.

  `broadtek-cmds-fw = 0`, so the framework client is not the command path
  either. Nine hooks, no commands.

- **Why nine hooks saw nothing: the filter, not the hooks.** Settled by
  disassembly rather than another reboot.
  `AppleUSBHostUserClient::_ControlRequest`, the external-method trampoline,
  makes only two direct calls (`IOMallocTypeImpl` and `IOFreeTypeImpl`) and
  two virtual ones, at vtable offsets `0x980` and `0x9d0`. Decoding those
  slots out of `AppleUSBHostInterfaceUserClient`'s vtable (chained fixups; the
  encoded value is a KC file offset, so add `0xffffff8000100000`):

  | Slot | Target |
  |---|---|
  | `0x980` | `AppleUSBHostUserClient::ControlRequest` |
  | `0x9d0` | `AppleUSBHostInterfaceUserClient::copyMemoryDescriptorFromArguments` |

  Slot `0x980` is the function BroadTek already hooks, and Lilu patches the
  function itself, so reaching it through a vtable changes nothing. The hook
  runs. Its *filter* is what discards the traffic:

  ```c
  if (request.bmRequestType == kHCICommandRequestType && dataBuffer != nullptr)
  ```

  `kHCICommandRequestType` is `0x20`, from the USB HCI transport spec. Nothing
  has ever checked that this dongle's commands actually carry it, and a
  rejected packet and an uncalled hook are indistinguishable in a counter.

  So the filter now comes after the counting. `broadtek-ctrl-calls` counts
  every control request each hook sees, unfiltered, and `broadtek-ctrl-types`
  lists the distinct `bmRequestType/bRequest` pairs with counts. That says
  what bluetoothd's commands really look like instead of assuming, and
  separates "the hook never ran" from "the filter threw it away" for good.

  A lesson worth keeping: four of these rounds went into finding a path that
  was hooked correctly from the third round onward. The counter that read 0
  was trusted as evidence about the *path* when it was really evidence about
  the *predicate*.

- **Eighth result, build `ba31d2e`: found it.** `broadtek-ctrl-calls = 935`,
  and `broadtek-ctrl-types` names every kind:

  | `bmRequestType`/`bRequest` | Direction, type, recipient | Count | |
  |---|---|---|---|
  | `80/06` | IN, standard, device | 66 | GET_DESCRIPTOR |
  | `00/09` | OUT, standard, device | 4 | SET_CONFIGURATION |
  | `00/03` | OUT, standard, device | 2 | SET_FEATURE |
  | `20/00` | OUT, class, **device** | 2 | the one warm-boot command, at two hooks |
  | `01/0b` | OUT, standard, interface | 30 | SET_INTERFACE |
  | **`21/00`** | OUT, class, **interface** | **549** | **bluetoothd's HCI commands** |
  | `a1/81`..`a1/87` | IN, class, interface | 210 | the webcam's UVC control reads |

  The commands were arriving at a hook BroadTek has had in place since the
  third round. They address the **interface** (`0x21`), not the device
  (`0x20`), and the filter demanded `0x20`. One bit of the recipient field.

  Both are a class OUT request with `bRequest` 0 and the same HCI command
  packet in the data stage, so the recipient is not something to filter on at
  all. The predicate is now direction OUT, type class, `bRequest` 0, any
  recipient, and it is written as `isHCICommand()` in one place instead of
  being repeated at five call sites.

  The webcam's `a1/8x` block is a useful check that the census decodes
  correctly: those are UVC GET_CUR/MIN/MAX/RES/INFO/DEF, exactly what a
  camera's control interface sends.

- **Both halves of the transport are now found.** The event path was proven in
  the sixth round and the command path in this one. What remains is to confirm
  the fixed filter actually yields the 549 commands, and then `-broadtekrw`
  is the next step rather than a guess.

### What the next boot answers

The command hook now sits on `AppleUSBHostUserClient::ControlRequest` as well.
That is the one entry point every user client funnels through: the device and
interface subclasses override only `internalControlRequest`, which the base
calls, so hooking the base catches bluetoothd whichever client it holds. Four
hooks now, and the counters are split by path:

| Property | Means |
|---|---|
| `broadtek-cmds-pipe` | commands through `IOUSBHostPipe::controlRequest` (the firmware loader) |
| `broadtek-cmds-uc` | commands through `AppleUSBHostUserClient::ControlRequest` (was 0) |
| `broadtek-cmds-dev` | commands through `IOUSBHostDevice::internalDeviceRequest` (**the chokepoint**) |
| `broadtek-opcodes-dev` | the opcodes on it: look for `0c03`, `2003`, `200d`, `1120` |
| `broadtek-cmd-clients` | the classes that started those transfers |
| `broadtek-opcodes-uc` | the opcodes seen on that path — look for `0x0c03`, `0x2003`, `0x200d`, `0x1120` |
| `broadtek-io-calls` | every `IOUSBHostPipe::io`, event pipe or not |
| `broadtek-events` | completions on the interrupt IN pipe |

`broadtek-io-calls` is there to separate two faults that look identical: `io`
never being called, and `io` being called but `isEventPipe()` never matching
it. If io-calls climbs while events stays at 0, the endpoint test is wrong,
not the hook.

### Reading the result

| `ioreg -l \| grep -i broadtek` shows | Meaning | Next |
|---|---|---|
| nothing at all | the plugin never started | check Lilu loaded it; `-liludbgall`; check the `PluginConfiguration` version range |
| `broadtek-started` but no `broadtek-hooks` | Lilu's patcher callback never fired | investigate Lilu patcher availability on this SIP configuration |
| `broadtek-hooks = 0` | the symbols did not resolve | `dmesg` names each one if read soon enough; the mangled names are in `installRoutes` |
| `broadtek-hooks < 9` | one symbol did not resolve | `dmesg` names it: `cannot route <symbol>` |
| `broadtek-cmds-uc = 0` | bluetoothd is not on the hooked user-client path | §8 item 1 |
| `broadtek-opcodes-uc` holding `0c03` or `2003` | **the path is right** | try `-broadtekrw` |

Read `broadtek-opcodes-uc`, not just the counters. A count that climbs says
only that *something* speaks HCI through that function; the opcodes say
whether it is bluetoothd. That distinction is what the second hardware result
got wrong (§7).

Nothing above the hook layer has to change whatever the answer is: `src/core`
does not care where the packets come from.

## 8. Open work, in order

1. ~~Parse the Watch's keychain blob.~~ **Done.** It is a property list with
   one field, `Remote IRK`, 16 bytes, and the item's `acct` attribute carries
   the identity address it belongs to. The query recipe is in §3.
2. **Give BroadTek a user client** to receive a key: root-only, write-only,
   feeding the same promote step `bt_smp.c` would use. It has none today.
3. **Then stage `full`**, with a real IRK in the resolving list, and see
   whether a watch notification arrives. That is the first end-to-end test
   this project has been able to attempt.
4. **Find the iPhone's key, or establish that it is not local.** It is not in
   `com.apple.bluetooth` under `BluetoothLESync`, yet macOS acts as though it
   has one. Ask by exact service name; do not enumerate access groups.
5. **An RPA that rotates while the identity is on the accept list.** Nothing
   updates the controller's list until bluetoothd adds the identity again. The
   old design pushed that update as an injected command; the no-injection
   design (§5a) cannot, by construction. In practice bluetoothd re-adds on
   each scan cycle, so this may never bite — but it is the first thing to
   suspect if a connection works once and not after 15 minutes.
6. Local IRK and local privacy are not implemented: LE Read Local Resolvable
   Address returns zeroes. Fine so far; revisit if macOS objects.
7. The slot pool for completion wrapping is 32 deep and silently passes the
   transfer through unwrapped when it runs out. Acceptable for an event pipe,
   worth revisiting if it is ever used on a busy pipe.

8. **LE advertising does not work at all. Not closed -- one route is open and
   untried.** The gate is the chipset number, and its only two
   sources are both shut -- the USB classifier gives 0 and also decides
   whether the controller is used at all, and the VSC that would correct it is
   never sent *because* the chipset is 0 -- all of which is true **at chipset
   0**. But the classifier can also produce chipset 4000, for seven MediaTek
   id pairs, from inside the dongle-found path so the controller is kept. That
   is reachable by overriding two IO registry properties with machinery this
   project already has, and if chipset 4000 has a non-empty VSC set the rest
   follows. See the correction in
   [LE_ADVERTISING.md](LE_ADVERTISING.md). Original note follows for context. This Mac emits no BLE advertisements: bluetoothd refuses
   every Continuity advertiser with "Stack is not ready" forever, because
   `LeBroadcaster::stackDidStart` fails with `0x98`. Traced end to end in
   [LE_ADVERTISING.md](LE_ADVERTISING.md): the refusal is one test on one
   global, which is fed from the **chipset number** (`+0x334` of the chipset
   singleton) and not from HCI at all. The controller already reports LE
   Extended Advertising -- bit 12 is set in the `BD 5F 66 3C` features line in
   §3 -- and bluetoothd logs that line and then ignores it, which is the same
   trap as LL Privacy. Chipset 0 (`THIRD_PARTY_DONGLE`) fails; the
   internal-module default of 3000 passes. So the work is in the classifier at
   `0x1001d1be0`, not in the HCI stream, and it would fix AirDrop, Handoff,
   Universal Clipboard and iPhone Mirroring together. Do not try the
   `SupportsMultipleAdvertising` preference: it fires and does nothing, and the
   document says why.

## 9. Hardware test cases

1. **Instant Hotspot** — pick the iPhone under Personal Hotspot in the Wi-Fi
   menu. Pass: it connects and sharingd logs no `BLE client connect timeout`.
2. **Apple Watch approval** — trigger a watch approval (unlocking a System
   Settings pane, or `sudo` with watch approval). Pass: **the prompt arrives on
   the watch.** A full Auto Unlock also needs Wi-Fi ranging over AWDL, which
   AirPort_RTW89 does not implement, so a prompt that arrives but does not
   complete the unlock still counts as a pass for BroadTek.
3. **Regression** — AirPods, Bluetooth keyboard and mouse still pair and
   reconnect.

```bash
log show --last 10m --predicate 'process == "bluetoothd"' | grep -Ei 'resolv|irk|connect'
```

## 10. Rules this work follows

- **Never load or unload a kext without asking first.** A bad load panics the
  machine and ends the session. This is not a formality: the plugin hooks a USB
  path in the kernel.
- **The user types sudo.** Never ask for the password, never try to store it,
  never run the EFI install. Prepare it; they run it.
- **Commit before any load**, so a panic loses nothing.
- **No GitHub remote and no push without asking.** The repository is local.
- **No AI or assistant mentions** in files or commits, and no `Co-Authored-By`
  trailers. Same rule as AirPort_RTW89.
- Build gently (§2).
- After a restart, check `/Library/Logs/DiagnosticReports/*.panic`.
