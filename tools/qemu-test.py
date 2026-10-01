#!/usr/bin/env python3
"""Boots build/aegis.img in QEMU and drives it from a script.

The serial port only carries output, so input goes through the QEMU monitor
(keyboard, mouse and device hot-plug). Each step is one argument:

    TEXT                 type TEXT and press Enter
    @type:TEXT           type TEXT without Enter
    @key:QCODE           press a key or combination (e.g. ctrl-c, f13, kp_5)
    @wait:TEXT           wait until TEXT appears in the serial output
    @expect:TEXT         like @wait, but fail the run if it never appears
    @reject:TEXT         fail the run if TEXT appears anywhere in the output
    @sleep:SECONDS       pause
    @shot:FILE.png       save a screenshot
    @mouse:DX,DY[,DZ]    move the mouse (DZ is the wheel)
    @button:MASK         set mouse buttons (1 left, 2 right, 4 middle; 0 releases)
    @hmp:COMMAND         run a QEMU monitor command (e.g. device_add usb-mouse,id=m1)
    @abs:X,Y             move an absolute pointer (--tablet) to screen pixel X,Y
    @click:X,Y[,BUTTON]  move there and click (BUTTON: left, right, middle)
    @drag:X1,Y1,X2,Y2    press the left button at X1,Y1, move to X2,Y2 and release
    @login:USER:PASSWORD wait for the login prompt and log in

Examples:
    tools/qemu-test.py @login:user:aegis 'ls /' '@expect:bin'
    tools/qemu-test.py --usb @login:user:aegis 'sudo evtest 5' aegis @mouse:10,0
"""

import argparse
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OVMF_CODE = "/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS = "/usr/share/OVMF/OVMF_VARS_4M.fd"

SHIFTED = {
    '!': '1', '@': '2', '#': '3', '$': '4', '%': '5', '^': '6', '&': '7', '*': '8',
    '(': '9', ')': '0', '_': 'minus', '+': 'equal', '{': 'bracket_left',
    '}': 'bracket_right', '|': 'backslash', ':': 'semicolon', '"': 'apostrophe',
    '~': 'grave_accent', '<': 'comma', '>': 'dot', '?': 'slash',
}
PLAIN = {
    ' ': 'spc', '\n': 'ret', '-': 'minus', '=': 'equal', '[': 'bracket_left',
    ']': 'bracket_right', '\\': 'backslash', ';': 'semicolon', "'": 'apostrophe',
    '`': 'grave_accent', ',': 'comma', '.': 'dot', '/': 'slash', '\t': 'tab',
}
ANSI = re.compile(r'\x1b\[[0-9;=?]*[A-Za-z]')


def qcode(ch):
    if ch in PLAIN:
        return PLAIN[ch]
    if ch in SHIFTED:
        return 'shift-' + SHIFTED[ch]
    if ch.isupper():
        return 'shift-' + ch.lower()
    return ch


