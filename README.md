# ModBusBLUpdater

> **Archived.** This project has moved into [ModBusBootlader](https://github.com/richcj10/ModBusBootlader) under `updater/`, so the updater is always built from the matching bootloader and device config. This repo is kept read-only for history.

Replaces the ModBusBL bootloader on an ATmega328P over RS-485, without an ISP programmer.

## Use

1. Build the bootloader: `pio run` in `../ModBusBootlader`.
2. Build this project: `pio run`. The script `gen_bl_image.py` embeds the bootloader's `firmware.hex` as `src/bl_image.h`. It prints the version it embeds, for example `bootloader v2`, and refuses to build if the bootloader has no version record.
3. On the ESP32PLC, open **FW Update**, click **Update BL** on the device, and pick `.pio/build/updater/firmware.hex` plus the device's app `.hex`. The PLC flashes the updater, finds the new bootloader, flashes the app back, restores the address if needed, and reports the result.

Without the PLC's guided flow, do it by hand: upload the updater like a normal app, wait for the device to reset into the new bootloader, then upload the real application again.

The slave ID stored in EEPROM is kept.

## Version check

The updater reads the installed bootloader's version from its info record at `0x7FF8` (`'M' 'B' 'B' 'L' <version> <~version>`). A bootloader without the record counts as version 1. It compares that with the version of the image it carries (`BL_IMAGE_VERSION`, taken from the same record in the embedded image at build time):

| Installed vs. image | What happens |
|---|---|
| Byte-for-byte identical | Nothing written. 1 long blink. Reports "already vN" on the PLC |
| Image version newer | Bootloader rewritten and verified. 2 long blinks. The PLC reports "vOld -> vNew" |
| Image version the same or older | Nothing written. Blink code 4. The PLC reports "already vN" for the same version (it can only compare version numbers) and "Refused" for an older one |

Same-or-older is refused so a rebuilt bootloader whose `MBBP_BL_VERSION` wasn't bumped can't slip out, and so nothing gets downgraded by mistake. Build with `-DALLOW_BL_DOWNGRADE` to allow it on purpose.

After writing, the updater checks every page, then the whole boot section, then that the version record reads back as the image's version. If any of those checks fails it stops with blink code 7.

The PLC gets the same answer independently: the bootloader reports its version in the HELLO reply before and after the update.

## LED codes

| Pattern | Meaning |
|---|---|
| Fast toggling | Writing bootloader pages |
| 2 long blinks | Success: new bootloader written and verified |
| 1 long blink | Already installed (identical). Nothing written |
| 4 blinks, repeated 3× | Image version not newer than the installed one. **Nothing was written.** |
| 5 blinks, repeated 3× | Unsafe to proceed. **Nothing was written.** |
| 7 blinks, forever | A page or the version record would not verify. The bootloader is half-written and the device needs the ISP programmer. |

After every pattern except 7, the device resets into its bootloader (new or unchanged), which waits for the real app.

## How it works

The ATmega328P can only run SPM from the boot section, so the app borrows an SPM routine from the bootloader:

- **`do_spm()`**: the fixed-address entry at `0x7FE0` (bootloaders from now on). **Never change it or move it.**
- **Gadget**: the `sts SPMCSR,r24 / spm / jmp 0` tail of `jump_to_app()`, found in older bootloaders too. The `jmp 0` lands in this app's reset vector, and `spm_return_hook()` turns it back into a return.

A page is never erased by code running from that same page:

- **Pass 1** writes every page except the one holding the routine in use.
- **Pass 2** writes that last page using a routine from the new image that lives in a different page.

Before anything is written, a dry run checks that pass 2 will be possible.

**Power loss during the update leaves the bootloader half-written.** Keep the device powered until it resets.
