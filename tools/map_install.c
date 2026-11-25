/* SPDX-License-Identifier: MIT */
/* Tell the kext which private address a device is using right now.
 *
 * The iPhone has no Bluetooth IRK on this machine (docs/STATE.md), so BroadTek
 * cannot derive its RPA the way it derives the Apple Watch's -- and no key in
 * the Rapport identities reproduces the NearbyInfo tag either
 * (tools/nearby_probe.c, 39744 derivations, no match). But the join does not
 * need a key:
 *
 *   the kext sees, on the air:   private address  <->  NearbyInfo auth tag
 *   bluetoothd says, in its log: identity         <->  NearbyInfo auth tag
 *                                therefore        identity <-> private address
 *
 * The kext publishes its half as `broadtek-nearby`. This program reads that,
 * reads the other half out of the log, joins the two on the tag, and installs
 * the result through BroadTekConfig. From there everything is the mechanism
 * already proven on the Watch: the accept-list add is rewritten to the private
 * address, and the connection complete is translated back to the identity.
 *
 *     make map-install && sudo build/out/map_install [--loop] [--quiet]
 *
 * Addresses are not secret -- bluetoothd prints them and ioreg carries the
 * controller's own -- and no key is read, so this needs no entitlement, only
 * root to set a property on the kext.
 *
 * The tags rotate with the addresses, every fifteen minutes or so, which is
 * what --loop is for. Reading the log is the weak part of this and it is worth
 * being honest about it: the format belongs to Apple, and a build that prints
 * the tag as <private> would stop the join working. It is a real route, not a
 * good one. */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAP_BLOB_LEN (1 + 6 + 6)
#define MAX_PAIRS 256

struct pair {                   /* one side of the join */
    char     tag[7];            /* six hex digits */
    uint8_t  addr[6];           /* most significant byte first, as printed */
};

static int parseHex6(const char *hex, uint8_t out[6])
{
    for (int i = 0; i < 6; i++) {
        char b[3] = { hex[i * 2], hex[i * 2 + 1], 0 };
        if (b[0] == '\0' || b[1] == '\0') return 0;
        char *end = NULL;
        out[i] = (uint8_t)strtoul(b, &end, 16);
        if (end != b + 2) return 0;
    }
    return 1;
}

/* ---- the kext's half: broadtek-nearby ---------------------------------- */

/* "<addr12>#<count>:<tlv hex>" per record, space separated. The auth tag is
 * the three bytes after the TLV's type, length and two flag bytes -- checked
 * against the log's own nbIAT field (docs/STATE.md). */
static int readNearby(struct pair *out, int max, int *n_tagless)
{
    /* The kext publishes on the platform expert, which is a child of the
     * plane's root, so the property has to be searched for rather than read
     * off "IOService:/" itself. */
    io_registry_entry_t root = IORegistryGetRootEntry(kIOMainPortDefault);
    if (root == IO_OBJECT_NULL) return 0;

    CFTypeRef prop = IORegistryEntrySearchCFProperty(root, kIOServicePlane,
                                                     CFSTR("broadtek-nearby"),
                                                     kCFAllocatorDefault,
                                                     kIORegistryIterateRecursively);
    IOObjectRelease(root);
    if (prop == NULL) return 0;

    char *text = NULL;
    if (CFGetTypeID(prop) == CFStringGetTypeID()) {
        CFIndex sz = CFStringGetMaximumSizeForEncoding(CFStringGetLength(prop),
                                                       kCFStringEncodingUTF8) + 1;
        text = malloc((size_t)sz);
        if (text != NULL && !CFStringGetCString(prop, text, sz, kCFStringEncodingUTF8)) {
            free(text);
            text = NULL;
        }
    }
    CFRelease(prop);
    if (text == NULL) return 0;

    int n = 0;
    for (char *item = strtok(text, " "); item != NULL && n < max; item = strtok(NULL, " ")) {
        char *colon = strchr(item, ':');
        char *hash = strchr(item, '#');
        if (colon == NULL || hash == NULL || hash > colon) continue;
        *hash = '\0';
        const char *tlv = colon + 1;
        if (strlen(item) < 12) continue;
        /* type(1) len(1) flags(2) then the tag: 8 hex digits in, 6 of tag. */
        if (strlen(tlv) < 14) { if (n_tagless != NULL) (*n_tagless)++; continue; }

        struct pair *p = &out[n];
        uint8_t raw[6];
        if (!parseHex6(item, raw)) continue;
        /* The kext publishes the address as HCI carries it, least significant
         * byte first; the log prints the other way round. Everything from here
         * on is most significant byte first, as printed. */
        for (int k = 0; k < 6; k++) p->addr[k] = raw[5 - k];
        memcpy(p->tag, tlv + 8, 6);
        p->tag[6] = '\0';
        n++;
    }
    free(text);
    return n;
}

