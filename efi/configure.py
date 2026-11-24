#!/usr/bin/env python3
"""Add BroadTek to an OpenCore config.plist, or switch it off again.

Usage: python3 efi/configure.py install|uninstall IN_CONFIG OUT_CONFIG [STAGE]

install: Kernel -> Add gets BroadTek.kext, after Lilu.kext (it is a Lilu
plugin and will not do anything if it loads first), and the boot-arg
-broadtekdbg is added, which turns on its logging.

STAGE says how much the plugin may change, and defaults to "default":

  default standard advertising fixes
  off     observe only: nothing is rewritten, the hooks just report
  events  rewrite events only -- the features rewrite, nothing else can
          fire because no command reaches the core to populate its state
  full    rewrite commands as well

Two further stages are an experiment rather than a rewrite stage, and they
leave the HCI path alone (see docs/LE_ADVERTISING.md):

  classdry  list the USB nodes bluetoothd's dongle test would take, and change
            nothing
  class     change their bDeviceClass so the test does not take them, which is
            what lets the LE advertising stack start

  advaddr   observe only, plus: give each extended advertising set bluetoothd's
            private address (LE Set Advertising Set Random Address, 0x2035)
            before it is enabled. The one command BroadTek sends itself.

  fulladv   both of the above: rewrite commands *and* address the advertising
            sets. The two the iPhone needs together -- Handoff goes out from
            the sets, and reaching the phone needs the accept-list rewrite.

Whichever stage is chosen, the boot-args for the others are removed, so
switching between them is just another install.

uninstall: the entry is switched off (not removed) and -broadtekdbg and
-broadtekrw are taken out of the boot-args, so OpenCore sees the config as it
was. install switches it back on.

Nothing else in the file is touched.
"""
import plistlib
import sys

MIN_KERNEL = '22.0.0'                    # Ventura
BUNDLE = 'BroadTek.kext'
BOOT_ARG = '-broadtekdbg'
STAGES = {'default': None, 'off': '-broadtekobserve', 'events': '-broadtekrwev', 'full': '-broadtekrw',
          'classdry': '-broadtekclassdry', 'class': '-broadtekclass',
          'advaddr': '-broadtekadvaddr',
          'fulladv': ('-broadtekrw', '-broadtekadvaddr')}
OUR_ARGS = ('-broadtekdbg', '-broadtekrw', '-broadtekrwev', '-broadtekoff',
            '-broadtekclass', '-broadtekclassdry', '-broadtekadvaddr', '-broadtekobserve')
RIVAL = 'BTLEAdvFixup.kext'
RIVAL_MARK = ' (off for BroadTek)'
NVRAM_GUID = '7C436110-AB2A-4BBB-A880-FE41995C9F82'


def entry():
    return {
        'Arch': 'x86_64', 'BundlePath': BUNDLE,
        'Comment': 'Bluetooth: software LL Privacy (BroadTek)', 'Enabled': True,
        'ExecutablePath': 'Contents/MacOS/BroadTek', 'MaxKernel': '',
        'MinKernel': MIN_KERNEL, 'PlistPath': 'Contents/Info.plist',
    }


def boot_args(config):
    """The dict holding boot-args, and the key, or (None, None)."""
    add = config.get('NVRAM', {}).get('Add', {}).get(NVRAM_GUID)
    if add is None or 'boot-args' not in add:
        return None, None
    return add, 'boot-args'


def set_boot_args(config, changes, wanted, unwanted):
    add, key = boot_args(config)
    if add is None:
        print('note: no boot-args in NVRAM -> Add -> %s; add %s by hand'
              % (NVRAM_GUID, ' '.join(wanted) or '(nothing)'))
        return
    args = add[key].split()
    before = list(args)
    args = [a for a in args if a not in unwanted]
    for w in wanted:
        if w not in args:
            args.append(w)
    if args != before:
        add[key] = ' '.join(args)
        changes.append('boot-args: ' + add[key])


def install(config, changes, stage='default'):
    add = config['Kernel']['Add']
    paths = [e.get('BundlePath') for e in add]
    if 'Lilu.kext' not in paths:
        sys.exit('Lilu.kext is not in Kernel -> Add; BroadTek is a Lilu plugin')

    if BUNDLE in paths:
        ours = add[paths.index(BUNDLE)]
        if not ours.get('Enabled'):
            ours['Enabled'] = True
            changes.append('switched on ' + BUNDLE)
    else:
        ours = entry()
        changes.append('Kernel -> Add: ' + BUNDLE)

    # After Lilu, whatever order OC Snapshot left behind.
    old_order = [e.get('BundlePath') for e in add]
    rest = [e for e in add if e is not ours]
    lilu = next(k for k, e in enumerate(rest) if e.get('BundlePath') == 'Lilu.kext')
    add[:] = rest[:lilu + 1] + [ours] + rest[lilu + 1:]
    if [e.get('BundlePath') for e in add if e.get('BundlePath') in old_order] != old_order:
        changes.append('Kernel -> Add order: BroadTek.kext after Lilu.kext')

    # BTLEAdvFixup does the same job for Intel and patches the same bluetoothd
    # code; with both loaded each would undo the other's guesses.
    for e in add:
        if e.get('BundlePath') == RIVAL and e.get('Enabled'):
            e['Enabled'] = False
            e['Comment'] = (e.get('Comment') or '') + RIVAL_MARK
            changes.append('switched off %s (BroadTek does its job)' % RIVAL)

    wanted = [BOOT_ARG]
    chosen = STAGES[stage]
    if isinstance(chosen, tuple):
        wanted.extend(chosen)
    elif chosen is not None:
        wanted.append(chosen)
    set_boot_args(config, changes, wanted, [a for a in OUR_ARGS if a not in wanted])


def uninstall(config, changes):
    for e in config['Kernel']['Add']:
        if e.get('BundlePath') == BUNDLE and e.get('Enabled'):
            e['Enabled'] = False
            changes.append('switched off ' + BUNDLE)
        elif e.get('BundlePath') == RIVAL and RIVAL_MARK in (e.get('Comment') or ''):
            e['Enabled'] = True
            e['Comment'] = e['Comment'].replace(RIVAL_MARK, '')
            changes.append('switched %s back on' % RIVAL)
    set_boot_args(config, changes, [], OUR_ARGS)


def main():
    if len(sys.argv) not in (4, 5) or sys.argv[1] not in ('install', 'uninstall'):
        sys.exit(__doc__)
    mode, src, dst = sys.argv[1:4]
    stage = sys.argv[4] if len(sys.argv) == 5 else 'default'
    if stage not in STAGES:
        sys.exit('unknown stage %r: expected one of %s' % (stage, ', '.join(STAGES)))
    if mode == 'uninstall' and stage not in ('default', 'off'):
        sys.exit('uninstall takes no stage')
    with open(src, 'rb') as f:
        config = plistlib.load(f)
    changes = []
    if mode == 'install':
        install(config, changes, stage)
    else:
        uninstall(config, changes)
    with open(dst, 'wb') as f:
        plistlib.dump(config, f, sort_keys=False)
    for c in changes:
        print('  ' + c)
    if not changes:
        print('  nothing to change')


if __name__ == '__main__':
    main()