class Machine:
    def __init__(self, args, workdir):
        self.workdir = workdir
        self.serial = os.path.join(workdir, 'serial.log')
        sock = os.path.join(workdir, 'monitor.sock')
        qmp_sock = os.path.join(workdir, 'qmp.sock')
        vars_fd = os.path.join(workdir, 'vars.fd')
        shutil.copy(OVMF_VARS, vars_fd)

        image = args.image
        if not args.persist:
            copy = os.path.join(workdir, 'disk.img')
            shutil.copy(image, copy)
            image = copy

        cmd = ['qemu-system-x86_64', '-machine', 'q35', '-m', str(args.memory),
               '-smp', str(args.cpus),
               '-drive', 'if=pflash,format=raw,readonly=on,file=' + OVMF_CODE,
               '-drive', 'if=pflash,format=raw,file=' + vars_fd,
               '-drive', 'format=raw,file=' + image,
               '-serial', 'file:' + self.serial, '-display', 'none',
               '-monitor', 'unix:' + sock + ',server,nowait',
               '-qmp', 'unix:' + qmp_sock + ',server,nowait']
        if args.usb:
            cmd += ['-device', 'qemu-xhci,id=xhci', '-device', 'usb-kbd',
                    '-device', 'usb-tablet' if args.tablet else 'usb-mouse']
        if not args.net:
            cmd += ['-net', 'none']
        cmd += args.qemu_arg
        self.proc = subprocess.Popen(cmd)
        for _ in range(100):
            if os.path.exists(sock):
                break
            time.sleep(0.05)
        self.mon = socket.socket(socket.AF_UNIX)
        self.mon.connect(sock)
        self.mon.settimeout(0.2)
        self.hmp('info version')
        self.qmp_sock = socket.socket(socket.AF_UNIX)
        self.qmp_sock.connect(qmp_sock)
        self.qmp_file = self.qmp_sock.makefile('rw')
        self.qmp_file.readline()
        self.qmp('qmp_capabilities')
        self.size = (1280, 800)

    def qmp(self, command, **arguments):
        msg = {'execute': command}
        if arguments:
            msg['arguments'] = arguments
        self.qmp_file.write(json.dumps(msg) + '\n')
        self.qmp_file.flush()
        while True:
            reply = json.loads(self.qmp_file.readline())
            if 'return' in reply or 'error' in reply:
                return reply

    def drag(self, x1, y1, x2, y2):
        self.absolute(x1, y1)
        self.qmp('input-send-event', events=[{'type': 'btn', 'data': {'down': True, 'button': 'left'}}])
        time.sleep(0.1)
        for i in range(1, 11):
            self.absolute(x1 + (x2 - x1) * i / 10, y1 + (y2 - y1) * i / 10)
        self.qmp('input-send-event', events=[{'type': 'btn', 'data': {'down': False, 'button': 'left'}}])
        time.sleep(0.2)

    def absolute(self, x, y, buttons=None):
        events = [
            {'type': 'abs', 'data': {'axis': 'x', 'value': int(x * 32767 / (self.size[0] - 1))}},
            {'type': 'abs', 'data': {'axis': 'y', 'value': int(y * 32767 / (self.size[1] - 1))}},
        ]
        self.qmp('input-send-event', events=events)
        time.sleep(0.1)
        if buttons:
            for down in (True, False):
                self.qmp('input-send-event', events=[{'type': 'btn', 'data': {'down': down, 'button': buttons}}])
                time.sleep(0.1)

    def hmp(self, command):
        self.mon.sendall((command + '\n').encode())
        time.sleep(0.05)
        out = b''
        try:
            while True:
                chunk = self.mon.recv(65536)
                if not chunk:
                    break
                out += chunk
        except socket.timeout:
            pass
        return out.decode(errors='replace')

    def log(self):
        try:
            with open(self.serial, errors='replace') as f:
                return f.read()
        except FileNotFoundError:
            return ''

    def wait(self, text, timeout, start=0):
        end = time.time() + timeout
        while time.time() < end:
            if text in self.log()[start:]:
                return True
            if self.proc.poll() is not None:
                return False
            time.sleep(0.2)
        return False

    def key(self, code):
        self.hmp('sendkey ' + code)
        time.sleep(0.03)

    def type(self, text):
        for ch in text:
            self.key(qcode(ch))

    def stop(self):
        if self.proc.poll() is None:
            self.proc.kill()
        self.proc.wait()


PROMPT = re.compile(r'^[\w.-]+@[\w.-]+:\S*[$#] ')


