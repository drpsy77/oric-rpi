/* SPDX-License-Identifier: EUPL-1.2 */
/*
 * oric_machine.c — Colle machine Oric pour le port Raspberry Pi / Circle.
 *
 * PROVENANCE : ce fichier est un EXTRAIT de Phosphoric src/main.c
 *   https://github.com/benedictemarty/Phosphoric (commit indiqué dans SOURCES.md)
 *   Copyright (c) 2026 bmarty <bmarty@mailo.com> — Licence EUPL-1.2
 *
 * Règle appliquée : le code est COPIÉ tel quel ; les seules modifications sont
 *   - des SUPPRESSIONS de branches hors périmètre (debugger, traces,
 *     co-simulation LOCI, SDL, réseau, captures, auto-frappe), signalées
 *     par « [port] » ;
 *   - les fonctions oric_machine_init/run_frame/reset qui remplacent
 *     main()/main_setup_*()/emulator_run() en n'en gardant que les appels utiles.
 * Chaque bloc indique la fonction d'origine de main.c.
 */

#include "oric_machine.h"
#include "emulator.h"
#include "card_module.h"
#include "cli/cli_opts.h"
#include "io/io_bus.h"
#include "io/tape_patches.h"
#include "io/loci_glue.h"
#include "io/loci_emu.h"
#include "savestate.h"
#include "rom_patches.h"
#include "utils/logging.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── main.c : io_read_callback ─────────────────────────────────────────── */
/* I/O callback: route VIA and Microdisc register access */
static uint8_t io_read_callback(uint16_t address, void* userdata) {
    emulator_t* emu = (emulator_t*)userdata;

    /* I/O bus: registered devices. Falls back to the VIA. */
    const io_device_t* dev = io_bus_find(emu, address);
    if (dev) return dev->read(emu, address);

    /* VIA 6522: $0300-$030F (mirrored in $0300-$03FF) */
    return via_read(&emu->via, (uint8_t)(address & 0x0F));
}

/* ── main.c : io_peek_callback ─────────────────────────────────────────── */
static uint8_t io_peek_callback(uint16_t address, void* userdata) {
    emulator_t* emu = (emulator_t*)userdata;
    const io_device_t* dev = io_bus_find(emu, address);
    if (dev) return dev->peek ? dev->peek(emu, address) : dev->read(emu, address);
    return via_read(&emu->via, (uint8_t)(address & 0x0F));
}

/* ── main.c : psg_decode ───────────────────────────────────────────────── */
/* ORIC PSG (AY-3-8912) is controlled via VIA (from Oricutron):
 * - VIA Port A (ORA) = PSG data bus
 * - VIA CA2 output = PSG BC1, VIA CB2 output = PSG BDIR */
static void psg_decode(emulator_t* emu) {
    /* BC1 = CA2 output state (PCR bits 1-3) */
    uint8_t ca2_mode = (emu->via.pcr >> 1) & 0x07;
    bool bc1 = (ca2_mode == 0x07); /* Mode 7 = CA2 high */

    /* BDIR = CB2 output state (PCR bits 5-7) */
    uint8_t cb2_mode = (emu->via.pcr >> 5) & 0x07;
    bool bdir = (cb2_mode == 0x07); /* Mode 7 = CB2 high */

    if (bdir && bc1) {
        /* Latch Address */
        ay_write_address(&emu->psg, emu->via.ora);
    } else if (bdir && !bc1) {
        /* Write Data — timestamped with the current CPU cycle */
        ay_write_data_timed(&emu->psg, emu->via.ora, emu->cpu.cycles);
        /* [port] --psg-trace supprimé */
    } else if (!bdir && bc1) {
        /* Read Data - PSG data goes onto VIA input for Port A reads */
        emu->via.ira = ay_read_data(&emu->psg);
    }
}

/* ── main.c : keyboard_matrix_read ─────────────────────────────────────── */
/* PSG Port A input callback - returns keyboard matrix row data */
static uint8_t keyboard_matrix_read(void* userdata) {
    emulator_t* emu = (emulator_t*)userdata;
    uint8_t col = emu->via.orb & 0x07;
    return emu->keyboard.matrix[col];
}

