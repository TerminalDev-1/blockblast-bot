"""Pick which connection the bots use to reach the tablet: USB when it's plugged in
(faster screenshots), wireless debugging otherwise.

    python adbwifi.py setup    # once, with the USB cable plugged in
    python adbwifi.py          # show which connection would be used

`setup` switches the tablet's adb to listen on Wi-Fi (port 5555), connects to it
and remembers the address in ~/.adb-wifi-tablet. The tablet forgets this mode when
it reboots; run `setup` again with the cable after a reboot.

Bots call `ensure()` at startup and whenever adb stops answering. It sets
ANDROID_SERIAL so every later `adb` command goes to the chosen connection.
"""
import os, re, subprocess, sys, time

ADDR_FILE = os.path.join(os.path.expanduser('~'), '.adb-wifi-tablet')
PORT = 5555


def _adb(*args, timeout=15):
    env = {k: v for k, v in os.environ.items() if k != 'ANDROID_SERIAL'}
    return subprocess.run(['adb', *map(str, args)], capture_output=True, text=True, timeout=timeout, env=env)


def devices():
    lines = _adb('devices').stdout.splitlines()[1:]
    return [l.split()[0] for l in lines if l.strip().endswith('\tdevice') or l.strip().endswith(' device')]


def is_wireless(serial):
    return ':' in serial or '._adb' in serial


def saved_address():
    try:
        with open(ADDR_FILE, encoding='utf-8') as f: return f.read().strip() or None
    except OSError:
        return None


def setup():
    usb = [d for d in devices() if not is_wireless(d)]
    if not usb: sys.exit('Plug the tablet in by USB (with USB debugging on) and run this again.')
    serial = usb[0]
    out = subprocess.run(['adb', '-s', serial, 'shell', 'ip', '-f', 'inet', 'addr', 'show', 'wlan0'],
                         capture_output=True, text=True, timeout=15).stdout
    m = re.search(r'inet (\d+\.\d+\.\d+\.\d+)', out)
    if not m: sys.exit('The tablet does not seem to be on Wi-Fi.')
    addr = f'{m.group(1)}:{PORT}'
    print(f'Tablet Wi-Fi address: {addr}. Switching adb to wireless mode...')
    subprocess.run(['adb', '-s', serial, 'tcpip', str(PORT)], timeout=20)
    for _ in range(10):
        time.sleep(1.5)
        if 'connected' in _adb('connect', addr, timeout=10).stdout and addr in devices(): break
    else:
        sys.exit(f'Could not connect to {addr}. Is the PC on the same Wi-Fi network as the tablet?')
    with open(ADDR_FILE, 'w', encoding='utf-8') as f: f.write(addr)
    print(f'Connected over Wi-Fi ({addr}). You can unplug the cable; the bots will reconnect by themselves.')


def ensure(wait=None):
    """Choose a connected tablet (USB first, then Wi-Fi, reconnecting to the saved
    address if needed), export it as ANDROID_SERIAL and return it. Waits as long as
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
        addr = saved_address()
        if addr: _adb('connect', addr, timeout=10)
        if not said:
            print('Tablet not reachable; waiting for USB or Wi-Fi'
                  + (f' ({addr})' if addr else ' (run "python adbwifi.py setup" once with the cable)') + '...', flush=True)
            said = True
        if wait is not None and time.time() - t0 > wait: raise RuntimeError('Tablet not reachable over USB or Wi-Fi.')
        time.sleep(3)


if __name__ == '__main__':
    if sys.argv[1:] == ['setup']: setup()
    else: print('Would use:', ensure(wait=10))
