#!/usr/bin/env python3
"""Client for the second player of CDDA multiplayer.

Connects to the host's game (TCP, one JSON object per line, see
docs/mp/protocol.md) and sends commands for the remote-controlled NPC.

Interactive mode (default) uses curses, or on Windows without the
windows-curses package a plain console:
    h j k l y u b n, arrow keys or numpad 1-9 - move / bump attack
    a + direction - attack
    . or 5 - wait        g - pick up everything here
    s - status           q - quit

Script mode sends commands given as arguments or on stdin, one per line,
in the short form "move n", "attack e", "wait", "pickup", "status", and
prints every message from the server:
    client.py --script "move n" "wait" status
"""

import argparse
import json
import select
import socket
import sys
import time

DIR_KEYS = {
    'k': 'n', 'u': 'ne', 'l': 'e', 'n': 'se',
    'j': 's', 'b': 'sw', 'h': 'w', 'y': 'nw',
    '8': 'n', '9': 'ne', '6': 'e', '3': 'se',
    '2': 's', '1': 'sw', '4': 'w', '7': 'nw',
}


class Connection:
    def __init__(self, host, port):
        self.sock = socket.create_connection((host, port))
        self.buffer = b''

    def send(self, message):
        self.sock.sendall(json.dumps(message).encode() + b'\n')

    def receive(self, timeout):
        """Returns the messages that arrived within timeout seconds."""
        ready, _, _ = select.select([self.sock], [], [], timeout)
        if ready:
            data = self.sock.recv(65536)
            if not data:
                raise ConnectionError('the host closed the connection')
            self.buffer += data
        messages = []
        while b'\n' in self.buffer:
            line, self.buffer = self.buffer.split(b'\n', 1)
            if line.strip():
                messages.append(json.loads(line))
        return messages


def parse_short(text):
    """'move n' -> {'cmd': 'move', 'dir': 'n'}"""
    parts = text.split()
    if not parts:
        return None
    message = {'cmd': parts[0]}
    if len(parts) > 1:
        message['dir'] = parts[1]
    return message


def describe(message):
    kind = message.get('type')
    if kind in ('status', 'your_turn', 'state'):
        s = message['status']
        text = ('{name}: HP {hp}/{hp_max}  stamina {stamina}/{stamina_max}  '
                'hunger {hunger}  thirst {thirst}  sleepiness {sleepiness}  '
                'pain {pain}  pos {pos}').format(**s)
        return ('YOUR TURN  ' if kind == 'your_turn' else '') + text
    if kind == 'welcome':
        return 'connected, protocol v{}, character: {}'.format(
            message.get('version'), message.get('npc', '(none yet)'))
    if kind == 'log':
        return '\n'.join(message.get('lines', []))
    if kind == 'ok':
        return 'ok: ' + message.get('cmd', '')
    if kind == 'rejected':
        return "can't do that: " + message.get('reason', '')
    if kind == 'error':
        return 'error: ' + message.get('message', '')
    return json.dumps(message)


def run_script(conn, commands, wait):
    def drain(timeout):
        end = time.time() + timeout
        while True:
            left = end - time.time()
            if left <= 0:
                return
            for message in conn.receive(left):
                if message.get('type') == 'view':
                    # The map is for the in-game client; just show its size.
                    message = {'type': 'view', 'radius': message.get('radius')}
                print(json.dumps(message), flush=True)

    drain(wait)
    for text in commands:
        message = parse_short(text)
        if message is None:
            continue
        conn.send(message)
        drain(wait)


def handle_key(conn, key, direction, state, show):
    """Shared by both interactive front ends. Returns False to quit."""
    if state.get('attack'):
        state['attack'] = False
        if direction:
            conn.send({'cmd': 'attack', 'dir': direction})
        else:
            show('attack cancelled')
    elif direction:
        conn.send({'cmd': 'move', 'dir': direction})
    elif key == 'a':
        state['attack'] = True
        show('attack where?')
    elif key in ('.', '5'):
        conn.send({'cmd': 'wait'})
    elif key == 'g':
        conn.send({'cmd': 'pickup'})
    elif key == 's':
        conn.send({'cmd': 'status'})
    elif key == 'q':
        return False
    return True


HELP = ('CDDA multiplayer - hjklyubn/arrows/numpad move, a+dir attack, '
        '. wait, g pick up, s status, q quit')


def run_console_windows(conn):
    """Fallback for Windows, where Python ships without curses."""
    import msvcrt

    arrows = {'H': 'n', 'P': 's', 'K': 'w', 'M': 'e',
              'G': 'nw', 'I': 'ne', 'O': 'sw', 'Q': 'se'}
    state = {}
    print(HELP, flush=True)
    while True:
        for message in conn.receive(0.05):
            if message.get('type') != 'view':
                print(describe(message), flush=True)
        if not msvcrt.kbhit():
            continue
        key = msvcrt.getwch()
        direction = None
        if key in ('\x00', '\xe0'):
            direction = arrows.get(msvcrt.getwch())
            key = ''
        else:
            direction = DIR_KEYS.get(key)
        if not handle_key(conn, key, direction, state, lambda t: print(t, flush=True)):
            return


def run_curses(conn):
    try:
        import curses
    except ImportError:
        if sys.platform == 'win32':
            run_console_windows(conn)
            return
        raise

    def main(screen):
        curses.curs_set(0)
        screen.timeout(100)
        log = []
        state = {}

        def show(text):
            log.append(text)
            del log[:-200]

        while True:
            for message in conn.receive(0):
                if message.get('type') != 'view':
                    show(describe(message))
            screen.erase()
            height, width = screen.getmaxyx()
            screen.addnstr(0, 0, HELP, width - 1)
            for row, text in enumerate(log[-(height - 2):]):
                screen.addnstr(row + 2, 0, text, width - 1)
            screen.refresh()

            key = screen.getch()
            if key == -1:
                continue
            arrows = {curses.KEY_UP: 'n', curses.KEY_DOWN: 's',
                      curses.KEY_LEFT: 'w', curses.KEY_RIGHT: 'e'}
            ch = chr(key) if 0 <= key < 256 else ''
            direction = arrows.get(key) or DIR_KEYS.get(ch)
            if not handle_key(conn, ch, direction, state, show):
                return

    curses.wrapper(main)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=7777)
    parser.add_argument('--script', nargs='*', metavar='COMMAND',
                        help='send commands instead of the interactive mode '
                             '(read from stdin if none are given)')
    parser.add_argument('--wait', type=float, default=1.0,
                        help='script mode: seconds to collect answers after each command')
    args = parser.parse_args()

    conn = Connection(args.host, args.port)
    if args.script is not None:
        commands = args.script or [line.strip() for line in sys.stdin]
        run_script(conn, commands, args.wait)
    else:
        run_curses(conn)


if __name__ == '__main__':
    main()