/* ── main.c : ijk_port_a_read ──────────────────────────────────────────── */
/* VIA Port A external pins callback — IJK joystick interface */
static uint8_t ijk_port_a_read(void* userdata) {
    emulator_t* emu = (emulator_t*)userdata;
    return oric_joystick_port_a_pins(&emu->joystick,
                                     emu->via.ora, emu->via.ddra,
                                     emu->via.orb, emu->via.ddrb);
}

/* ── main.c : portb_read_callback ──────────────────────────────────────── */
/* VIA Port B read callback - keyboard scan result on PB3 (from Oricutron) */
static uint8_t portb_read_callback(void* userdata) {
    emulator_t* emu = (emulator_t*)userdata;

    uint8_t col = emu->via.orb & 0x07;
    uint8_t reg7 = emu->psg.registers[7];
    uint8_t reg14 = emu->psg.registers[14];
    uint8_t mcol = emu->keyboard.matrix[col];
    uint8_t result;

    if (!(reg7 & 0x40)) {
        /* Port A in output mode → PB3 always 0 (no key detected) */
        result = 0xF7;
    } else {
        /* [port] comptage des passes de scan pour --type-keys supprimé */
        /* Check: any pressed key in column matches the inverted mask? */
        uint8_t pressed = (~mcol) & (~reg14) & 0xFF;
        /* PB3 = 1 if any key matches, 0 otherwise. */
        result = pressed ? 0xFF : 0xF7;
    }
    /* [port] --kbd-scan-trace supprimé */
    return result;
}

/* ── main.c : io_write_callback ────────────────────────────────────────── */
static void io_write_callback(uint16_t address, uint8_t value, void* userdata) {
    emulator_t* emu = (emulator_t*)userdata;

    /* I/O bus: registered devices; a device can decline the write. */
    const io_device_t* dev = io_bus_find_write(emu, address);
    if (dev && dev->write(emu, address, value)) return;

    uint8_t reg = (uint8_t)(address & 0x0F);

    /* LOCI snoops VIA ORB writes ($0300) for the cassette motor line
     * (PB6), like the firmware tap_act() hook (Sprint 36f). */
    if (emu->card_on[CARD_IDX_loci] && address == 0x0300) {
        loci_tap_motor(&emu->loci, (value & 0x40) != 0);
        loci_emu_tap_motor(value);     /* co-sim: the firmware's tap_act() (no-op otherwise) */
    }

    /* --tape-signal-free: gates the signal-level cassette motor on ORB PB6 */
    if (emu->cassette.free_gate && address == 0x0300) {
        bool on = (value & 0x40) != 0;
        if (on && !emu->cassette.started) {
            cassette_rewind(&emu->cassette);
            emu->cassette.started = true;
        }
        cassette_set_motor(&emu->cassette, on);
    }

    /* Capture old PCR before VIA write (for printer strobe edge detection) */
    uint8_t old_pcr = emu->via.pcr;

    via_write(&emu->via, reg, value);

    /* Decode PSG bus state ONLY when control lines change (PCR writes). */
    if (reg == VIA_PCR) {
        psg_decode(emu);
        /* Check for Centronics printer STROBE (CA2 forced low → high) */
        oric_printer_check_strobe(&emu->printer, old_pcr, value, emu->via.ora);
    }
}

/* ── main.c : irq_callback ─────────────────────────────────────────────── */
/* VIA IRQ callback - level-triggered: set/clear VIA IRQ source bit */
static void irq_callback(bool state, void* userdata) {
    emulator_t* emu = (emulator_t*)userdata;
    if (state) {
        cpu_irq_set(&emu->cpu, IRQF_VIA);
    } else {
        cpu_irq_clear(&emu->cpu, IRQF_VIA);
    }
}

/* ── main.c : cpu_cycle_tick ───────────────────────────────────────────── */
/* Per-cycle clock callback (registered on the CPU). */
static void cpu_cycle_tick(void* ctx, int cycles) {
    emulator_t* emu = (emulator_t*)ctx;
    /* [port] --cycle-trace supprimé */
    if (cycles == 1) via_tick(&emu->via);
    else             via_update(&emu->via, cycles);
    if (emu->cassette.signal_mode)
        cassette_tick(&emu->cassette, &emu->via, cycles);
    if (emu->tape_capture.active)
        tape_capture_sample(&emu->tape_capture, &emu->via, emu->cpu.cycles);
    /* Timed bus devices (FDC, ...) */
    io_bus_tick(emu, cycles);
}

