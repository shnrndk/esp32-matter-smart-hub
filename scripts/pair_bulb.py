#!/usr/bin/env python3
"""Commission bulbs using a private JSON config and persistent Mac fabric."""
import argparse
from getpass import getpass
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
TOOL = ROOT / '.toolchains/esp-matter/connectedhomeip/connectedhomeip/out/host/chip-tool'


def normalize_code(value):
    if not isinstance(value, str):
        raise ValueError('Matter codes must be JSON strings, in double quotes.')
    code = value.strip()
    if code.startswith('MT:'):
        return code
    code = re.sub(r'[\s\-\u2010-\u2015\u2212]', '', code)
    if not re.fullmatch(r'[0-9]{11}|[0-9]{21}', code):
        raise ValueError('Expected 11 or 21 digits (hyphens/spaces are allowed), or an MT: QR payload.')
    return code


def load_config(path):
    data = json.loads(path.read_text())
    wifi = data.get('wifi', {})
    if not isinstance(wifi, dict) or not isinstance(wifi.get('ssid'), str) or not wifi['ssid']:
        raise ValueError('Set wifi.ssid to your 2.4 GHz Wi-Fi name.')
    if not isinstance(wifi.get('password', ''), str):
        raise ValueError('wifi.password must be a JSON string.')
    bulbs = data.get('bulbs')
    if not isinstance(bulbs, list) or not bulbs:
        raise ValueError('Add at least one entry to bulbs.')
    ids = set()
    for bulb in bulbs:
        if not isinstance(bulb, dict):
            raise ValueError('Each bulb must be a JSON object.')
        node = bulb.get('node_id')
        if type(node) is not int or not 0 < node < 0xFFFFFFEFFFFFFFFF or node in ids:
            raise ValueError('Each bulb needs a unique positive Matter node_id integer.')
        ids.add(node)
        bulb['matter_code'] = normalize_code(bulb.get('matter_code'))
    return wifi, bulbs


def run_pairing(command, password):
    """Keep console output live and identify the actual failure before teardown."""
    recent = []
    with subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          text=True, bufsize=1) as process:
        for line in process.stdout:
            if password:
                line = line.replace(password, '[Wi-Fi password redacted]')
            print(line, end='', flush=True)
            recent.append(line)
            if len(recent) > 200:
                recent.pop(0)
        status = process.wait()
    if status and 'Discovery timed out' in ''.join(recent):
        print('\nThe controller could not discover this bulb; no Wi-Fi credentials were delivered.')
        print('For first-time setup: power the bulb off once, then on near the Mac, and retry promptly.')
        print("Confirm Apple's Matter Bluetooth developer profile is installed and restart the Mac after installing it.")
        print('For an already commissioned bulb: open a new pairing window on its existing controller and use the NEW code.')
    return status


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', type=Path, default=ROOT / 'config.local.json')
    parser.add_argument('--bulb', type=int, help='Pair only this node_id; otherwise pair all entries in order.')
    parser.add_argument('--validate-only', action='store_true', help='Check config and Matter codes without pairing or asking for a password.')
    args = parser.parse_args()
    try:
        wifi, bulbs = load_config(args.config)
    except (OSError, ValueError, TypeError, AttributeError) as exc:
        sys.exit('Config error: ' + str(exc))
    if args.bulb is not None:
        bulbs = [b for b in bulbs if b['node_id'] == args.bulb]
        if not bulbs:
            sys.exit('No bulb with that node_id exists in the config.')
    if not TOOL.is_file():
        sys.exit('CHIP Tool has not been built yet.')
    # Check the manual-code checksum/QR payload using Matter's own parser.
    # Capture its output because it includes decoded setup secrets.
    for bulb in bulbs:
        result = subprocess.run([str(TOOL), 'payload', 'parse-setup-payload', bulb['matter_code']], capture_output=True, text=True)
        if result.returncode:
            sys.exit('Bulb {}: Matter rejected the code. Check the printed code in the config.'.format(bulb['node_id']))
        print('Bulb {}: Matter code validated.'.format(bulb['node_id']))
    if args.validate_only:
        print('Config validated. No pairing attempted.')
        return
    if not any((ROOT / 'paa-roots').glob('*.der')):
        sys.exit('Download the CSA attestation roots before pairing.')
    print("Enable Bluetooth and install Apple's Matter developer profile before pairing.")
    password = wifi.get('password', '') or getpass('Wi-Fi password (config field is empty): ')
    state = ROOT / '.matter-state'
    state.mkdir(mode=0o700, exist_ok=True)
    state.chmod(0o700)
    for bulb in bulbs:
        node = str(bulb['node_id'])
        print('\nPairing bulb {} ({}). Keep it powered on and in commissioning mode.'.format(node, bulb.get('name', 'unnamed')))
        status = run_pairing([
            str(TOOL), 'pairing', 'code-wifi', node, wifi['ssid'], password, bulb['matter_code'],
            '--storage-directory', str(state), '--paa-trust-store-path', str(ROOT / 'paa-roots'),
        ], password)
        if status:
            print('Stopped at bulb {}. After resolving the error, retry only this bulb:'.format(node), file=sys.stderr)
            print('.venv/bin/python scripts/pair_bulb.py --bulb ' + node, file=sys.stderr)
            return status
        print('Mac commissioning command completed for bulb ' + node + '. Verify its actual state:')
        print('bash scripts/chip-tool.sh descriptor read parts-list ' + node + ' 0')
        print('For the ESP32, open a NEW commissioning window for this bulb:')
        print('bash scripts/chip-tool.sh pairing open-commissioning-window ' + node + ' 1 300 1000 2365')
        print('ESP32 console: matter esp controller pairing code ' + node + ' NEW_CODE')
    print('\nThese bulbs are now commissioned to the Mac. The ESP32 needs its own access for phone control.')
    print('Complete that step automatically: .venv/bin/python scripts/add_esp32.py')
    print('Keep .matter-state intact. Follow README.md for endpoint discovery and control.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
