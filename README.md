# nxnative-xone-usb

USB support for the **original Xbox One controller** (2013, micro-USB, no Bluetooth) in the macOS host of [nxnative](https://github.com/seedds/nxnative).

macOS does not treat this controller as a game controller: it shows up as a plain USB device, never gets started, and the pad blinks for a minute and switches itself off. This patch adds a small module to the nxnative Mac host that talks to the pad over USB itself and feeds its buttons and sticks into the game as a normal controller (a player of its own, like any GameController pad).

> **Status: early, works on one setup.** Detection, start-up and input reading work. Two quirks are still open (see [Known issues](#known-issues)). Please test on your own pad and report back.

## Why this exists

Apple added native support for *wired* Xbox controllers in macOS 15, but the first-generation Xbox One controller is not among the models it starts. Steam can drive it with its own driver, but a game that reads controllers through Apple's GameController framework does not see Steam's virtual pad. Rather than buy a newer controller, this project makes the old one work directly.

## How it works

1. macOS leaves a vendor-class USB device (`bDeviceClass` 0xFF) **unconfigured**, so `ioreg` shows the pad with no interfaces under it. The module opens the device and selects configuration 1, which makes macOS create its interfaces.
2. It finds interface 0 of the pad (`045e:02d1`), takes the interrupt IN (`0x81`) and OUT (`0x01`) endpoints, and sends Microsoft's GIP "power on" packet (`05 20 00 01 00`). Until it gets that packet the pad keeps announcing itself (packet type `0x02`) and then gives up.
3. It reads the 18-byte input reports (type `0x20`), applies a 12 % radial dead zone to the sticks, and passes buttons, triggers and sticks to nxnative's `dm_pad_set_player()`.
4. The pad takes a player slot through nxnative's `pad_slots.h`, like any other controller.

No driver, kernel extension, entitlement or reduced system security is needed. The report layout comes from the Linux `xpad` driver.

## Requirements

- macOS 12 or later (uses `IOUSBHost` and `kIOMainPortDefault`). Tested on macOS 27.0, MacBook Pro with Apple M1.
- An nxnative checkout you can build the Mac app from. Developed against commit `6ea9b5b` (28 Oct 2026).
- An original Xbox One controller with USB id `045e:02d1` (check in *Apple menu, hold Option, System Information, USB*), and a micro-USB cable that carries data (not a charge-only cable).
- **Steam and any other controller software must be closed**: they claim the pad exclusively.

## Install

From the root of your nxnative checkout:

```sh
cp /path/to/nxnative-xone-usb/pad_xone_usb.h dm/host/
python3 /path/to/nxnative-xone-usb/apply_xone_patch.py
git --no-pager diff dm/host        # review what changed
```

`apply_xone_patch.py` adds a few lines to `dm/host/front_mac.m` (an include, a start call in `pads_start()`, and release/resume calls on focus changes) and to `dm/host/Makefile` (the new header as a dependency, plus `-framework IOUSBHost -framework IOKit`). It writes `*.orig` backups, refuses to run twice, and stops without writing anything if an expected line is missing.

Then rebuild the Mac app the way you normally do (`--platform mac-app`). If the host is not rebuilt, delete `<title ID>_build/tools/dmhost` and run the build again.

## Use

Quit Steam, plug in the controller, open the game. The controller should light up steadily. In `~/Library/Logs/nxnative/<title ID>.log` you should see:

```
Xbox One USB: watching for the pad (045e:02d1)
Xbox One USB: the pad is configured, looking for its interface
controller 'Xbox One Controller (USB)' connected: player 1
Xbox One USB: the power-on packet was queued
Xbox One USB: packet 1: ...
```

### Settings

Put these in `~/Library/Application Support/nxnative/<title ID>/env.txt`, one per line:

| Variable | Effect |
|---|---|
| `DM_XONE_USB=0` | Turn the module off without rebuilding. |
| `DM_XONE_DEBUG=1` | Log the raw report bytes and how they were read, twice a second. |

## Button mapping

Buttons are mapped **by position**, as nxnative does for other controllers:

| Xbox One | Switch |
|---|---|
| A (bottom) | B |
| B (right) | A |
| X (left) | Y |
| Y (top) | X |
| LB / RB | L / R |
| LT / RT | ZL / ZR (pressed above ~30 % travel) |
| Left / right stick click | L3 / R3 |
| Menu | + |
| View | − |
| D-pad, both sticks | D-pad, both sticks |

## Known issues

- On the tested pad the **left bumper registers only now and then** and the **menu may drift upward**. The pad is second-hand, so worn contacts or stick drift are likely, but this is not yet confirmed. `DM_XONE_DEBUG=1` shows the raw bytes and settles it.
- The dead zone (12 %) and the trigger threshold are fixed in the code.
- No rumble, no Guide button.
- Only controllers with `045e:02d1`. Newer Xbox One controllers use other ids and are handled by macOS itself.
- Tested with one pad, one Mac and one game.

## Troubleshooting

Look for lines starting with `Xbox One USB:` in the log.

| Message | Meaning |
|---|---|
| nothing at all | The build does not contain the module (rebuild, see Install), or `DM_XONE_USB=0` is set. |
| `cannot open the device` / `cannot open the interface` | Something else holds the pad. Quit Steam. |
| `cannot configure the pad` | macOS refused to select the configuration. Include the full message in an issue. |
| `interface 0 of the pad not found among N USB interfaces` | Replug the pad; include N in an issue. |
| `endpoints 0x81 / 0x01 not found` | This pad variant uses other endpoint addresses. Include the message in an issue. |
| `the power-on packet was NOT sent` | See the lines just above it for the reason. |
| packets of type `0x02` only | The pad is still waiting for the power-on packet. |

## Uninstall

```sh
git checkout dm/host/front_mac.m dm/host/Makefile
rm dm/host/pad_xone_usb.h
```

## Scope

This repository contains controller input code only. It contains no game files, no keys and nothing that circumvents protection of any kind. It is not affiliated with the authors of nxnative, Microsoft or Apple. Use it with software and files you are entitled to use.

## Credits

- [nxnative](https://github.com/seedds/nxnative) (MIT) provides the host this plugs into.
- The report layout and the power-on packet come from the Linux kernel's `xpad` driver.
- Written with the help of an AI assistant (Claude by Anthropic), tested on real hardware by the repository owner.

## License

MIT, see [LICENSE](LICENSE).
