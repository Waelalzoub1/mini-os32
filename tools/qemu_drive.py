#!/usr/bin/env python3
"""Drive mini-os32 under qemu-system-i386 headlessly.

Keyboard input goes through the QEMU monitor (sendkey), console output is
captured from the kernel's COM1 serial mirror. Usage:

    qemu_drive.py IMAGE [--cmd "line" ...] [--screendump out.png] [--timeout S]

Each --cmd is typed at the shell prompt (or at whatever prompt the running
program shows) and the driver waits for the next "/> " prompt before typing
the next one. Prints the full serial transcript to stdout.
"""
import argparse, os, socket, subprocess, sys, tempfile, time

KEYMAP = {
    ' ': 'spc', '\n': 'ret', '.': 'dot', ',': 'comma', '-': 'minus', '/': 'slash',
    ';': 'semicolon', "'": 'apostrophe', '=': 'equal', '\\': 'backslash',
    '[': 'bracket_left', ']': 'bracket_right', '`': 'grave_accent', '\t': 'tab',
}
SHIFTED = {
    '!': '1', '@': '2', '#': '3', '$': '4', '%': '5', '^': '6', '&': '7', '*': '8',
    '(': '9', ')': '0', '_': 'minus', '+': 'equal', '{': 'bracket_left',
    '}': 'bracket_right', '|': 'backslash', ':': 'semicolon', '"': 'apostrophe',
    '<': 'comma', '>': 'dot', '?': 'slash', '~': 'grave_accent',
}

def keyname(ch):
    if ch in KEYMAP: return KEYMAP[ch]
    if ch in SHIFTED: return 'shift-' + SHIFTED[ch]
    if ch.isalpha():
        return ('shift-' + ch.lower()) if ch.isupper() else ch
    if ch.isdigit(): return ch
    raise ValueError('no key for %r' % ch)

class Monitor:
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        for _ in range(100):
            try:
                self.s.connect(path); break
            except OSError:
                time.sleep(0.1)
        self.s.settimeout(2)
        self._drain()
    def _drain(self):
        try:
            while True:
                d = self.s.recv(65536)
                if not d: break
                if d.endswith(b'(qemu) '): break
        except socket.timeout:
            pass
    def cmd(self, c):
        self.s.sendall((c + '\n').encode())
        self._drain()
    def type(self, text, hold=None):
        for ch in text:
            k = keyname(ch)
            self.cmd('sendkey %s%s' % (k, (' %d' % hold) if hold else ''))
            time.sleep(0.03)

class Session:
    def __init__(self, image, serial_log, mon_path, extra=()):
        self.serial_log = serial_log
        self.proc = subprocess.Popen(
            ['qemu-system-i386', '-hda', image, '-display', 'none', '-no-reboot',
             '-serial', 'file:' + serial_log,
             '-monitor', 'unix:%s,server,nowait' % mon_path] + list(extra),
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.mon = Monitor(mon_path)
        self.seen = 0
    def serial(self):
        try:
            with open(self.serial_log, 'rb') as f: return f.read().decode('latin-1')
        except FileNotFoundError:
            return ''
    def wait_for(self, marker, timeout=30):
        """Wait until `marker` appears in serial output after the last consumed point."""
        t0 = time.time()
        while time.time() - t0 < timeout:
            s = self.serial()
            i = s.find(marker, self.seen)
            if i >= 0:
                self.seen = i + len(marker)
                return True
            time.sleep(0.05)
        return False
    def send_line(self, line, wait_prompt='/> ', timeout=60):
        self.mon.type(line + '\n')
        if wait_prompt:
            return self.wait_for(wait_prompt, timeout)
        return True
    def screendump(self, png):
        ppm = png + '.ppm'
        self.mon.cmd('screendump ' + ppm)
        time.sleep(0.5)
        try:
            from PIL import Image
            Image.open(ppm).save(png); os.unlink(ppm)
        except Exception as e:
            print('screendump conversion failed:', e, file=sys.stderr)
    def close(self):
        try: self.mon.cmd('quit')
        except Exception: pass
        try: self.proc.wait(timeout=5)
        except Exception: self.proc.kill()

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('image')
    ap.add_argument('--cmd', action='append', default=[], help='line to type; prefix with "@" to not wait for a prompt')
    ap.add_argument('--screendump')
    ap.add_argument('--timeout', type=float, default=60)
    ap.add_argument('--settle', type=float, default=0.5, help='seconds to wait before screendump')
    a = ap.parse_args()
    tmp = tempfile.mkdtemp(prefix='qemu_drive_')
    ses = Session(a.image, os.path.join(tmp, 'serial.log'), os.path.join(tmp, 'mon.sock'))
    ok = ses.wait_for('/> ', timeout=30)
    if not ok:
        print('BOOT FAILED: no shell prompt on serial', file=sys.stderr)
        print(ses.serial()); ses.close(); sys.exit(2)
    rc = 0
    for c in a.cmd:
        if c.startswith('@'):
            ses.send_line(c[1:], wait_prompt=None)
        else:
            if not ses.send_line(c, timeout=a.timeout):
                print('TIMEOUT waiting for prompt after: %r' % c, file=sys.stderr); rc = 3; break
    time.sleep(a.settle)
    if a.screendump: ses.screendump(a.screendump)
    print(ses.serial())
    ses.close()
    sys.exit(rc)

if __name__ == '__main__':
    main()
