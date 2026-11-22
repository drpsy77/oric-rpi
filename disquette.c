/* SPDX-License-Identifier: EUPL-1.2 */
/*
 * disquette.c — Réécriture des .dsk modifiés sur la carte SD (port Circle).
 *
 * Écrit pour le port, sauf mfm_crc16 et mfm_inject_track, COPIÉES de
 * Phosphoric src/storage/sedoric.c (fonctions statiques, © 2026 bmarty,
 * EUPL-1.2, voir SOURCES.md).
 *
 * Phosphoric charge chaque .dsk en mémoire (secteurs à plat) et ne le
 * réécrit qu'à la sortie de l'émulateur : loci_dsk_flush (LOCI, jamais pour
 * une MFM_DISK : « session-only ») ou à la fermeture (Microdisc). Sur la Pi,
 * on coupe le courant : rien n'est jamais réécrit. Le vrai LOCI écrit sur la
 * clé au fil de l'eau.
 *
 * Le port garde pour chaque disquette l'état de son fichier (sedoric_load :
 * secteurs à plat et conteneur MFM). DISQUETTE_DELAI_TRAMES trames après la
 * dernière écriture de l'Oric, il compare piste par piste l'image de
 * Phosphoric à cet état et réécrit sur place les seules pistes changées
 * (6400 octets en MFM_DISK, secteurs réencodés et CRC recalculés ; 4352 en
 * format brut). Une coupure pendant l'écriture n'abîme qu'une piste.
 *
 * Disquette montée depuis une vraie clé USB (« USB:/... », cles.cpp) : la
 * clé peut être retirée à tout moment, sans prévenir. L'image restant en
 * mémoire, l'Oric continue de la lire et d'y écrire ; les pistes changées
 * attendent le retour de la clé (nouvel essai toutes les DISQUETTE_ATTENTE
 * trames) et ne sont réécrites que sur la même clé (numéro de série du
 * volume, relevé quand la disquette est vue pour la première fois). Clé
 * remise : le fichier est relu avant la comparaison (il a pu changer
 * ailleurs ; l'image de l'Oric l'emporte sur les pistes qui diffèrent).
 *
 * Appelé sur le cœur 1 entre deux trames, comme les accès de LOCI à la carte.
 */
#include "disquette.h"
#include "cles.h"
#include "storage/sedoric.h"
#include "utils/logging.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define DISQUETTE_DELAI_TRAMES  100     /* 2 s sans écriture */
#define NB_LECTEURS             4
#define DISQUETTE_ATTENTE       50      /* clé absente : nouvel essai toutes les 1 s */

/* État du fichier d'une disquette montée */
typedef struct {
    const uint8_t*  image;      /* image de Phosphoric (clé, avec le chemin) */
    char            path[256];
    sedoric_disk_t* fichier;    /* contenu du fichier ; NULL : à relire */
    bool            refus;      /* géométrie différente : fichier non touché */
    /* Disquette d'une clé USB (cles_usb_etat) */
    bool            cle;        /* chemin sur une clé */
    bool            serie_lue;  /* numéro de série de la clé relevé */
    unsigned        serie;      /* ... celui de la clé d'origine */
    unsigned        branchement;/* branchement où le fichier a été lu */
    bool            attente;    /* pistes en attente de la clé */
} copie_t;

static copie_t s_loci[NB_LECTEURS], s_microdisc[NB_LECTEURS];
static const char* s_microdisc_path[NB_LECTEURS];
static int s_calme = -1;        /* trames depuis la dernière écriture ; -1 : rien */
static int s_attente;           /* trames depuis le dernier essai (clé absente) */
/* Réécritures sur la carte : +1 avant, +1 après (impair : en cours). Lu par
 * le cœur 0 (carte.cpp) autour d'une lecture du cœur 2 : une disquette
 * montée réécrite pendant qu'il la lit est relue. */
static volatile unsigned s_reecritures;

void disquette_set_microdisc_path(int drive, const char* path) {
    if (drive >= 0 && drive < NB_LECTEURS) s_microdisc_path[drive] = path;
}

