# BroadTek: LL Privacy for Bluetooth controllers that lack it.
#
#   make test     # the core in userspace, ASan/UBSan
#   make kext     # build/out/BroadTek.kext (runs the test first)
#   make release  # build/out/BroadTek-$(VERSION).zip: the kext, the Watch key
#                 # helper and its install.sh / uninstall.sh
#
# Loading the kext is never done from here; see README.md.

MKSDK     := third_party/MacKernelSDK
LILU      := third_party/Lilu/Lilu
CORE      := src/core
PLUGIN    := src/plugin
BUILD     := build
OUT       := $(BUILD)/out
VERSION   := 0.3.2

SDK   := $(shell xcrun --show-sdk-path 2>/dev/null)
ARCH  := -arch x86_64
MINOS := -mmacosx-version-min=13.0
CC    := xcrun clang
CXX   := xcrun clang++

CORE_SRCS := $(CORE)/bt_adv.c $(CORE)/bt_aes.c $(CORE)/bt_hci.c $(CORE)/bt_smp.c
CORE_HDRS := $(CORE)/bt_core.h

# Which build is actually running is not a rhetorical question: a kext copied
# from the wrong repository, or not copied at all, looks exactly like one whose
# hooks did nothing. The commit goes into the binary and out again as
# broadtek-build in the IO registry.
BUILD_ID := $(shell git -C $(dir $(lastword $(MAKEFILE_LIST))) describe --always --dirty --abbrev=7 2>/dev/null || echo unknown)

DEFS := -DPRODUCT_NAME=BroadTek -DMODULE_VERSION=$(VERSION) -DPLUGIN_VERSION='"$(VERSION)"' \
        -DBROADTEK_BUILD='"$(BUILD_ID)"'

# Lilu compiles DBGLOG out unless DEBUG is defined, and this build exists to be
# read. DEBUG=0 builds the quiet version.
DEBUG ?= 1
ifeq ($(DEBUG),1)
DEFS += -DDEBUG=1
endif

KEXT_CFLAGS := $(ARCH) $(MINOS) -isysroot $(SDK) -nostdinc \
               -std=gnu11 -O2 -fno-strict-aliasing -fwrapv -mkernel \
               -fno-builtin -fno-common -fno-stack-protector -fno-omit-frame-pointer \
               -Wframe-larger-than=1024 \
               -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
               -I$(MKSDK)/Headers -I$(CORE) -Wall -Wextra -Werror -MMD -MP

KEXT_CXXFLAGS := $(ARCH) $(MINOS) -isysroot $(SDK) -nostdinc \
               -std=gnu++17 -O2 -fno-strict-aliasing -mkernel -fapple-kext -fno-rtti \
               -fno-exceptions -fno-builtin -fno-common -fno-stack-protector \
               -Wframe-larger-than=1024 \
               -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
               -I$(MKSDK)/Headers -isystem $(LILU) -I$(CORE) $(DEFS) \
               -Wall -Werror -MMD -MP

HOST_CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Werror -I$(CORE) \
               -fsanitize=address,undefined -fno-sanitize-recover=all

OBJS := $(BUILD)/obj/bt_adv.o $(BUILD)/obj/bt_aes.o $(BUILD)/obj/bt_hci.o $(BUILD)/obj/bt_smp.o \
        $(BUILD)/obj/kern_broadtek.o $(BUILD)/obj/kern_config.o $(BUILD)/obj/kern_usbclass.o \
        $(BUILD)/obj/kern_start.o \
        $(BUILD)/obj/plugin_start.o $(BUILD)/obj/kmod_info.o

BUNDLE := $(OUT)/BroadTek.kext
BIN    := $(BUNDLE)/Contents/MacOS/BroadTek

.PHONY: all test kext release clean probe irk-install dongle-probe nearby-probe map-install

all: kext

# A diagnostic, not part of the kext: can anything here read bluetoothd's
# keychain items (docs/STATE.md §3). Signed locally with the Apple-private
# entitlement it needs, which only has a chance because this machine boots
# with AMFI entitlement enforcement off.
probe: $(OUT)/keychain_probe

