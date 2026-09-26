/*
 * ModBusBLUpdater — replaces the ModBusBL bootloader from the application section.
 *
 * The ATmega328P only executes SPM from the boot section, so this app borrows
 * an SPM routine that already lives in the bootloader:
 *
 *   do_spm()  — fixed-address function at 0x7FE0 (bootloaders from now on).
 *   gadget    — tail of jump_to_app(), "sts SPMCSR,r24 / spm / jmp 0", present
 *               in older bootloaders too. Its jmp 0 lands in this app's reset
 *               vector, where spm_return_hook() returns to the caller.
 *
 * A page must never be erased by code running from that same page. So:
 *   pass 1 — write every page except p0 (the page holding the routine used);
 *   pass 2 — write p0 with a routine from the new image found outside p0.
 * Before writing anything, a dry run on the image confirms pass 2 is possible.
 *
 * Fail-safe rules:
 *   - Anything unexpected BEFORE the first flash write: blink code 5, clear
 *     APP_VALID and reset. The untouched bootloader then waits for a new app.
 *   - A page that will not verify after retries: the bootloader is half-written
 *     and must not run, so stay here blinking code 7 (needs ISP to recover).
 *   - Success: clear APP_VALID and reset into the new bootloader, which waits
 *     for the real application. The slave ID in EEPROM is left alone.
 */

#include <avr/io.h>
#include <avr/boot.h>
#include <avr/eeprom.h>
#include <avr/interrupt.h>
#include <avr/pgmspace.h>
#include <avr/wdt.h>
#include <util/delay.h>
#include <stdint.h>

#include "MBBP.h"
#include "bl_image.h"

#define BL_START        0x7800u
#define BL_END          0x8000u
#define PAGE            SPM_PAGESIZE           /* 128 bytes */
#define PAGE_OF(a)      ((uint16_t)(a) & ~(uint16_t)(PAGE - 1))

#define SPM_ENTRY       0x7FE0u                /* do_spm() in the bootloader */
#define SPM_ENTRY_LEN   20u
#define PAGE_RETRIES    3

#define ERR_UNSAFE      5   /* nothing written, safe to fall back */
#define ERR_BRICKED     7   /* bootloader half-written */

/* ── LED / RS-485 DIR pins (same mapping as the bootloader) ───────────────── */
#ifndef MBBP_LED_PIN
#define MBBP_LED_PIN 13
#endif
#if MBBP_LED_PIN >= 8 && MBBP_LED_PIN <= 13
  #define LED_DDR DDRB
  #define LED_PORT PORTB
  #define LED_BIT (MBBP_LED_PIN - 8)
#elif MBBP_LED_PIN >= 0 && MBBP_LED_PIN <= 7
  #define LED_DDR DDRD
  #define LED_PORT PORTD
  #define LED_BIT MBBP_LED_PIN
#else
  #error "Unsupported MBBP_LED_PIN value"
#endif

#ifndef MBBP_DIR_PIN
#define MBBP_DIR_PIN 8
#endif
#if MBBP_DIR_PIN >= 8 && MBBP_DIR_PIN <= 13
  #define DIR_DDR DDRB
  #define DIR_PORT PORTB
  #define DIR_BIT (MBBP_DIR_PIN - 8)
#elif MBBP_DIR_PIN >= 0 && MBBP_DIR_PIN <= 7
  #define DIR_DDR DDRD
  #define DIR_PORT PORTD
  #define DIR_BIT MBBP_DIR_PIN
#else
  #error "Unsupported MBBP_DIR_PIN value"
#endif

/* ═══════════════════════════════════════════════════════════════════════════
   SPM access
   ═══════════════════════════════════════════════════════════════════════════ */

typedef void (*spm_fn)(uint16_t addr, uint8_t cmd, uint16_t data);

/* Word address of the old bootloader's "sts SPMCSR,r24 / spm / jmp 0". */
static uint16_t gadget_word;

/* Survives the gadget's jmp 0 (no hardware reset happens, RAM is untouched). */
#define SPM_MAGIC 0x5350D0B7UL
volatile uint32_t spm_magic __attribute__((section(".noinit")));