/* ── main.c : basic_rechain ────────────────────────────────────────────── */
/* Fix BASIC next-line pointers after CLOAD (the ROM does not rechain). */
static void basic_rechain(memory_t* mem) {
    /* TXTTAB = start of BASIC text ($9A/$9B), typically $0501 */
    uint16_t ptr = (uint16_t)(mem->ram[0x9A] | (mem->ram[0x9B] << 8));

    int lines_fixed = 0;
    while (ptr + 1 < 0xC000) {
        uint8_t next_hi = mem->ram[ptr + 1];
        if (next_hi == 0)
            break;

        uint16_t scan = ptr + 4;
        while (scan < 0xC000 && mem->ram[scan] != 0x00)
            scan++;
        scan++;  /* Skip the null terminator */

        uint16_t old_next = (uint16_t)(mem->ram[ptr] | (mem->ram[ptr + 1] << 8));
        if (old_next != scan) {
            mem->ram[ptr]     = (uint8_t)(scan & 0xFF);
            mem->ram[ptr + 1] = (uint8_t)(scan >> 8);
            lines_fixed++;
        }

        ptr = scan;
    }

    if (lines_fixed > 0) {
        log_info("BASIC rechain: fixed %d line pointer(s)", lines_fixed);
    }
}

/* ── main.c : emulator_init ────────────────────────────────────────────── */
static bool emulator_init(emulator_t* emu) {
    log_info("Initializing Phosphoric v%s (port Circle)", EMU_VERSION);

    if (!memory_init(&emu->memory)) {
        log_error("Failed to initialize memory");
        return false;
    }

    cpu_init(&emu->cpu, &emu->memory);

    via_init(&emu->via);
    via_reset(&emu->via);

    ula_ng_init(&emu->ula_ng);   /* ULA-NG locked at startup (HCS10017 state) */

    cassette_init(&emu->cassette);

    oric_keyboard_init(&emu->keyboard);
    oric_joystick_init(&emu->joystick);
    oric_printer_init(&emu->printer);

    /* Card modules, present or not: idle state. */
    card_modules_init(emu);

    /* Initialize PSG (AY-3-8912) with keyboard input callback */
    ay_init(&emu->psg, ORIC_CLOCK_HZ);
    emu->psg.porta_input = keyboard_matrix_read;
    emu->psg.userdata = emu;

    /* Wire up I/O callbacks */
    memory_set_io_callbacks(&emu->memory, io_read_callback, io_write_callback, emu);
    memory_set_io_peek(&emu->memory, io_peek_callback);
    via_set_irq_callback(&emu->via, irq_callback, emu);

    /* Exposes the bus table to serialisation: devices that provide a
     * save/load hook persist their .ost state (LOCI « reprendre la session »). */
    int io_bus_n = 0;
    const io_device_t* io_bus_tbl = io_bus_devices(&io_bus_n);
    savestate_set_io_devices(io_bus_tbl, io_bus_n);

    cpu_set_cycle_callback(&emu->cpu, cpu_cycle_tick, emu);

    emu->via.porta_read = ijk_port_a_read;
    emu->via.portb_read = portb_read_callback;
    emu->via.userdata = emu;

    video_init(&emu->video);

    /* ULA-NG palette-indirection : wire video to the NG LUT. */
    emu->video.ng_pal        = emu->ula_ng.pal;
    emu->video.ng_active     = &emu->ula_ng.active;
    emu->video.ng_scrstart   = &emu->ula_ng.scrstart;
    emu->video.ng_scrollx    = &emu->ula_ng.scrollx;
    emu->video.ng_scrolly    = &emu->ula_ng.scrolly;
    emu->video.ng_attr       = emu->ula_ng.attr;
    emu->video.ng_attr_active = &emu->ula_ng.attr_active;
    emu->video.ng_dev        = &emu->ula_ng;
    emu->video.ng_chunky_active = &emu->ula_ng.chunky_active;
    emu->video.ng_text80_active = &emu->ula_ng.text80_active;
    emu->video.ng_vram        = emu->ula_ng.vram;
    emu->video.ng_vram_active = &emu->ula_ng.vram_active;

    /* [port] renderer_init / audio_init (SDL), hostfs_init, debugger_init
     *        supprimés : l'affichage et le son sont faits par le kernel Circle. */

    emu->running = true;
    emu->kbd_scan_prev_col = 0xFF;
    /* [port] initialisation des captures/traces/when-* supprimée */

    log_info("Emulator initialized successfully");
    return true;
}

