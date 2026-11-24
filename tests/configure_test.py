#!/usr/bin/env python3
"""Check efi/configure.py against a synthetic config.plist: it edits the boot
configuration, so it should not meet a real one untested."""
import plistlib, subprocess, sys, os, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
CONFIGURE = os.path.join(HERE, '..', 'efi', 'configure.py')
GUID = '7C436110-AB2A-4BBB-A880-FE41995C9F82'
ARGS = 'debug=0x100 keepsyms=1 -lilubetaall -wegnoegpu'


def fresh():
    return {
        'Kernel': {'Add': [
            {'BundlePath': 'Lilu.kext', 'Enabled': True, 'ExecutablePath': 'Contents/MacOS/Lilu'},
            {'BundlePath': 'VirtualSMC.kext', 'Enabled': True},
            {'BundlePath': 'AirPort_RTW89.kext', 'Enabled': True},
        ], 'Block': []},
        'NVRAM': {'Add': {GUID: {'boot-args': ARGS}}},
    }


def run(mode, config, tmp, expect_fail=False, stage=None):
    src, dst = os.path.join(tmp, 'in.plist'), os.path.join(tmp, 'out.plist')
    with open(src, 'wb') as f:
        plistlib.dump(config, f)
    cmd = [sys.executable, CONFIGURE, mode, src, dst]
    if stage is not None:
        cmd.append(stage)
    r = subprocess.run(cmd, capture_output=True, text=True)
    if expect_fail:
        assert r.returncode != 0, 'should have refused: ' + r.stdout
        return None
    assert r.returncode == 0, r.stderr
    with open(dst, 'rb') as f:
        return plistlib.load(f)


def entry(config):
    return next(e for e in config['Kernel']['Add'] if e['BundlePath'] == 'BroadTek.kext')


def main():
    with tempfile.TemporaryDirectory() as tmp:
        one = run('install', fresh(), tmp)
        order = [e['BundlePath'] for e in one['Kernel']['Add']]
        assert order.index('BroadTek.kext') == order.index('Lilu.kext') + 1, order
        e = entry(one)
        assert e['Enabled'] and e['MinKernel'] == '22.0.0'
        assert e['ExecutablePath'] == 'Contents/MacOS/BroadTek'
        assert one['NVRAM']['Add'][GUID]['boot-args'] == ARGS + ' -broadtekdbg'

        two = run('install', one, tmp)
        assert [x['BundlePath'] for x in two['Kernel']['Add']] == order, 'install must be repeatable'
        assert two['NVRAM']['Add'][GUID] == one['NVRAM']['Add'][GUID]

        off = run('uninstall', two, tmp)
        assert entry(off)['Enabled'] is False
        assert off['NVRAM']['Add'][GUID]['boot-args'] == ARGS, 'boot-args must go back as they were'
        others = [x['BundlePath'] for x in off['Kernel']['Add'] if x['BundlePath'] != 'BroadTek.kext']
        assert others == ['Lilu.kext', 'VirtualSMC.kext', 'AirPort_RTW89.kext'], others

        assert entry(run('install', off, tmp))['Enabled'] is True, 'install must switch it back on'

        # The stages, and the switch between them. Each one must leave exactly
        # its own boot-arg behind: a config carrying two of them would be a
        # plugin whose behaviour depends on which the kext happens to read.
        args = lambda c: c['NVRAM']['Add'][GUID]['boot-args'].split()

        ev = run('install', fresh(), tmp, stage='events')
        assert '-broadtekrwev' in args(ev), args(ev)
        assert '-broadtekrw' not in args(ev), 'the full flag must not be set too'
        assert '-broadtekdbg' in args(ev)

        full = run('install', ev, tmp, stage='full')
        assert '-broadtekrw' in args(full), args(full)
        assert '-broadtekrwev' not in args(full), 'moving up a stage must drop the old flag'

        back = run('install', full, tmp, stage='events')
        assert '-broadtekrwev' in args(back) and '-broadtekrw' not in args(back), args(back)

        obs = run('install', back, tmp, stage='off')
        assert not [a for a in args(obs) if a.startswith('-broadtekrw')], args(obs)
        assert '-broadtekdbg' in args(obs), 'observing still logs'

        assert '-broadtekobserve' in args(obs)
        assert '-broadtekobserve' not in args(run('install', obs, tmp)), 'default restores advertising'
        assert '-broadtekobserve' not in args(run('install', obs, tmp, stage='full'))

        gone = run('uninstall', full, tmp)
        assert not [a for a in args(gone) if a.startswith('-broadtek')], args(gone)

        run('install', fresh(), tmp, expect_fail=True, stage='sideways')
        run('uninstall', fresh(), tmp, expect_fail=True, stage='full')

        # Intel: BTLEAdvFixup goes off with install and back on with
        # uninstall, but only when it was install that turned it off.
        rival = lambda c: next(x for x in c['Kernel']['Add'] if x['BundlePath'] == 'BTLEAdvFixup.kext')
        withfix = fresh()
        withfix['Kernel']['Add'].append({'BundlePath': 'BTLEAdvFixup.kext', 'Enabled': True})
        on = run('install', withfix, tmp)
        assert rival(on)['Enabled'] is False, 'install must switch BTLEAdvFixup off'
        assert rival(run('uninstall', on, tmp))['Enabled'] is True, 'uninstall must switch it back on'
        mine = fresh()
        mine['Kernel']['Add'].append({'BundlePath': 'BTLEAdvFixup.kext', 'Enabled': False})
        assert rival(run('uninstall', run('install', mine, tmp), tmp))['Enabled'] is False, \
            'a BTLEAdvFixup the user had off must stay off'

        no_lilu = fresh()
        no_lilu['Kernel']['Add'] = [x for x in no_lilu['Kernel']['Add']
                                    if x['BundlePath'] != 'Lilu.kext']
        run('install', no_lilu, tmp, expect_fail=True)
    print('efi/configure.py: all checks passed')


if __name__ == '__main__':
    main()