/* ---- bluetoothd's half: identity <-> tag ------------------------------- */

/* "Identities resolve added: Addr <12 hex>, AT <6 hex>".
 *
 * When macOS could not resolve a device it prints the private address here
 * instead, and such a line maps an address to itself. Those are dropped by the
 * join, not here: an identity cannot be recognised by its bit pattern. The
 * iPhone's own public address is 64:00:00:00:00:01, whose top two bits are 01
 * -- the same as a resolvable private address -- so testing the bits skips the
 * one device this exists for. That bug cost a run. */
static int readIdentities(struct pair *out, int max)
{
    FILE *f = popen("/usr/bin/log show --last 20m --info --predicate "
                    "'process == \"bluetoothd\" AND eventMessage CONTAINS "
                    "\"Identities resolve\"' 2>/dev/null", "r");
    if (f == NULL) return 0;

    char line[4096];
    int n = 0;
    while (n < max && fgets(line, sizeof(line), f) != NULL) {
        char *at = strstr(line, "Addr ");
        if (at == NULL) continue;
        at += 5;
        if (strlen(at) < 12) continue;

        uint8_t addr[6];
        if (!parseHex6(at, addr)) continue;

        char *tagp = strstr(at, ", AT");
        if (tagp == NULL) continue;
        tagp += 4;
        while (*tagp == 'N' || *tagp == 'C' || *tagp == ' ') tagp++;
        if (strlen(tagp) < 6) continue;

        uint8_t tmp[3];
        char six[7] = {0};
        memcpy(six, tagp, 6);
        for (int i = 0; i < 3; i++) {
            char b[3] = { six[i * 2], six[i * 2 + 1], 0 };
            char *end = NULL;
            tmp[i] = (uint8_t)strtoul(b, &end, 16);
            if (end != b + 2) { six[0] = '\0'; break; }
        }
        (void)tmp;
        if (six[0] == '\0') continue;

        /* Keep every distinct (identity, tag) pair, not just each identity's
         * newest tag: a slightly older tag is often the one still on the air,
         * and the air is the source of truth for which RPA is current. The
         * join is driven from the air, so a stale pair here is simply never
         * matched. */
        int seen = -1;
        for (int i = 0; i < n; i++)
            if (memcmp(out[i].addr, addr, 6) == 0 && memcmp(out[i].tag, six, 7) == 0) {
                seen = i; break;
            }
        if (seen >= 0) continue;
        memcpy(out[n].addr, addr, 6);
        memcpy(out[n].tag, six, 7);
        n++;
    }
    pclose(f);
    return n;
}

/* ---- the identity's address type --------------------------------------- */

/* The type the kext matches on is the one bluetoothd puts on the accept list,
 * which the kext publishes in broadtek-conn-targets ("fal+/pub/c4:f7:..."). It
 * cannot be read off the address bits: a public address is a vendor OUI and
 * may start with anything, so c4:f7:c1:... -- an iPhone 16 Pro Max's public
 * identity -- looks like a static random address by its top two bits. Taking
 * that guess installed the mapping under type 1, rl_find() looked under type 0
 * and nothing was ever substituted.
 *
 * Returns 0 (public) or 1 (random), or -1 if the kext has not seen bluetoothd
 * use this identity yet. */
static int identityType(const uint8_t addr[6])
{
    io_registry_entry_t root = IORegistryGetRootEntry(kIOMainPortDefault);
    if (root == IO_OBJECT_NULL) return -1;
    CFTypeRef prop = IORegistryEntrySearchCFProperty(root, kIOServicePlane,
                                                     CFSTR("broadtek-conn-targets"),
                                                     kCFAllocatorDefault,
                                                     kIORegistryIterateRecursively);
    IOObjectRelease(root);
    if (prop == NULL) return -1;

    char text[2048] = {0};
    int ok = CFGetTypeID(prop) == CFStringGetTypeID() &&
             CFStringGetCString(prop, text, sizeof(text), kCFStringEncodingUTF8);
    CFRelease(prop);
    if (!ok) return -1;

    char want[18];
    snprintf(want, sizeof(want), "%02x:%02x:%02x:%02x:%02x:%02x",
             addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
    for (char *at = strstr(text, want); at != NULL; at = strstr(at + 1, want)) {
        if (at - text < 4) continue;
        if (strncmp(at - 4, "pub/", 4) == 0) return 0;
        if (strncmp(at - 4, "rnd/", 4) == 0) return 1;
    }
    return -1;
}

/* ---- the install ------------------------------------------------------- */

static int install(const uint8_t *blob)
{
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault,
                                                   IOServiceMatching("BroadTekConfig"));
    if (svc == IO_OBJECT_NULL) {
        fprintf(stderr, "BroadTekConfig not found: is the kext loaded?\n");
        return 0;
    }

    CFDataRef data = CFDataCreate(NULL, blob, MAP_BLOB_LEN);
    CFStringRef key = CFSTR("broadtek-install-map");
    CFDictionaryRef props = CFDictionaryCreate(NULL, (const void **)&key,
                                               (const void **)&data, 1,
                                               &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    kern_return_t kr = IORegistryEntrySetCFProperties(svc, (CFTypeRef)props);
    CFRelease(props);
    CFRelease(data);
    IOObjectRelease(svc);

    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "  the kext refused it: 0x%x%s\n", kr,
                kr == kIOReturnNotPrivileged ? " (not privileged: run with sudo)" : "");
        return 0;
    }
    return 1;
}