/*
 * Runs first at every entry to 0x0000, before the stack pointer is set up.
 * If we got here from the gadget, return to spm_via_gadget(); otherwise fall
 * through into the normal C startup.
 */
__attribute__((naked, used, section(".init0")))
void spm_return_hook(void) {
    asm volatile(
        /* Bytes of SPM_MAGIC, little-endian — keep in sync. */
        "lds  r24, spm_magic+0  \n\t"
        "cpi  r24, 0xB7         \n\t"
        "brne 1f                \n\t"
        "lds  r24, spm_magic+1  \n\t"
        "cpi  r24, 0xD0         \n\t"
        "brne 1f                \n\t"
        "lds  r24, spm_magic+2  \n\t"
        "cpi  r24, 0x50         \n\t"
        "brne 1f                \n\t"
        "lds  r24, spm_magic+3  \n\t"
        "cpi  r24, 0x53         \n\t"
        "brne 1f                \n\t"
        "clr  r24               \n\t"
        "sts  spm_magic+0, r24  \n\t"
        "sts  spm_magic+1, r24  \n\t"
        "sts  spm_magic+2, r24  \n\t"
        "sts  spm_magic+3, r24  \n\t"
        "ret                    \n\t"
        "1:                     \n\t");
}

/* Mode B: push our return address and the gadget address, then "ret" into it.
   Z is needed for the SPM address, so icall is not an option. */
static void spm_via_gadget(uint16_t addr, uint8_t cmd, uint16_t data) {
    spm_magic = SPM_MAGIC;
    asm volatile(
        "ldi  r26, pm_lo8(.Lgret%=) \n\t"
        "ldi  r27, pm_hi8(.Lgret%=) \n\t"
        "push r26                   \n\t"
        "push r27                   \n\t"
        "push %A[gw]                \n\t"
        "push %B[gw]                \n\t"
        "movw r0,  %[data]          \n\t"
        "movw r30, %[addr]          \n\t"
        "mov  r24, %[cmd]           \n\t"
        "ret                        \n\t"
        ".Lgret%=:                  \n\t"
        "clr  r1                    \n\t"
        :: [gw] "r" (gadget_word), [data] "r" (data),
           [addr] "r" (addr), [cmd] "r" (cmd)
        : "r0", "r24", "r25", "r26", "r27", "r30", "r31", "memory");
    boot_spm_busy_wait();
}

/* Mode A / final page of mode B: the bootloader's fixed-address do_spm(). */
static void spm_via_entry(uint16_t addr, uint8_t cmd, uint16_t data) {
    ((spm_fn)(SPM_ENTRY / 2))(addr, cmd, data);
}

/* ═══════════════════════════════════════════════════════════════════════════
   Helpers
   ═══════════════════════════════════════════════════════════════════════════ */

static inline uint8_t img(uint16_t a) { return pgm_read_byte(&BL_IMAGE[a - BL_START]); }

static uint8_t page_matches(uint16_t page) {
    for (uint16_t i = 0; i < PAGE; i++)
        if (pgm_read_byte(page + i) != img(page + i)) return 0;
    return 1;
}

static uint8_t range_matches(uint16_t start, uint16_t len) {
    for (uint16_t i = 0; i < len; i++)
        if (pgm_read_byte(start + i) != img(start + i)) return 0;
    return 1;
}

static uint8_t write_page(spm_fn spm, uint16_t page) {
    for (uint8_t attempt = 0; attempt < PAGE_RETRIES; attempt++) {
        for (uint16_t i = 0; i < PAGE; i += 2) {
            uint16_t w = img(page + i) | ((uint16_t)img(page + i + 1) << 8);
            spm(page + i, _BV(SPMEN), w);
        }
        spm(page, _BV(PGERS) | _BV(SPMEN), 0);
        spm(page, _BV(PGWRT) | _BV(SPMEN), 0);
        LED_PORT ^= _BV(LED_BIT);
        if (page_matches(page)) return 1;
    }
    return 0;
}

/* Find "sts SPMCSR,r24 / spm / jmp 0" in the installed boot section (in_image=0)
   or in the new image (in_image=1), skipping page `avoid` and any copy that
   straddles two pages. Returns its byte address or 0. */
