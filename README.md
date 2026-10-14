# Sengled Matter controller

## Current status

Detected on 2026-10-13 at `/dev/cu.usbserial-0001`: ESP32-D0WD-V3 revision 3.1,
dual-core 240 MHz, 4 MB flash. Original firmware was backed up before installing
the controller.

`firmware/` is based on Espressif's public-domain controller example from
ESP-Matter `release/v1.5`. It is prepared for on-network commissioning with a
single application slot inside 4 MB flash and DCL attestation roots. It has been
compiled, flashed, and verified to boot its Matter commissioner and USB console.
The binary is 1,890,672 bytes. Startup diagnostics reported 111,096 bytes of free
internal heap, a 90,112-byte largest block, and no PSRAM. SDK dependencies and the
ESP-IDF v5.5.5 compiler are installed locally in `.toolchains/`.

Both bulbs have now completed commissioning on the Mac and on the ESP32.
Both report light endpoint 1 and support power, brightness, and color control.
The Mac's CHIP Tool has also built successfully and passed a published example
setup-code parsing check. Its generated diagnostic logger uses `-O0` to reduce
build time; the other host components keep their usual optimization settings.

## Chosen setup: Mac first, ESP32 for ongoing control

The Sengled W41-N15A uses Matter over Wi-Fi. No Thread border router is needed.
An unpaired bulb needs a commissioner to deliver Wi-Fi credentials over BLE.
Espressif documents BLE-Wi-Fi commissioning for ESP32-S3. The connected classic
ESP32 profile here deliberately uses on-network commissioning; it cannot perform
the bulb's first-time Wi-Fi setup with this configuration.

The selected route is the Mac for initial commissioning. Hardware alternative:

1. Use the Mac's CHIP Tool for initial BLE-Wi-Fi commissioning, then open a new
   commissioning window and add the ESP32 to its own fabric over the network.
   macOS BLE commissioning needs Apple's Bluetooth Central Matter Client Developer
   Mode Profile. This route still needs a firmware build and memory validation.
2. Use ESP32-S3 with PSRAM for a standalone commissioner/controller. Verify its
   exact flash capacity and PSRAM mode before changing the configuration. The
   official S3 profile and RAM optimization settings are in the SDK controller example.

## On-network controller commands (after successful firmware build)

Use the device console at 115200 baud. Enter credentials locally; do not commit
them to source files. The bulb and controller need the same LAN with IPv6 and
multicast available, and the bulb needs an open commissioning window.

```text
matter esp wifi connect "YOUR_2_4_GHZ_SSID" "YOUR_PASSWORD"
matter esp controller pairing code 1 NEW_MATTER_PAIRING_CODE
matter esp controller read-attr 1 0 0x1d 3
```

The last command reads the root Descriptor PartsList. For each returned endpoint,
read its ServerList (`matter esp controller read-attr 1 ENDPOINT 0x1d 1`) to find
OnOff (0x6), LevelControl (0x8), and ColorControl (0x300). Do not assume endpoint 1.
Substitute the confirmed light endpoint below:

```text
matter esp controller invoke-cmd 1 ENDPOINT 6 1
matter esp controller invoke-cmd 1 ENDPOINT 6 0
matter esp controller invoke-cmd 1 ENDPOINT 6 2
matter esp controller read-attr 1 ENDPOINT 6 0
```

The commands turn on, turn off, toggle, and read the actual OnOff state.
Check command responses and readbacks before treating a command as successful.
Keep the controller's NVS intact after pairing: it holds the fabric credentials.

## Sources

