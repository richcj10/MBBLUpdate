# ModBusBLUpdater

Replaces the ModBusBL bootloader on an ATmega328P over RS-485, without an ISP programmer.

## Use

1. Build the bootloader: `pio run` in `../ModBusBootlader`.
2. Build this project: `pio run`. The script `gen_bl_image.py` embeds the bootloader's `firmware.hex` as `src/bl_image.h`.
3. Upload `.pio/build/updater/firmware.hex` to the device through the PLC, like a normal app.
4. The device rewrites its bootloader, clears APP_VALID and resets. The new bootloader waits for an app.
5. Upload the real application through the PLC again.

The slave ID stored in EEPROM is kept.

## LED codes

| Pattern | Meaning |
|---|---|
| Fast toggling | Writing bootloader pages |
| 5 blinks, repeated 3× | Unsafe to proceed. **Nothing was written.** The device resets into the old bootloader, which waits for an app. |
| 7 blinks, forever | A page would not verify. The bootloader is half-written and the device needs the ISP programmer. |

## How it works

The ATmega328P can only run SPM from the boot section, so the app borrows an SPM routine from the bootloader:

- **`do_spm()`**: the fixed-address entry at `0x7FE0` (bootloaders from now on). **Never change it or move it.**
- **Gadget**: the `sts SPMCSR,r24 / spm / jmp 0` tail of `jump_to_app()`, found in older bootloaders too. The `jmp 0` lands in this app's reset vector, and `spm_return_hook()` turns it back into a return.

A page is never erased by code running from that same page:

- **Pass 1** writes every page except the one holding the routine in use.
- **Pass 2** writes that last page using a routine from the new image that lives in a different page.

Before anything is written, a dry run checks that pass 2 will be possible.

**Power loss during the update leaves the bootloader half-written.** Keep the device powered until it resets.