static uint16_t find_gadget(uint8_t in_image, uint16_t avoid) {
    static const uint8_t sig[10] PROGMEM =
        { 0x80, 0x93, 0x57, 0x00, 0xE8, 0x95, 0x0C, 0x94, 0x00, 0x00 };
    for (uint16_t a = BL_START; a <= BL_END - sizeof(sig); a += 2) {
        if (PAGE_OF(a) == avoid || PAGE_OF(a) != PAGE_OF(a + sizeof(sig) - 1)) continue;
        uint8_t i = 0;
        while (i < sizeof(sig) &&
               (in_image ? img(a + i) : pgm_read_byte(a + i)) == pgm_read_byte(&sig[i])) i++;
        if (i == sizeof(sig)) return a;
    }
    return 0;
}

/*
 * Pick an SPM routine that does not live in page `avoid` — the page being
 * erased must never be the one executing the SPM. Looks at the installed
 * bootloader (in_image=0, and then arms the gadget) or at the new image
 * (in_image=1, a dry run). Returns NULL if there is none; *where = its page.
 */
static spm_fn pick_source(uint8_t in_image, uint16_t avoid, uint16_t *where) {
    uint8_t has_entry = in_image || range_matches(SPM_ENTRY, SPM_ENTRY_LEN);
    if (has_entry && PAGE_OF(SPM_ENTRY) != avoid) {
        *where = PAGE_OF(SPM_ENTRY);
        return spm_via_entry;
    }
    uint16_t g = find_gadget(in_image, avoid);
    if (!g) return 0;
    if (!in_image) gadget_word = g / 2;
    *where = PAGE_OF(g);
    return spm_via_gadget;
}

static void blink(uint8_t n) {
    for (uint8_t i = 0; i < n; i++) {
        LED_PORT |= _BV(LED_BIT);  _delay_ms(150);
        LED_PORT &= ~_BV(LED_BIT); _delay_ms(150);
    }
    _delay_ms(1000);
}

/* Hand control back to the bootloader and have it wait for a real app. */
static void finish(void) {
    eeprom_update_byte((uint8_t *)MBBP_EE_APP_VALID, 0xFF);
    eeprom_busy_wait();
    wdt_enable(WDTO_15MS);
    for (;;) {}
}

static void fail_safe(void) {
    for (uint8_t i = 0; i < 3; i++) blink(ERR_UNSAFE);
    finish();
}

static void fail_bricked(void) {
    for (;;) blink(ERR_BRICKED);
}

/* ═══════════════════════════════════════════════════════════════════════════
   main
   ═══════════════════════════════════════════════════════════════════════════ */

int main(void) {
    cli();
    LED_DDR  |= _BV(LED_BIT);
    DIR_DDR  |= _BV(DIR_BIT);
    DIR_PORT &= ~_BV(DIR_BIT);          /* keep the RS-485 bus released */

    if (range_matches(BL_START, BL_END - BL_START)) finish();   /* already installed */

    /* Pass 1 source: whatever the installed bootloader offers. Its page (p0)
       is the one page pass 1 cannot touch. */
    uint16_t p0, p1;
    spm_fn s0 = pick_source(0, 0, &p0);
    if (!s0) fail_safe();

    /* Dry run: once pass 1 is done, the new image must offer a routine outside
       p0 to rewrite p0 with. Check that now, while nothing is written yet. */
    if (!pick_source(1, p0, &p1)) fail_safe();

    /* From here on, a failure leaves the bootloader half-written. */
    for (uint16_t page = BL_START; page < BL_END; page += PAGE) {
        if (page == p0 || page_matches(page)) continue;
        if (!write_page(s0, page)) fail_bricked();
    }

    /* Pass 2: every page but p0 now holds the new image — use it to write p0. */
    spm_fn s1 = pick_source(0, p0, &p1);
    if (!s1) fail_bricked();
    if (!page_matches(p0) && !write_page(s1, p0)) fail_bricked();

    if (!range_matches(BL_START, BL_END - BL_START)) fail_bricked();

    finish();
    return 0;
}