# Reads the IRK out of the keychain and hands it to the kext. Same entitlement
# as the probe, and the same rule: the key is never printed.
irk-install: $(OUT)/irk_install

$(OUT)/irk_install: tools/irk_install.c tools/keychain_probe.entitlements
	@mkdir -p $(OUT)
	@$(CC) -std=gnu11 -O1 -g -Wall -Wextra -Werror \
	    -framework CoreFoundation -framework Security -framework IOKit \
	    tools/irk_install.c -o $@
	@codesign -s - --entitlements tools/keychain_probe.entitlements -f $@ 2>/dev/null
	@echo "  TOOL  $@  (run: sudo $@)"

# Joins the kext's air-side capture to bluetoothd's log and hands the kext the
# private address a device is using now. No keychain, so no entitlement.
map-install: $(OUT)/map_install

$(OUT)/map_install: tools/map_install.c
	@mkdir -p $(OUT)
	@$(CC) -std=gnu11 -O1 -g -Wall -Wextra -Werror \
	    -framework CoreFoundation -framework IOKit \
	    tools/map_install.c -o $@
	@echo "  TOOL  $@  (run: sudo $@)"

# Does a stored Rapport identity key produce the NearbyInfo auth tag macOS
# matches the iPhone by? One access group, and no key is ever printed.
nearby-probe: $(OUT)/nearby_probe

$(OUT)/nearby_probe: tools/nearby_probe.c tools/nearby_probe.entitlements
	@mkdir -p $(OUT)
	@$(CC) -std=gnu11 -O1 -g -Wall -Wextra -Werror \
	    -framework CoreFoundation -framework Security \
	    tools/nearby_probe.c -o $@
	@codesign -s - --entitlements tools/nearby_probe.entitlements -f $@ 2>/dev/null
	@echo "  TOOL  $@  (run: sudo $@ <rpa> <tag>)"

# bluetoothd's external-dongle test, reproduced (docs/LE_ADVERTISING.md). Says
# which USB node bluetoothd settles on, which decides whose bDeviceClass has to
# change. Read-only, needs no privileges and no entitlement.
dongle-probe: $(OUT)/dongle_probe

$(OUT)/dongle_probe: tools/dongle_probe.c
	@mkdir -p $(OUT)
	@$(CC) -std=gnu11 -O1 -g -Wall -Wextra -Werror \
	    -framework CoreFoundation -framework IOKit \
	    tools/dongle_probe.c -o $@
	@echo "  TOOL  $@  (run: $@)"

$(OUT)/keychain_probe: tools/keychain_probe.c tools/keychain_probe.entitlements
	@mkdir -p $(OUT)
	@$(CC) -std=gnu11 -O1 -g -Wall -Wextra -Werror \
	    -framework CoreFoundation -framework Security \
	    tools/keychain_probe.c -o $@
	@codesign -s - --entitlements tools/keychain_probe.entitlements -f $@ 2>/dev/null
	@echo "  PROBE $@  (run: sudo $@)"

test: $(OUT)/hosttest
	@$(OUT)/hosttest
	@python3 tests/configure_test.py

$(OUT)/hosttest: tests/hosttest.c $(CORE_SRCS) $(CORE_HDRS)
	@mkdir -p $(OUT)
	@$(CC) $(HOST_CFLAGS) tests/hosttest.c $(CORE_SRCS) -o $@
	@echo "  TEST $@"

$(BUILD)/obj/%.o: $(CORE)/%.c
	@mkdir -p $(dir $@)
	@echo "  CC   core/$*.c"
	@$(CC) $(KEXT_CFLAGS) -c $< -o $@

# BROADTEK_BUILD is baked into kern_broadtek.o, so that object has to be
# rebuilt when the commit changes and not only when the source does --
# otherwise install.sh reports one build and the kext carries another.
GIT_STATE := $(wildcard .git/HEAD .git/index)