def clean(text):
    # The shell redraws its line after every key; keep only the final state
    # of each command line.
    lines = []
    for line in ANSI.sub('', text).split('\n'):
        line = line.rstrip('\r')
        if '\r' in line:
            line = line.split('\r')[-1]
        lines.append(line)
    out = []
    for i, line in enumerate(lines):
        nxt = lines[i + 1] if i + 1 < len(lines) else None
        if (PROMPT.match(line) and nxt is not None and PROMPT.match(nxt)
                and nxt.startswith(line.rstrip()) and len(nxt.rstrip()) >= len(line.rstrip())):
            continue
        out.append(line)
    return '\n'.join(out)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('steps', nargs='*')
    p.add_argument('--image', default=os.path.join(ROOT, 'build', 'aegis.img'))
    p.add_argument('--persist', action='store_true', help='write to the image instead of a copy')
    p.add_argument('--usb', action='store_true', help='add an xHCI controller with a USB keyboard and mouse')
    p.add_argument('--tablet', action='store_true', help='with --usb, use an absolute USB tablet')
    p.add_argument('--net', action='store_true', help='keep QEMU networking')
    p.add_argument('--memory', type=int, default=512)
    p.add_argument('--cpus', type=int, default=2)
    p.add_argument('--timeout', type=float, default=60, help='seconds for @wait/@expect/@login')
    p.add_argument('--step-delay', type=float, default=1.0, help='seconds after each typed line')
    p.add_argument('--qemu-arg', action='append', default=[], help='extra QEMU argument')
    p.add_argument('--full-log', action='store_true', help='print the whole serial log, not only after login')
    args = p.parse_args()

    failed = []
    rejects = [s[8:] for s in args.steps if s.startswith('@reject:')]
    with tempfile.TemporaryDirectory() as tmp:
        m = Machine(args, tmp)
        try:
            for step in args.steps:
                if step.startswith('@login:'):
                    _, user, password = step.split(':', 2)
                    if not m.wait('login:', args.timeout):
                        failed.append('no login prompt')
                        break
                    m.type(user + '\n')
                    m.wait('Password:', 10)
                    m.type(password + '\n')
                    m.wait('$ ', 10)
                elif step.startswith('@type:'):
                    m.type(step[6:])
                elif step.startswith('@key:'):
                    m.key(step[5:])
                    time.sleep(0.3)
                elif step.startswith('@wait:'):
                    m.wait(step[6:], args.timeout)
                elif step.startswith('@expect:'):
                    if not m.wait(step[8:], args.timeout):
                        failed.append(step[8:])
                elif step.startswith('@reject:'):
                    pass
                elif step.startswith('@sleep:'):
                    time.sleep(float(step[7:]))
                elif step.startswith('@shot:'):
                    path = os.path.abspath(step[6:])
                    ppm = os.path.join(tmp, 'shot.ppm')
                    m.hmp('screendump ' + ppm)
                    time.sleep(1)
                    try:
                        from PIL import Image
                        Image.open(ppm).save(path)
                    except ImportError:
                        shutil.copy(ppm, os.path.splitext(path)[0] + '.ppm')
                elif step.startswith('@mouse:'):
                    m.hmp('mouse_move ' + step[7:].replace(',', ' '))
                    time.sleep(0.2)
                elif step.startswith('@button:'):
                    m.hmp('mouse_button ' + step[8:])
                    time.sleep(0.2)
                elif step.startswith('@drag:'):
                    x1, y1, x2, y2 = (float(v) for v in step[6:].split(','))
                    m.drag(x1, y1, x2, y2)
                elif step.startswith('@abs:') or step.startswith('@click:'):
                    parts = step.split(':', 1)[1].split(',')
                    button = None
                    if step.startswith('@click:'):
                        button = parts[2] if len(parts) > 2 else 'left'
                    m.absolute(float(parts[0]), float(parts[1]), button)
                elif step.startswith('@hmp:'):
                    out = m.hmp(step[5:])
                    print('(monitor) ' + step[5:] + ': ' + out.strip().split('\n')[-1].strip(), file=sys.stderr)
                    time.sleep(0.5)
                else:
                    m.type(step + '\n')
                    time.sleep(args.step_delay)
            time.sleep(1)
        finally:
            log = m.log()
            m.stop()

    out = clean(log)
    for r in rejects:
        if r in out:
            failed.append('unexpected ' + repr(r))
    if not args.full_log and 'Welcome to Aegis' in out:
        out = out[out.index('Aegis kernel'):] if 'Aegis kernel' in out else out
    print(out)
    if failed:
        print('\nFAILED: ' + ', '.join(f if f.startswith('unexpected') else repr(f) + ' not seen'
                                     for f in failed), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
