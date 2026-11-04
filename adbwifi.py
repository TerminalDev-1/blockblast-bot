"""Pick which connection the bots use to reach the tablet: USB when it's plugged in
(faster screenshots), Android's "Wireless debugging" otherwise.

    python adbwifi.py pair     # once: pair this PC with the tablet over Wi-Fi
    python adbwifi.py          # show which connection would be used

Pairing is one-time. After it, whenever the tablet's Wireless debugging is on and it
is on the same Wi-Fi as the PC, adb finds it on the network by itself (mDNS), so
nothing has to be re-run after a reboot.

Bots call `ensure()` at startup and whenever adb stops answering. It sets
ANDROID_SERIAL so every later `adb` command goes to the chosen connection.
"""
import os, re, subprocess, sys, time


def _adb(*args, timeout=15):
    env = {k: v for k, v in os.environ.items() if k != 'ANDROID_SERIAL'}
    return subprocess.run(['adb', *map(str, args)], capture_output=True, text=True, timeout=timeout, env=env)


def devices():
    lines = _adb('devices').stdout.splitlines()[1:]
    return [l.split()[0] for l in lines if l.split()[1:2] == ['device']]


def is_wireless(serial):
    return ':' in serial or '._adb' in serial


def discover():
    """Connect to any paired tablet that announces itself on the network."""
    out = _adb('mdns', 'services', timeout=10).stdout
    for addr in re.findall(r'_adb-tls-connect\._tcp\.?\s+(\d+\.\d+\.\d+\.\d+:\d+)', out):
        _adb('connect', addr, timeout=10)


def pair():
    print('One-time pairing. On the tablet:\n'
          '  Settings > Additional settings > Developer options > Wireless debugging: turn it on,\n'
          '  then tap "Pair device with pairing code".\n'
          'The tablet shows an IP address & port and a 6-digit code.')
    addr = input('IP address & port shown under the code (e.g. 192.168.1.20:37099): ').strip()
    code = input('6-digit pairing code: ').strip()
    out = _adb('pair', addr, code, timeout=30)
    print((out.stdout + out.stderr).strip())
    if 'Successfully paired' not in out.stdout:
        sys.exit('Pairing failed. Check that the PC and tablet are on the same Wi-Fi and try again.')
    for _ in range(10):
        discover(); time.sleep(2)
        wifi = [d for d in devices() if is_wireless(d)]
        if wifi:
            print(f'Connected over Wi-Fi ({wifi[0]}). From now on the bots find the tablet by themselves.'); return
    print('Paired. The connection will appear within a minute; the bots will pick it up.')


def ensure(wait=None):
    """Choose a connected tablet (USB first, then Wi-Fi, discovering it on the
    network if needed), export it as ANDROID_SERIAL and return it. Waits as long as
    it takes (the tablet may be charging after running flat) unless `wait` seconds
    are given."""
    t0 = time.time(); said = False
    while True:
        devs = devices()
        usb = [d for d in devs if not is_wireless(d)]
        wifi = [d for d in devs if is_wireless(d)]
        pick = (usb or wifi or [None])[0]
        if pick:
            if os.environ.get('ANDROID_SERIAL') != pick:
                print(f'Using tablet over {"Wi-Fi" if is_wireless(pick) else "USB"} ({pick}).', flush=True)
            os.environ['ANDROID_SERIAL'] = pick
            return pick
        discover()
        if not said:
            print('Waiting for the tablet (USB cable, or Wireless debugging on the same Wi-Fi)...', flush=True)
            said = True
        if wait is not None and time.time() - t0 > wait: raise RuntimeError('Tablet not reachable over USB or Wi-Fi.')
        time.sleep(3)


def open_app(package):
    """Bring the game to the front (launching it if needed)."""
    _adb('-s', os.environ.get('ANDROID_SERIAL', ''), 'shell', 'monkey', '-p', package,
         '-c', 'android.intent.category.LAUNCHER', '1', timeout=20)


if __name__ == '__main__':
    if sys.argv[1:] == ['pair']: pair()
    else: print('Would use:', ensure(wait=10))