/* ── main.c : main_setup_tape (branche sans fast-load) ─────────────────── */
static bool load_tape(emulator_t* emu, const char* tape_file) {
    FILE* f = fopen(tape_file, "rb");
    if (!f) {
        log_error("Failed to open tape: %s", tape_file);
        return false;
    }
    fseek(f, 0, SEEK_END);
    emu->tapelen = ftell(f);
    fseek(f, 0, SEEK_SET);
    emu->tapebuf = (uint8_t*)malloc(emu->tapelen);
    if (emu->tapebuf) {
        size_t rd = fread(emu->tapebuf, 1, emu->tapelen, f);
        if ((int)rd == emu->tapelen) {
            emu->tapeoffs = 0;
            emu->tape_loaded = true;
            emu->tape_syncstack = -1;
            log_info("Tape loaded: %d bytes (CLOAD\"\" to read)", emu->tapelen);
        }
    }
    fclose(f);
    return emu->tape_loaded;
}

/* ── Instance unique de la machine (main() : « emulator_t emu; ») ─────── */
static emulator_t s_emu;                   /* ~2,7 Mo : hors pile */

_Static_assert(ORIC_MAX_DRIVES == MICRODISC_MAX_DRIVES, "ORIC_MAX_DRIVES");

/* ── Remplace main() + main_setup_machine/rom_model/tape/disks_speech ──── */
bool oric_machine_init(const oric_media_t* media) {
    emulator_t* emu = &s_emu;
    memset(emu, 0, sizeof(*emu));          /* main() : memset(&emu, 0, ...) */
    emu->breakpoint = -1;
    emu->headless = true;                  /* pas de renderer SDL */

    /* Options « ligne de commande » minimales attendues par les cartes. */
    static cli_opts_t cfg;                 /* (cli_opts_init non repris) */
    memset(&cfg, 0, sizeof(cfg));
    cfg.card_cfg = card_modules_cfg_new();
    cfg.rom_file = media->rom_file;
    cfg.disk_rom_file = media->disk_rom_file;
    for (int i = 0; i < MICRODISC_MAX_DRIVES; i++)
        cfg.disk_files[i] = media->disk_files[i];
    /* --fdc-timing fast|real (lu par card_microdisc.c) */
    cfg.fdc_timing_arg = media->fdc_timing;
    /* cli_opts_init : réglages fins LOCI désactivés (-1) */
    cfg.loci_mia_win_lo = cfg.loci_mia_win_hi = -1;
    cfg.loci_serve_cycles = cfg.loci_tdsr_ns = -1;
    cfg.loci_serve_jitter = -1;
    /* --loci, --loci-flash DIR, --loci-usb DIR */
    cfg.loci_enabled = media->loci;
    cfg.loci_flash_root = media->loci_flash_dir;
    if (media->loci_usb_dir) {
        cfg.loci_usb_args[0] = media->loci_usb_dir;
        cfg.loci_usb_count = 1;
    }

    /* main_setup_machine */
    if (!emulator_init(emu)) return false;
    if (card_modules_setup(emu, &cfg, CARD_STAGE_MACHINE) != 0) return false;

    /* main_setup_loci */
    if (card_modules_setup(emu, &cfg, CARD_STAGE_LOCI) != 0) return false;

    /* main_setup_rom_model */
    if (!media->rom_file || !memory_load_rom(&emu->memory, media->rom_file, 0)) {
        log_error("Failed to load ROM: %s", media->rom_file ? media->rom_file : "(none)");
        return false;
    }
    /* Direct LOCI menu ROM boot (-r roms/loci/locirom --loci): patch
     * the firmware version/timing placeholders like the real MIA. */
    if (emu->card_on[CARD_IDX_loci])
        loci_patch_rom_info(emu);
    emu->model = detect_rom_version(&emu->memory);
    emu->rom_patches = get_rom_patches(emu->model);
    log_info("Machine model: %s, ROM patches: %s",
             emu->model == ORIC_MODEL_ATMOS ? "ORIC Atmos" : "ORIC-1",
             emu->rom_patches->name);

    /* main_setup_tape (sans fast-load) */
    if (media->tape_file) load_tape(emu, media->tape_file);

    /* main_setup_disks_speech : Jasmin (absent), puis Microdisc */
    if (card_modules_setup(emu, &cfg, CARD_STAGE_DISKS_EARLY) != 0) return false;
    if (card_modules_setup(emu, &cfg, CARD_STAGE_DISKS) != 0) return false;

    /* emulator_run : power-on reset */
    cpu_reset(&emu->cpu);
    log_info("Starting emulation at PC=$%04X", emu->cpu.PC);
    return true;
}