$(BUILD)/obj/kern_broadtek.o: $(PLUGIN)/kern_broadtek.cpp $(GIT_STATE)
	@mkdir -p $(dir $@)
	@echo "  CXX  plugin/kern_broadtek.cpp"
	@$(CXX) $(KEXT_CXXFLAGS) -c $< -o $@

$(BUILD)/obj/%.o: $(PLUGIN)/%.cpp
	@mkdir -p $(dir $@)
	@echo "  CXX  plugin/$*.cpp"
	@$(CXX) $(KEXT_CXXFLAGS) -c $< -o $@

$(BUILD)/obj/kmod_info.o: $(PLUGIN)/kmod_info.c
	@mkdir -p $(dir $@)
	@echo "  CC   plugin/kmod_info.c"
	@$(CC) $(filter-out -I$(CORE),$(KEXT_CFLAGS)) $(DEFS) -c $< -o $@

$(BUILD)/obj/plugin_start.o: $(LILU)/Library/plugin_start.cpp
	@mkdir -p $(dir $@)
	@echo "  CXX  Lilu/Library/plugin_start.cpp"
	@$(CXX) $(KEXT_CXXFLAGS) -c $< -o $@

kext: test $(OBJS) $(PLUGIN)/Info.plist
	@rm -rf $(BUNDLE)
	@mkdir -p $(dir $(BIN))
	@sed -e 's/$$(EXECUTABLE_NAME)/BroadTek/g' -e 's/$$(PRODUCT_NAME)/BroadTek/g' \
	    -e 's/$$(PRODUCT_BUNDLE_IDENTIFIER)/org.franciumos.BroadTek/g' \
	    -e 's/$$(MODULE_VERSION)/$(VERSION)/g' $(PLUGIN)/Info.plist > $(BUNDLE)/Contents/Info.plist
	@plutil -lint -s $(BUNDLE)/Contents/Info.plist
	@! grep -q '\$$(' $(BUNDLE)/Contents/Info.plist
	@$(CXX) $(ARCH) $(MINOS) -isysroot $(SDK) -nostdlib -Xlinker -kext \
	    -L$(MKSDK)/Library/x86_64 $(OBJS) -lkmod -o $(BIN)
	@codesign --force --sign - $(BUNDLE) 2>/dev/null || true
	@echo "  KEXT $(BUNDLE)"

# The release zip. The kext goes into the EFI; install.sh is run afterwards,
# from macOS, once the kext has booted, and adds the Watch key helper. The
# helper is stripped: its debug map would carry the build machine's paths.
REL := $(OUT)/BroadTek-$(VERSION)
release: kext irk-install
	@rm -rf $(REL) $(REL).zip
	@mkdir -p $(REL)/Helper
	@cp -R $(BUNDLE) $(REL)/
	@cp $(OUT)/irk_install $(REL)/Helper/broadtek-irk-install
	@strip -S -x $(REL)/Helper/broadtek-irk-install
	@codesign -s - --entitlements tools/keychain_probe.entitlements -f $(REL)/Helper/broadtek-irk-install 2>/dev/null
	@cp tools/irk_session.sh $(REL)/Helper/broadtek-irk-session
	@cp tools/org.franciumos.broadtek.irk.plist $(REL)/Helper/
	@cp tools/keychain_probe.entitlements $(REL)/Helper/keychain.entitlements
	@cp dist/install.sh dist/uninstall.sh LICENSE $(REL)/
	@chmod 755 $(REL)/install.sh $(REL)/uninstall.sh $(REL)/Helper/broadtek-irk-install $(REL)/Helper/broadtek-irk-session
	@! LC_ALL=C grep -r -a -l -i "/Users/" $(REL) || { echo "a build path leaked into the release" >&2; exit 1; }
	@cd $(OUT) && COPYFILE_DISABLE=1 zip -q -r -X BroadTek-$(VERSION).zip BroadTek-$(VERSION)
	@echo "  ZIP  $(REL).zip"

clean:
	rm -rf $(BUILD)

-include $(OBJS:.o=.d)