static int runOnce(int quiet)
{
    struct pair air[MAX_PAIRS], ident[MAX_PAIRS];
    int tagless = 0;
    int n_air = readNearby(air, MAX_PAIRS, &tagless);
    int n_id = readIdentities(ident, MAX_PAIRS);

    if (!quiet)
        printf("%d address%s with a tag on the air, %d identit%s in the log\n",
               n_air, n_air == 1 ? "" : "es", n_id, n_id == 1 ? "y" : "ies");

    if (n_air == 0 && !quiet)
        printf("  nothing captured yet: is a build with broadtek-nearby loaded?\n");

    /* Air-driven: every RPA on the air is a current address. For each, if the
     * log ties its tag to an identity, that is a live identity<->RPA pair. One
     * install per identity (the resolving-list entry is replaced), so a device
     * advertising two addresses at once settles on the last seen. */
    int done = 0;
    for (int j = 0; j < n_air; j++) {
        /* Skip an air address that is itself a known identity (a device macOS
         * could not resolve, logged against its own tag). */
        int self = 0;
        for (int i = 0; i < n_id; i++)
            if (memcmp(ident[i].addr, air[j].addr, 6) == 0) { self = 1; break; }
        if (self) continue;

        for (int i = 0; i < n_id; i++) {
            if (strcmp(ident[i].tag, air[j].tag) != 0) continue;
            if (memcmp(ident[i].addr, air[j].addr, 6) == 0) continue;

            uint8_t blob[MAP_BLOB_LEN] = {0};
            int type = identityType(ident[i].addr);
            if (type < 0) type = (ident[i].addr[0] & 0xc0) == 0xc0 ? 0x01 : 0x00;
            blob[0] = (uint8_t)type;
            for (int k = 0; k < 6; k++) blob[1 + k] = ident[i].addr[5 - k];
            for (int k = 0; k < 6; k++) blob[7 + k] = air[j].addr[5 - k];

            if (!quiet)
                printf("  %02x:%02x:%02x:%02x:%02x:%02x -> "
                       "%02x:%02x:%02x:%02x:%02x:%02x  (tag %s): ",
                       ident[i].addr[0], ident[i].addr[1], ident[i].addr[2],
                       ident[i].addr[3], ident[i].addr[4], ident[i].addr[5],
                       air[j].addr[0], air[j].addr[1], air[j].addr[2],
                       air[j].addr[3], air[j].addr[4], air[j].addr[5], ident[i].tag);
            if (install(blob)) {
                done++;
                if (!quiet) printf("installed\n");
            }
            break;
        }
    }

    if (!quiet) printf("%d mapping(s) installed.\n", done);
    return done;
}

int main(int argc, char **argv)
{
    int loop = 0, quiet = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--loop") == 0) loop = 1;
        else if (strcmp(argv[i], "--quiet") == 0) quiet = 1;
        else { printf("usage: sudo %s [--loop] [--quiet]\n", argv[0]); return 2; }
    }

    if (!loop) {
        int done = runOnce(quiet);
        if (done == 0 && !quiet)
            printf("\nNothing joined. Either no identity in the log shares a tag with an\n"
                   "address on the air (leave the devices nearby for a minute), or this\n"
                   "build of macOS no longer prints the tag.\n");
        return done > 0 ? 0 : 1;
    }

    /* The addresses rotate; a mapping is only true until they do. */
    for (;;) {
        int done = runOnce(quiet);
        if (!quiet) { printf("---\n"); fflush(stdout); }
        (void)done;
        sleep(60);
    }
}