/* main.c sdl_function_key : F5 = cpu_reset, Maj+F5 = cpu_nmi */
void oric_machine_reset(void) {
    cpu_reset(&s_emu.cpu);
}

void oric_machine_nmi(void) {
    cpu_nmi(&s_emu.cpu);
}

/* main.c sdl_function_key (SDLK_F8) et SDL_KEYUP : bouton Action de LOCI.
 * Appui = instantané de session + piège IRQ ; relâchement = menu LOCI. */
void oric_machine_loci_button(bool down) {
    emulator_t* emu = &s_emu;
    if (!emu->card_on[CARD_IDX_loci]) return;
    if (down) {
        loci_action_button_short(&emu->loci);
    } else {
        emu->loci_button_long = false;     /* [port] appui long (ROM de diagnostic) non repris */
        loci_action_button_release(&emu->loci);
    }
}

/* loci_glue.c loci_sync_kbd_from_sdl : rapport clavier HID transmis à LOCI
 * (le clavier USB branché sur la cartouche, vu par l'API de LOCI). */
void oric_machine_loci_kbd_report(uint8_t modifiers, const uint8_t keys[6]) {
    if (s_emu.card_on[CARD_IDX_loci])
        loci_kbd_set_report(&s_emu.loci, modifiers, keys);
}

/* ── main.c : run_frame_instructions ───────────────────────────────────── */
void oric_machine_run_frame(void) {
    emulator_t* emu = &s_emu;
    emu_clock_frame_begin(emu);   /* master clock: start (or resume) of frame */
    while (emu->raster_cycle < CYCLES_PER_FRAME && !emu->cpu.halted) {
        /* [port] breakpoint, debugger, trace, profiler supprimés */

        tape_patches(emu);

        /* Jasmin autoboot trap (gardé : la carte Jasmin fait partie du noyau) */
        if (emu->card_on[CARD_IDX_jasmin] && !emu->jasmin.autoboot_done && !emu->jasmin.romdis) {
            uint16_t jtrap = (emu->model == ORIC_MODEL_ATMOS) ? 0xEB78 : 0xE905;
            if (emu->cpu.PC == jtrap) {
                jasmin_write(&emu->jasmin, JASMIN_ROMDIS, 1);
                emu->memory.jasmin_romdis = emu->jasmin.romdis;
                emu->memory.jasmin_olay   = emu->jasmin.olay;
                cpu_reset(&emu->cpu);
                emu->jasmin.autoboot_done = true;
            }
        }

        emu_step(emu);

        /* [port] LOCI idle poll supprimé */

        if (emu->tape_readbyte_active && emu->rom_patches &&
            emu->cpu.PC == emu->rom_patches->cload_data_rts) {
            if (emu->memory.ram[emu->rom_patches->tape_type_addr] == 0x00) {
                basic_rechain(&emu->memory);
            }
            emu->tape_readbyte_active = false;
        }
    }

    /* Finishes the frame (remaining lines if the CPU stopped midway). */
    emu_clock_frame_end(emu);
}

/* ── Accès pour le kernel Circle (ajout du port) ───────────────────────── */
const uint8_t* oric_machine_framebuffer(int* w, int* h) {
    *w = s_emu.video.native_w;
    *h = s_emu.video.native_h;
    return s_emu.video.framebuffer;
}

uint8_t* oric_machine_keyboard_matrix(void) {
    return s_emu.keyboard.matrix;
}

void oric_machine_keyboard_release_all(void) {
    oric_keyboard_release_all(&s_emu.keyboard);
}

int oric_machine_audio_rate(void) {
    return AUDIO_SAMPLE_RATE;
}

void oric_machine_audio(int16_t* stereo, int n) {
    ay_generate(&s_emu.psg, stereo, n);
}

void oric_machine_quiet_log(void) {
    log_init(LOG_LEVEL_WARNING);
}

const char* oric_machine_version(void) {
    return EMU_VERSION;
}