/* ── sedoric.c : mfm_crc16 (copie) ─────────────────────────────────────── */
static uint16_t mfm_crc16(const uint8_t* data, int len) {
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < len; i++) {
        crc ^= (uint16_t)((uint16_t)data[i] << 8);
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

/* ── sedoric.c : mfm_inject_track (copie) ──────────────────────────────── */
static int mfm_inject_track(uint8_t* track_data, const uint8_t* flat, uint32_t flat_size,
                            uint8_t sectors_per_track, uint8_t num_tracks) {
    int written = 0;
    for (int i = 0; i < MFM_TRACK_SIZE - 4; i++) {
        if (track_data[i] == 0xA1 && track_data[i+1] == 0xA1 &&
            track_data[i+2] == 0xA1 && track_data[i+3] == 0xFE) {
            uint8_t id_track  = track_data[i+4];
            uint8_t id_side   = track_data[i+5];
            uint8_t id_sector = track_data[i+6];

            if (id_sector < 1 || id_sector > sectors_per_track) continue;

            /* mark (4) + data (256) + CRC (2) must fit in the track */
            for (int j = i + 10; j < i + 60 && j + 4 + SEDORIC_SECTOR_SIZE + 2 <= MFM_TRACK_SIZE; j++) {
                if (track_data[j] == 0xA1 && track_data[j+1] == 0xA1 &&
                    track_data[j+2] == 0xA1 && track_data[j+3] == 0xFB) {
                    uint32_t offset = ((uint32_t)id_side * num_tracks * sectors_per_track +
                                       (uint32_t)id_track * sectors_per_track +
                                       (uint32_t)(id_sector - 1)) * SEDORIC_SECTOR_SIZE;
                    /* Same bound as extraction (ID marks come from the file). */
                    if (offset > flat_size || flat_size - offset < SEDORIC_SECTOR_SIZE) break;
                    /* Overwrite the 256-byte data field */
                    memcpy(&track_data[j+4], flat + offset, SEDORIC_SECTOR_SIZE);
                    /* Recompute CRC over $A1 $A1 $A1 $FB + 256 data bytes */
                    uint16_t crc = mfm_crc16(&track_data[j], 4 + SEDORIC_SECTOR_SIZE);
                    track_data[j + 4 + SEDORIC_SECTOR_SIZE]     = (uint8_t)(crc >> 8);
                    track_data[j + 4 + SEDORIC_SECTOR_SIZE + 1] = (uint8_t)(crc & 0xFF);
                    written++;
                    break;
                }
            }
        }
    }
    return written;
}

/* ── Écrit pour le port ────────────────────────────────────────────────── */

static void oublier(copie_t* c) {
    sedoric_destroy(c->fichier);
    memset(c, 0, sizeof(*c));
}

/* Disquette (image, chemin) dans ce lecteur : nouvelle, on oublie l'ancienne
 * et on relève la clé dont elle vient (sa clé est là : elle vient d'être lue) */
static void associer(copie_t* c, const uint8_t* image, const char* path) {
    if (c->image == image && strncmp(c->path, path, sizeof(c->path)) == 0) return;
    oublier(c);                 /* autre disquette dans le lecteur */
    c->image = image;
    strncpy(c->path, path, sizeof(c->path) - 1);
    unsigned serie = 0, branchement = 0;
    int etat = cles_usb_etat(path, &serie, &branchement);
    c->cle = etat >= 0;
    if (etat == 1) {
        c->serie_lue = true;
        c->serie = serie;
        c->branchement = branchement;
    }
}

/* Disquette d'une clé : peut-on écrire maintenant ? (sinon : en attente) */
static bool cle_prete(copie_t* c) {
    unsigned serie = 0, branchement = 0;
    int etat = cles_usb_etat(c->path, &serie, &branchement);
    if (etat == 0) {
        if (!c->attente) log_warning("[port] %s: USB key removed, write-back postponed", c->path);
        c->attente = true;
        return false;
    }
    if (!c->serie_lue) {        /* clé absente quand la disquette a été vue */
        c->serie_lue = true;
        c->serie = serie;
        c->branchement = branchement;
    }
    if (serie != c->serie) {
        if (!c->attente)
            log_warning("[port] %s: another USB key (serial %08X, not %08X), nothing written",
                        c->path, serie, c->serie);
        c->attente = true;
        return false;
    }
    if (branchement != c->branchement) {
        /* même clé, remise : fichier relu (il a pu changer ailleurs) */
        sedoric_destroy(c->fichier);
        c->fichier = NULL;
        c->branchement = branchement;
        log_info("[port] %s: USB key back, pending tracks written back", c->path);
    }
    c->attente = false;
    return true;
}

/* Réécrit dans le fichier les pistes où l'image diffère de son contenu. */
static void sauver(copie_t* c, const uint8_t* image, uint32_t size, const char* path) {
    associer(c, image, path);
    if (c->refus) return;
    if (c->cle && !cle_prete(c)) return;

    if (!c->fichier) {
        c->fichier = sedoric_load(path);
        if (!c->fichier || c->fichier->size != size || c->fichier->sectors == 0) {
            log_warning("[port] %s: geometry differs from the mounted image, not written back", path);
            sedoric_destroy(c->fichier);
            c->fichier = NULL;
            c->refus = true;
            return;
        }
    }

    sedoric_disk_t* f = c->fichier;
    uint32_t piste = (uint32_t)f->sectors * SEDORIC_SECTOR_SIZE;
    FILE* fp = NULL;
    int n = 0;
    bool echec = false;
    bool compte = false;        /* s_reecritures déjà augmenté */
    for (uint32_t k = 0; k < size / piste; k++) {
        uint32_t off = k * piste;
        if (memcmp(image + off, f->data + off, piste) == 0) continue;
        memcpy(f->data + off, image + off, piste);

        /* Piste k du fichier : face 0 puis face 1, comme sedoric_load */
        const uint8_t* bloc = f->data + off;
        long pos = (long)off;
        size_t len = piste;
        if (f->is_mfm) {
            pos = MFM_DISK_HEADER_SIZE + (long)k * MFM_TRACK_SIZE;
            if (pos + MFM_TRACK_SIZE > (long)f->mfm_raw_size) continue;
            mfm_inject_track(&f->mfm_raw[pos], f->data, f->size, f->sectors, f->tracks);
            bloc = &f->mfm_raw[pos];
            len = MFM_TRACK_SIZE;
        }

        if (!compte) {
            compte = true;
            s_reecritures++;
            __sync_synchronize();
        }
        if (!fp && !(fp = fopen(path, "r+b"))) { echec = true; break; }
        if (fseek(fp, pos, SEEK_SET) != 0 || fwrite(bloc, 1, len, fp) != len) { echec = true; break; }
        n++;
    }
    if (fp && fclose(fp) != 0) echec = true;
    if (compte) {
        __sync_synchronize();
        s_reecritures++;
    }

    if (echec) {
        /* fichier dans un état inconnu : relu à la prochaine réécriture ;
         * clé retirée pendant l'écriture : nouvel essai à son retour */
        log_error("[port] %s: write-back failed", path);
        sedoric_destroy(c->fichier);
        c->fichier = NULL;
        if (c->cle) c->attente = true;
    } else if (n > 0) {
        log_info("[port] %s: %d track(s) written back", path, n);
    }
}

void disquette_service(emulator_t* emu) {
    /* Écritures de la dernière trame (drapeaux remis à zéro par le port :
     * LOCI ne lit pas disk_modified, Phosphoric ne lit disk_dirty qu'à la
     * sortie) */
    bool ecrit = false;
    if (emu->card_on[CARD_IDX_loci] && emu->loci.dsk_fdc.disk_modified) {
        emu->loci.dsk_fdc.disk_modified = false;
        ecrit = true;
    }
    if (emu->card_on[CARD_IDX_microdisc]) {
        for (int d = 0; d < NB_LECTEURS; d++) {
            if (emu_disk_dirty(emu, d)) {
                emu_disk_clear_dirty(emu, d);
                ecrit = true;
            }
        }
    }
    /* Disquette nouvellement montée : sa clé est relevée tout de suite */
    for (int d = 0; d < NB_LECTEURS; d++) {
        const loci_t* l = &emu->loci;
        if (emu->card_on[CARD_IDX_loci] && l->dsk_image[d] && !l->dsk_web[d] && l->dsk_host_path[d][0])
            associer(&s_loci[d], l->dsk_image[d], l->dsk_host_path[d]);
        const sedoric_disk_t* disk = emu->disks[d];
        if (emu->card_on[CARD_IDX_microdisc] && disk && disk->data && s_microdisc_path[d])
            associer(&s_microdisc[d], disk->data, s_microdisc_path[d]);
    }
    if (ecrit) {
        s_calme = 0;
        return;
    }
    if (s_calme >= 0 && ++s_calme >= DISQUETTE_DELAI_TRAMES) {
        disquette_flush(emu);
        return;
    }
    /* Pistes en attente d'une clé retirée : nouvel essai régulier */
    bool attente = false;
    for (int d = 0; d < NB_LECTEURS; d++)
        attente = attente || s_loci[d].attente || s_microdisc[d].attente;
    if (attente && ++s_attente >= DISQUETTE_ATTENTE) {
        s_attente = 0;
        disquette_flush(emu);
    }
}

/* Chemin ramené à une forme comparable : sans « SD: » ni « / » ou « ./ » en
 * tête, « / » doubles réduits, minuscules (FAT ne distingue pas la casse). */
static void normaliser(const char* in, char* out, size_t n) {
    if (strncasecmp(in, "SD:", 3) == 0) in += 3;
    size_t k = 0;
    while (*in && k + 1 < n) {
        if (k == 0 || out[k - 1] == '/') {
            if (*in == '/') { in++; continue; }
            if (in[0] == '.' && in[1] == '/') { in += 2; continue; }
        }
        char c = *in++;
        out[k++] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    out[k] = '\0';
}

bool disquette_montee(emulator_t* emu, const char* path) {
    char cherche[256], monte[256];
    normaliser(path, cherche, sizeof(cherche));
    for (int d = 0; d < NB_LECTEURS; d++) {
        const loci_t* l = &emu->loci;
        if (emu->card_on[CARD_IDX_loci] && l->dsk_image[d] && l->dsk_host_path[d][0]) {
            normaliser(l->dsk_host_path[d], monte, sizeof(monte));
            if (strcmp(cherche, monte) == 0) return true;
        }
        if (emu->card_on[CARD_IDX_microdisc] && emu->disks[d] && s_microdisc_path[d]) {
            normaliser(s_microdisc_path[d], monte, sizeof(monte));
            if (strcmp(cherche, monte) == 0) return true;
        }
    }
    return false;
}

void disquette_flush_attente(emulator_t* emu) {
    bool attente = s_calme >= 0;
    for (int d = 0; d < NB_LECTEURS; d++)
        attente = attente || s_loci[d].attente || s_microdisc[d].attente;
    if (attente) disquette_flush(emu);
}

unsigned disquette_reecritures(void) {
    __sync_synchronize();
    return s_reecritures;
}

void disquette_flush(emulator_t* emu) {
    s_calme = -1;

    /* Disquettes de LOCI (dsk_image : secteurs à plat, comme sedoric_load) */
    for (int d = 0; d < NB_LECTEURS; d++) {
        const loci_t* l = &emu->loci;
        if (emu->card_on[CARD_IDX_loci] && l->dsk_image[d] && !l->dsk_web[d] && l->dsk_host_path[d][0])
            sauver(&s_loci[d], l->dsk_image[d], l->dsk_image_size[d], l->dsk_host_path[d]);
        else
            oublier(&s_loci[d]);
    }
    /* Disquettes du Microdisc (disk=) */
    for (int d = 0; d < NB_LECTEURS; d++) {
        const sedoric_disk_t* disk = emu->disks[d];
        if (emu->card_on[CARD_IDX_microdisc] && disk && disk->data && s_microdisc_path[d])
            sauver(&s_microdisc[d], disk->data, disk->size, s_microdisc_path[d]);
        else
            oublier(&s_microdisc[d]);
    }
}
