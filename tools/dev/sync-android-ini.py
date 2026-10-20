#!/usr/bin/env python3
"""Regenerate Engine/Config/android/Default.ini from the native file.

  tools/dev/sync-android-ini.py            rewrite the Android file
  tools/dev/sync-android-ini.py --check    exit 1 if it is not what this script would write

The Android app carries Engine/Config/android/Default.ini in its APK and installs it into the game folder's System/
before the game starts (Android/app/build.gradle, GameFolder.installSystemFiles), as the native run copies
Engine/Config's inis into a run tree. It is the native file with the differences listed in DELTAS below and nothing
else, so a change of the native defaults reaches the device without anybody copying it. Bytes are kept as they are
(Latin-1) and the file is LF: a CR in the native file stops the script.
"""
import os, re, sys
here = os.path.dirname(os.path.abspath(__file__))
cfg = os.path.join(here, '..', '..', 'Engine', 'Config')
out_dir = os.path.join(cfg, 'android')

# (file, section, key, native value expected, Android value)
DELTAS = [
    # The window is the whole screen anyway (the client makes it so on Android).
    ('Default.ini', 'NSDLDrv.NSDLClient', 'StartupFullscreen', 'False', 'True'),
    # A phone is played with its pads and the on-screen pad: the mouse is left out (also the compiled default on
    # Android).
    ('Default.ini', 'NSDLDrv.NSDLClient', 'UseMouse', 'True', 'False'),
]

def build(name):
    data = open(os.path.join(cfg, name), 'rb').read().decode('latin-1')
    if '\r' in data:
        sys.exit('%s has CR line endings: Engine/Config is LF' % name)
    lines = data.split('\n')
    for f, section, key, native, android in DELTAS:
        if f != name:
            continue
        cur = None; done = False
        for i, line in enumerate(lines):
            m = re.match(r'\[(.+)\]\s*$', line)
            if m:
                cur = m.group(1); continue
            if cur == section and line.startswith(key + '='):
                if line != key + '=' + native:
                    sys.exit('%s [%s] %s: the native file has %r, this script expects %r: update DELTAS' % (name, section, key, line, key + '=' + native))
                lines[i] = key + '=' + android
                done = True
                break
        if not done:
            sys.exit('%s: [%s] %s not found' % (name, section, key))
    return '\n'.join(lines).encode('latin-1')

def main():
    check = '--check' in sys.argv
    bad = 0
    if not check:
        os.makedirs(out_dir, exist_ok=True)
    for name in ('Default.ini',):
        want = build(name)
        path = os.path.join(out_dir, name)
        have = open(path, 'rb').read() if os.path.exists(path) else b''
        if have == want:
            print('android/%s is up to date' % name)
        elif check:
            print('android/%s is not the native file with the Android deltas (run tools/dev/sync-android-ini.py)' % name); bad = 1
        else:
            tmp = path + '.tmp'
            open(tmp, 'wb').write(want); os.replace(tmp, path)
            print('android/%s rewritten (%d bytes)' % (name, len(want)))
    sys.exit(bad)

main()
