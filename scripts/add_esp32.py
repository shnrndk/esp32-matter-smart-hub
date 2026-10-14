#!/usr/bin/env python3
"""Add the ESP32 controller to bulbs already commissioned by this Mac."""
import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
import time
import serial
from pair_bulb import load_config, ROOT, TOOL

ANSI = re.compile(r'\x1b\[[0-9;]*m')


def mac_command(*args):
    result = subprocess.run([str(TOOL), *map(str, args), '--storage-directory', str(ROOT / '.matter-state'),
                             '--paa-trust-store-path', str(ROOT / 'paa-roots')], capture_output=True, text=True, timeout=75)
    output = ANSI.sub('', result.stdout + result.stderr)
    if result.returncode:
        relevant = [line for line in output.splitlines() if 'failure' in line.lower() or 'error' in line.lower() or 'timeout' in line.lower()]
        raise RuntimeError('Mac could not reach/open pairing on this bulb:\n' + '\n'.join(relevant[-8:]))
    return output


def read_serial(port, seconds):
    end = time.monotonic() + seconds
    data = bytearray()
    while time.monotonic() < end:
        data.extend(port.read(4096))
    return ANSI.sub('', data.decode(errors='replace'))


def commission(port, node, code):
    port.reset_input_buffer()
    port.write(('matter esp controller pairing code {} {}\r'.format(node, code)).encode())
    deadline = time.monotonic() + 120
    output = ''
    while time.monotonic() < deadline:
        output += read_serial(port, 0.5)
        if re.search(r'Commissioning success with node [0-9A-Fa-f]+-' + format(node, 'X') + r'(?:\r|\n|$)', output):
            read_serial(port, 1)  # allow final NVS writes/commissioning cleanup
            return
        if 'Commissioning failure with node' in output or 'PASE session establishment failure' in output:
            output += read_serial(port, 1)
            break
    relevant = [line for line in output.splitlines() if any(word in line.lower() for word in ['failure', 'error', 'attestation', 'timeout', 'abort', 'panic'])]
    raise RuntimeError('ESP32 commissioning did not complete for bulb {}:\n{}'.format(node, '\n'.join(relevant[-12:])))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', type=Path, default=ROOT / 'config.local.json')
    parser.add_argument('--port', default='/dev/cu.usbserial-0001')
    parser.add_argument('--bulb', type=int, help='Add just this configured node ID.')
    args = parser.parse_args()
    wifi, bulbs = load_config(args.config)
    if args.bulb is not None:
        bulbs = [b for b in bulbs if b['node_id'] == args.bulb]
    if not bulbs:
        raise ValueError('No matching bulbs in config.')
    # Verify Mac connectivity before resetting/opening the ESP32 serial port.
    for bulb in bulbs:
        mac_command('descriptor', 'read', 'parts-list', bulb['node_id'], 0)
        print('Mac verified bulb {}.'.format(bulb['node_id']), flush=True)
    port = serial.Serial(port=None, baudrate=115200, timeout=0.1)
    port.dtr = False; port.rts = False; port.port = args.port
    with port:
        port.rts = True; time.sleep(0.1); port.rts = False
        boot = read_serial(port, 12)
        if 'IP_EVENT_STA_GOT_IP' not in boot:
            if not wifi.get('password'):
                raise ValueError('Set wifi.password in config.local.json or connect the ESP32 to Wi-Fi first.')
            command = 'matter esp wifi connect {} {}\r'.format(json.dumps(wifi['ssid']), json.dumps(wifi['password']))
            port.write(command.encode())
            boot += read_serial(port, 15)
        if 'IP_EVENT_STA_GOT_IP' not in boot:
            raise RuntimeError('ESP32 did not connect to Wi-Fi. Check its network before adding bulbs.')
        print('ESP32 Wi-Fi verified. Opening a separate Matter fabric for each bulb.', flush=True)
        for bulb in bulbs:
            node = bulb['node_id']
            output = mac_command('pairing', 'open-commissioning-window', node, 1, 300, 1000, 2365)
            match = re.search(r'Manual pairing code:\s*\[?([0-9]{11}|[0-9]{21})\]?', output)
            if not match:
                raise RuntimeError('Mac did not return a new commissioning code for bulb {}.'.format(node))
            print('Adding bulb {} to the ESP32…'.format(node), flush=True)
            commission(port, node, match.group(1))
            print('ESP32 commissioning succeeded for bulb {}.'.format(node), flush=True)
    print('Both controllers keep their own access. Reload the SmartHub page and use Check bulbs.')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as exc:
        sys.exit(str(exc))