- [Espressif controller guide](https://docs.espressif.com/projects/esp-matter/en/latest/esp32/controller.html)
- [Upstream controller example](https://github.com/espressif/esp-matter/tree/release/v1.5/examples/controller)
- [SDK setup](https://docs.espressif.com/projects/esp-matter/en/latest/esp32/developing.html)

The SDK checkout uses ESP-IDF v5.5.5. `scripts/env.sh` enables the project-local
toolchain; run it with `source` from bash when using `idf.py` directly.

## Private configuration for both bulbs

Edit `config.local.json`. It contains the shared `wifi.ssid`, `wifi.password`,
and a `bulbs` list with separate `node_id` values 1 and 2. Both supplied Matter
codes are already entered as strings. Set `wifi.password` locally; if you leave
it empty, the script prompts once and does not save the answer. The file is
excluded from Git and has owner-only read/write permissions.

Check configuration and code checksums without commissioning:

```sh
.venv/bin/python scripts/pair_bulb.py --validate-only
```

Pair both bulbs with the command below, or select one with
`.venv/bin/python scripts/pair_bulb.py --bulb 2`. After a failure, use `--bulb`
to retry the affected bulb rather than pairing an already commissioned bulb again.

## Pair locally

1. Install [Apple's Bluetooth Central Matter Client Developer Mode profile](https://developer.apple.com/services-account/download?path=/iOS/iOS_Logs/EnableBluetoothCentralMatterClientDeveloperMode.mobileconfig)
   in System Settings (Profiles/Device Management), and restart the Mac. Enable
   Bluetooth and allow CHIP Tool access when macOS requests it. The upstream
   [macOS guide](https://github.com/project-chip/connectedhomeip/blob/master/docs/guides/darwin.md)
   documents this requirement. Do not restart during a build or flash.
2. In a terminal, run:

   ```sh
   cd /Users/sahanr/Desktop/smarthub
   .venv/bin/python scripts/pair_bulb.py
   ```

   The script reads the network and both codes from `config.local.json` and pairs
   them sequentially. It prompts for the password only if the config field is empty.
   Keep both bulbs in commissioning mode and wait for commissioning success.
   The Mac's fabric credentials persist in the ignored `.matter-state/` directory.
3. Discover the light endpoint and verify its OnOff state using the Mac:

   ```sh
   bash scripts/chip-tool.sh descriptor read parts-list 1 0
   bash scripts/chip-tool.sh descriptor read server-list 1 ENDPOINT
   bash scripts/chip-tool.sh onoff read on-off 1 ENDPOINT
   ```

   Replace `ENDPOINT` with the endpoint whose ServerList includes cluster 6.
   These examples use node ID 1; repeat with node ID 2 for the second bulb.
4. Open the ESP32 console in another terminal:

   ```sh
   bash scripts/console.sh
   ```

   Enter `matter esp wifi connect "YOUR_SSID" "YOUR_PASSWORD"` and wait for the
   board to obtain an IP address. Then open a new bulb commissioning window on the Mac:

   ```sh
   bash scripts/chip-tool.sh pairing open-commissioning-window 1 1 300 1000 2365
   ```

   Copy the **new** manual pairing code from this command into the ESP32 console:
   `matter esp controller pairing code 1 NEW_CODE`. The window lasts five minutes.
   If it expires, reopen it. Do not reuse the printed bulb code for this second fabric.
   Repeat for bulb 2 using node ID 2 in both the Mac and ESP32 commands. Each bulb
   needs its own newly generated commissioning code.
5. Discover endpoints on the ESP32 as shown above, then turn the bulb on/off and
   read back the OnOff attribute. Once this succeeds the Mac can be turned off;
   keep the ESP32 and bulb powered and on the same LAN.

Exit the USB console with Ctrl+]. Only one program can use the serial port at a time.
Network isolation, disabled IPv6, or blocked multicast can prevent discovery.

## Rebuild and recovery

```sh
bash scripts/build.sh
```

To rebuild the Mac commissioning tool, run `bash scripts/build_mac_tools.sh`.

The original board flash is saved in `backups/original-esp32-2026-10-13.bin`
with a SHA-256 sidecar. It contains device data and is excluded from Git.
To restore it, close the USB console, then run:

```sh
.venv/bin/python -m esptool --port /dev/cu.usbserial-0001 write_flash 0 backups/original-esp32-2026-10-13.bin
```

## Add Mac-commissioned bulbs to the ESP32 automatically

Successful Mac commissioning does not automatically give the ESP32 access.
Run the following once for bulbs that have completed Mac setup:

```sh
.venv/bin/python scripts/add_esp32.py
```

This verifies the Mac's access, opens a new commissioning window on each bulb,
and sends its temporary code to the ESP32 through USB. Codes stay in memory and
are not written to the config. Close the USB console before running this helper.
Use `--bulb 1` or `--bulb 2` to add only a specific missing bulb. Once added,
the ESP32 operates independently; the Mac can be shut down.

## Phone page hosted by the ESP32

Open `http://192.168.1.208/` from a phone on the same Wi-Fi. The router may assign
another address later; the USB console prints `Phone controls: http://.../` after
connecting. You can reserve the address in your router to keep it stable.

The embedded page has separate controls for bulb IDs 1 and 2: power, 1–100%
brightness, Nightlight/Unwind/Everyday/Bright brightness presets, six white tones
from Candlelight to Daylight, eight color swatches, a custom color picker, and
color intensity. White temperatures are clamped to the range reported by the bulb.
The interface sends a real Matter command, then reads back the bulb attributes.
It only marks the command confirmed after the read succeeds. Endpoints and
supported clusters are discovered, rather than assuming endpoint 1.

Use **Check bulbs** to refresh state. Until a bulb responds, controls remain
unavailable. Cached state is the last read, not a continuous live subscription.
The web API serializes requests to bound memory on the classic ESP32. No external
fonts, scripts or servers are needed for the page. The web service is intended
for this home LAN; anyone who can access the page on that LAN can operate it.

This firmware update preserves the NVS Wi-Fi/fabric storage. A pre-update copy
is saved in `backups/pre-web-controller-nvs-2026-10-13.bin`.

### Discovery timeout during initial Mac pairing

`Discovery timed out` means the commissioner did not find a matching bulb.
The final `Incorrect state` error occurs afterward and does not show a Wi-Fi
password problem. First-time setup needs the bulb in commissioning mode near
the Mac, and Apple's Matter Bluetooth developer profile installed followed by
an OS restart. Sengled's setup guide describes a 15-minute first-time window
after power-on. Power off/on once to reopen it, then retry only the affected bulb:

```sh
.venv/bin/python scripts/pair_bulb.py --bulb 1
```

If the bulb already has an administrator, use that administrator to open a new
commissioning window and use its newly generated code. Do not factory-reset a
bulb merely because another commissioning attempt times out.

### Verification of this web build (2026-10-13)

The firmware compiled and flashed in place successfully. The ESP32 served both
`/` and `/api/status` over Wi-Fi at 192.168.1.208. A 390-pixel browser viewport had
no horizontal overflow and showed all six white-tone presets, eight color presets,
brightness and custom color controls. The API rejected headerless control requests,
unknown bulb IDs and out-of-range brightness values. Failed bulb discovery released
the pending operation, and the page correctly kept unavailable controls disabled.

Both configured bulb IDs timed out in authenticated operational discovery. Real
power, brightness and color effects therefore remain unverified until commissioning
succeeds. The Mac's configuration-profile query did not report the required Matter
Bluetooth profile. The pairing helper now explains discovery timeout before the
secondary teardown error and redacts the Wi-Fi password from console output.

### Connection and UI fix (2026-10-13)

The Mac log showed both bulbs commissioned successfully, but the ESP32 had not
been added to their fabrics. The new `scripts/add_esp32.py` completed that step
for both bulbs. The web API now reads their actual attributes successfully.
Disabled controls use an unavailable cursor; a progress cursor is shown only
while a request is running. HTTP requests have a six-second connection timeout.

The controller waits for requested attributes to settle before marking a change
confirmed. This handles bulbs that fade even with a zero transition duration.
The confirmation wait is bounded to 15 seconds after command acceptance. The
reported temperature range is limited to the page's 1800–6500 K controls.

### Final live verification

Both bulb IDs 1 and 2 are commissioned to the ESP32 and respond on endpoint 1.
Power, brightness, warm-white temperature and hue/saturation color were tested
through the real web API and read back from the bulbs. Tests confirmed 80%
brightness, about 2700 K white, red at full saturation, and bulb 2 power toggling.
The settings present at the start of testing were restored, including inactive
hue/saturation values. The hub remained responsive with about 100 KB free heap.
The browser shows both bulbs online, all controls enabled, and a normal pointer
on the power button when idle. Mac commissioning is complete; do not rerun initial
pairing for these bulbs. Keep the ESP32 powered and on Wi-Fi for normal control.

## Both bulbs mode and standalone power

Select **Both bulbs** next to Bulb 1 and Bulb 2. Power, brightness, white-tone
presets, color presets, custom color and intensity apply to both lights. Commands
run one bulb at a time to keep ESP32 memory use bounded. Mixed settings are shown
as **Mixed**. If one bulb is on and the other is off, the group power button turns
both on; when both are on, it turns both off. If either action fails, the page
reports partial success instead of claiming both bulbs changed.

When their hues differ, pick a shared color before changing group color intensity.
The individual bulb tabs continue to work independently.

You can unplug the ESP32's USB cable from the laptop and plug it into a standard
5 V USB wall power supply through the board's USB port. The Wi-Fi and Matter
credentials stay in NVS and reconnect after boot. Keep the ESP32 and both bulbs
on the same Wi-Fi network. The Mac is only needed for future setup or firmware
updates, not everyday control. The router can assign a different IP later; a DHCP
reservation for the ESP32 keeps the page address stable.

The Both bulbs update was compiled, flashed without erasing NVS, and tested after
reboot. One browser brightness action set both bulbs to 70%; one Ruby color action
set both bulbs to the matching hue/saturation. Both starting configurations were
then restored individually. The 390-pixel layout contains all three mode buttons
without horizontal overflow.
