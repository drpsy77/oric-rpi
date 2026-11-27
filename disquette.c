/* SPDX-License-Identifier: EUPL-1.2 */
/*
 * disquette.c — Réécriture des .dsk modifiés sur la carte SD (port Circle).
 *
 * Écrit pour le port, sauf mfm_crc16 et mfm_inject_track, COPIÉES de
 * Phosphoric src/storage/sedoric.c (fonctions statiques, © 2026 bmarty,
 * EUPL-1.2, voir SOURCES.md).
 *
 * Phosphoric charge chaque .dsk en mémoire (secteurs à plat). Le Microdisc
 * ne le réécrit qu'à la sortie de l'émulateur (--disk-writeback) : sur la Pi,
 * on coupe le courant, rien n'est jamais réécrit. Le vrai LOCI écrit sur la
 * clé au fil de l'eau ; Phosphoric aussi depuis 2.31.17 (loci_dsk_sync, à la
 * fin de chaque commande d'écriture), mais par stdio, sur le cœur 1 : sur la
 * Pi, chaque secteur écrit par l'Oric ouvrirait, écrirait et fermerait le
 * fichier par FatFs pendant la trame (l'Oric arrêté, comme avant le travail
 * du cœur 2), et une clé retirée perdrait ses écritures. Le port neutralise
 * cette réécriture (« Réécriture de LOCI » plus bas) et garde la sienne pour
 * LOCI comme pour le Microdisc.
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
 * Comparaison sur le cœur 1 entre deux trames ; lecture et écriture du
 * fichier par le cœur 2 pendant que l'Oric continue (voir « Travail du
 * cœur 2 » plus bas).
 */
#include "disquette.h"
#include "cles.h"
#include "storage/sedoric.h"
#include "storage/disk.h"
#include "utils/logging.h"

#include <fatfs/ff.h>
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
/* Réécritures sur la carte : +1 quand le cœur 1 les demande, +1 quand il en
 * relève la fin (impair : en cours). Lu par le cœur 0 (carte.cpp) autour
 * d'une lecture du cœur 2 : une disquette montée réécrite entre-temps est
 * relue. */
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

/* ── Travail du cœur 2 ─────────────────────────────────────────────────
 *
 * Une réécriture se fait en deux temps. Le cœur 1, entre deux trames,
 * compare l'image de Phosphoric au contenu du fichier (copie_t.fichier),
 * recopie les pistes changées dans ce contenu (pistes MFM réencodées) et
 * note leur place : c'est court (memcmp d'un Mo). Le cœur 2 (CarteCoeur2,
 * carte.cpp) les écrit ensuite dans le fichier, pendant que l'Oric
 * continue : sur le cœur 1, chaque réécriture arrêtait l'émulation de 20 à
 * 60 ms (latence de la carte), et la première lecture du fichier bien plus.
 * Contenu du fichier absent (première réécriture, clé remise) : le cœur 2 le
 * lit, la comparaison se fait à la trame suivante.
 *
 * Une case (s_travail), comme celle de carte.cpp : LIBRE → DEMANDE par le
 * cœur 1, DEMANDE → FINI par le cœur 2, FINI → LIBRE par le cœur 1 au début
 * de disquette_service (journal). Tant qu'elle n'est pas libre, le cœur 1
 * ne touche à aucun copie_t (le cœur 2 lit leurs fichiers) : il ne fait que
 * noter les écritures de l'Oric.
 *
 * Le cœur 2 n'utilise que FatFs (f_*, lectures par morceaux de
 * DISQUETTE_MORCEAU : le verrou du volume est rendu entre deux, LOCI sur
 * le cœur 1 n'attend qu'un morceau), malloc et sedoric_mfm_extract_track
 * (calcul seul) : ni stdio ni journal (voir carte.cpp). Tampons de malloc,
 * alignés : sinon diskio.cpp de Circle copie dans son tampon statique,
 * commun à tous les volumes et sans verrou.
 */
#define DISQUETTE_MORCEAU   (64u << 10)

enum { TRAVAIL_LIBRE, TRAVAIL_DEMANDE, TRAVAIL_FINI };

typedef struct {
    copie_t*        c;
    bool            charger;    /* lire le fichier (c->fichier absent) */
    uint32_t        taille;     /* charger : taille de l'image de Phosphoric */
    sedoric_disk_t* lu;         /* charger : contenu lu (NULL : illisible) */
    const uint8_t*  base;       /* écrire : piste i à base + pos[i] ... */
    uint32_t        longueur;   /* ... de cette longueur */
    uint32_t*       pos;        /* ... places dans le fichier (malloc) */
    int             n;
    bool            echec;
} element_t;

typedef struct {
    volatile unsigned etat;
    int             nb;
    bool            ecrit;      /* des pistes à écrire (compteur s_reecritures) */
    bool            sur_cle;    /* un fichier sur une clé USB */
    element_t       el[2 * NB_LECTEURS];
} travail_t;

static travail_t s_travail;

/* Contenu d'un fichier lu en mémoire, rangé comme par sedoric_load (même
 * décodage, écrit d'après lui : sedoric_load lit par stdio). raw est gardé
 * (conteneur MFM) ou libéré. */
static sedoric_disk_t* decoder(uint8_t* raw, uint32_t fsize) {
    sedoric_disk_t* disk = (sedoric_disk_t*)calloc(1, sizeof(sedoric_disk_t));
    if (!disk) { free(raw); return NULL; }

    if (fsize > MFM_DISK_HEADER_SIZE && memcmp(raw, "MFM_DISK", 8) == 0) {
        uint32_t sides  = raw[8]  | ((uint32_t)raw[9]  << 8) | ((uint32_t)raw[10] << 16) | ((uint32_t)raw[11] << 24);
        uint32_t tracks = raw[12] | ((uint32_t)raw[13] << 8) | ((uint32_t)raw[14] << 16) | ((uint32_t)raw[15] << 24);
        if (sides > MFM_MAX_SIDES) sides = MFM_MAX_SIDES;
        if (tracks > MFM_MAX_TRACKS) tracks = MFM_MAX_TRACKS;
        uint8_t spt = MFM_MAX_SECTORS;
        uint32_t flat_size = sides * tracks * spt * SEDORIC_SECTOR_SIZE;
        disk->data = (uint8_t*)calloc(1, flat_size ? flat_size : 1);
        if (!disk->data) { free(disk); free(raw); return NULL; }
        for (uint32_t s = 0; s < sides; s++) {
            for (uint32_t t = 0; t < tracks; t++) {
                uint32_t raw_offset = MFM_DISK_HEADER_SIZE + (s * tracks + t) * MFM_TRACK_SIZE;
                if (raw_offset + MFM_TRACK_SIZE > fsize) break;
                sedoric_mfm_extract_track(&raw[raw_offset], disk->data, flat_size,
                                          spt, (uint8_t)tracks);
            }
        }
        disk->size = flat_size;
        disk->tracks = (uint8_t)tracks;
        disk->sectors = spt;
        disk->sides = (uint8_t)sides;
        disk->is_mfm = true;
        disk->mfm_raw = raw;
        disk->mfm_raw_size = fsize;
        return disk;
    }

    /* Secteurs à plat : seuls size et sectors servent ici */
    disk->data = raw;
    disk->size = fsize;
    disk->sectors = SEDORIC_SECTORS;
    return disk;
}

/* Cœur 2 : lecture d'un fichier (NULL : absent, vide ou illisible) */
static sedoric_disk_t* charger(const char* path) {
    FIL f;
    if (f_open(&f, path, FA_READ | FA_OPEN_EXISTING) != FR_OK) return NULL;
    FSIZE_t fsize = f_size(&f);
    uint8_t* raw = (fsize > 0 && fsize < 0x10000000) ? (uint8_t*)malloc((size_t)fsize) : NULL;
    bool ok = raw != NULL;
    for (uint32_t p = 0; ok && p < fsize; ) {
        UINT n = (fsize - p > DISQUETTE_MORCEAU) ? DISQUETTE_MORCEAU : (UINT)(fsize - p);
        UINT lu = 0;
        ok = f_read(&f, raw + p, n, &lu) == FR_OK && lu == n;
        p += n;
    }
    f_close(&f);
    if (!ok) { free(raw); return NULL; }
    return decoder(raw, (uint32_t)fsize);
}

/* Cœur 2 : pistes écrites sur place (fopen "r+b" avant) */
static bool ecrire(const char* path, const element_t* e) {
    FIL f;
    if (f_open(&f, path, FA_WRITE | FA_OPEN_EXISTING) != FR_OK) return false;
    bool ok = true;
    for (int i = 0; ok && i < e->n; i++) {
        UINT ecrit = 0;
        ok = f_lseek(&f, e->pos[i]) == FR_OK
          && f_write(&f, e->base + e->pos[i], e->longueur, &ecrit) == FR_OK
          && ecrit == e->longueur;
    }
    if (f_close(&f) != FR_OK) ok = false;
    return ok;
}

/* Cœur 2, ou sur place (disquette_flush) */
static void executer(travail_t* t) {
    for (int i = 0; i < t->nb; i++) {
        element_t* e = &t->el[i];
        if (e->charger) e->lu = charger(e->c->path);
        else e->echec = !ecrire(e->c->path, e);
    }
}

/* Cœur 1 : pistes où l'image diffère du fichier, recopiées dans son contenu
 * et notées dans la case ; contenu absent : lecture demandée. */
static void preparer(travail_t* t, copie_t* c, const uint8_t* image, uint32_t size,
                     const char* path) {
    associer(c, image, path);
    if (c->refus) return;
    if (c->cle && !cle_prete(c)) return;

    element_t* e = &t->el[t->nb];
    memset(e, 0, sizeof(*e));
    e->c = c;
    if (!c->fichier) {
        e->charger = true;
        e->taille = size;
        t->nb++;
        if (c->cle) t->sur_cle = true;
        return;
    }

    sedoric_disk_t* f = c->fichier;
    uint32_t piste = (uint32_t)f->sectors * SEDORIC_SECTOR_SIZE;
    uint32_t nb_pistes = size / piste;
    for (uint32_t k = 0; k < nb_pistes; k++) {
        uint32_t off = k * piste;
        if (memcmp(image + off, f->data + off, piste) == 0) continue;
        if (!e->pos && !(e->pos = (uint32_t*)malloc(nb_pistes * sizeof(uint32_t)))) return;
        memcpy(f->data + off, image + off, piste);

        /* Piste k du fichier : face 0 puis face 1, comme sedoric_load */
        uint32_t pos = off;
        if (f->is_mfm) {
            pos = MFM_DISK_HEADER_SIZE + k * MFM_TRACK_SIZE;
            if (pos + MFM_TRACK_SIZE > f->mfm_raw_size) continue;
            mfm_inject_track(&f->mfm_raw[pos], f->data, f->size, f->sectors, f->tracks);
        }
        e->pos[e->n++] = pos;
    }
    if (e->n == 0) {
        free(e->pos);
        return;
    }
    e->base = f->is_mfm ? f->mfm_raw : f->data;
    e->longueur = f->is_mfm ? MFM_TRACK_SIZE : piste;
    t->nb++;
    t->ecrit = true;
    if (c->cle) t->sur_cle = true;
}

/* Cœur 1, entre deux trames : remplit la case pour toutes les disquettes
 * montées ; vrai s'il y a quelque chose à faire. */
static bool preparer_tout(emulator_t* emu) {
    travail_t* t = &s_travail;
    s_calme = -1;
    t->nb = 0;
    t->ecrit = false;
    t->sur_cle = false;

    /* Disquettes de LOCI (dsk_image : secteurs à plat, comme sedoric_load) */
    for (int d = 0; d < NB_LECTEURS; d++) {
        const loci_t* l = &emu->loci;
        if (emu->card_on[CARD_IDX_loci] && l->dsk_image[d] && !l->dsk_web[d] && l->dsk_host_path[d][0])
            preparer(t, &s_loci[d], l->dsk_image[d], l->dsk_image_size[d], l->dsk_host_path[d]);
        else
            oublier(&s_loci[d]);
    }
    /* Disquettes du Microdisc (disk=) */
    for (int d = 0; d < NB_LECTEURS; d++) {
        const sedoric_disk_t* disk = emu->disks[d];
        if (emu->card_on[CARD_IDX_microdisc] && disk && disk->data && s_microdisc_path[d])
            preparer(t, &s_microdisc[d], disk->data, disk->size, s_microdisc_path[d]);
        else
            oublier(&s_microdisc[d]);
    }
    if (t->ecrit) {
        s_reecritures++;        /* impair : réécriture en cours */
        __sync_synchronize();
    }
    return t->nb > 0;
}

/* Cœur 1 (ou celui de disquette_flush) : résultats de la case, journal */
static void recueillir(travail_t* t) {
    __sync_synchronize();
    bool lu = false;
    for (int i = 0; i < t->nb; i++) {
        element_t* e = &t->el[i];
        copie_t* c = e->c;
        if (e->charger) {
            unsigned serie, branchement;
            if (e->lu && e->lu->size == e->taille && e->lu->sectors != 0) {
                c->fichier = e->lu;
                lu = true;
            } else if (!e->lu && c->cle && cles_usb_etat(c->path, &serie, &branchement) == 0) {
                /* clé retirée avant la lecture : nouvel essai à son retour */
                if (!c->attente) log_warning("[port] %s: USB key removed, write-back postponed", c->path);
                c->attente = true;
            } else {
                log_warning("[port] %s: geometry differs from the mounted image, not written back", c->path);
                sedoric_destroy(e->lu);
                c->refus = true;
            }
        } else {
            if (e->echec) {
                /* fichier dans un état inconnu : relu à la prochaine
                 * réécriture ; clé retirée pendant l'écriture : nouvel
                 * essai à son retour */
                log_error("[port] %s: write-back failed", c->path);
                sedoric_destroy(c->fichier);
                c->fichier = NULL;
                if (c->cle) c->attente = true;
            } else {
                log_info("[port] %s: %d track(s) written back", c->path, e->n);
            }
            free(e->pos);
        }
    }
    if (t->ecrit) {
        __sync_synchronize();
        s_reecritures++;
    }
    t->nb = 0;
    t->ecrit = false;
    t->sur_cle = false;
    /* contenu lu : comparaison à la trame suivante (sauf écriture de l'Oric
     * d'ici là : 2 s plus tard, comme d'habitude) */
    if (lu && s_calme < 0) s_calme = DISQUETTE_DELAI_TRAMES - 1;
    __sync_synchronize();
    t->etat = TRAVAIL_LIBRE;
}

/* Cœur 1 : la case remplie part au cœur 2 (réveillé par SEV, WFE dans
 * CarteCoeur2) */
static void lancer(void) {
    __sync_synchronize();
    s_travail.etat = TRAVAIL_DEMANDE;
    __asm__ volatile ("dsb sy\n\tsev" ::: "memory");
}

bool disquette_coeur2_demande(void) {
    return s_travail.etat == TRAVAIL_DEMANDE;
}

bool disquette_coeur2_sur_cle(void) {
    return s_travail.etat == TRAVAIL_DEMANDE && s_travail.sur_cle;
}

void disquette_coeur2(void) {
    if (s_travail.etat != TRAVAIL_DEMANDE) return;
    __sync_synchronize();
    executer(&s_travail);
    __sync_synchronize();
    s_travail.etat = TRAVAIL_FINI;
}

/* ── Réécriture de LOCI (Phosphoric 2.31.17) neutralisée ─────────────── */
/* [port] loci_dsk_sync (loci_bus.c, statique) réécrit le fichier à la fin de
 * chaque commande d'écriture : loci_dsk_flush compare l'image à sa copie
 * dsk_shadow et écrit les secteurs changés (fopen, fwrite, fclose sur le
 * cœur 1), puis remet disk_modified à faux. Trois détournements (--wrap,
 * Makefile), appelés depuis d'autres fichiers de Phosphoric :
 *   - loci_dsk_open (MOUNT, locidisk=) : dsk_shadow libérée après le
 *     montage ; sans elle, loci_dsk_flush ne fait rien (Phosphoric le
 *     prévoit : malloc refusé) ;
 *   - loci_dsk_tick (io_bus.c) : contrôleur seulement, comme avant 2.31.17,
 *     sans loci_dsk_sync : disk_modified reste vrai jusqu'à disquette_service ;
 *   - loci_dsk_write (card_loci.c) : son loci_dsk_sync final peut remettre
 *     disk_modified à faux (dernier octet, interruption forcée) ; une commande
 *     d'écriture vue avant ou après l'appel est notée dans s_loci_ecrit.
 * Une écriture notée de trop ne coûte qu'une comparaison. Proposé à bmarty :
 * point 10 de docs/phosphoric-f8.md. */
static bool s_loci_ecrit;

static bool fdc_en_ecriture(const fdc_t* fdc) {
    return fdc->disk_modified || fdc->currentop == FDC_OP_WRITE_SECTOR ||
           fdc->currentop == FDC_OP_WRITE_SECTORS || fdc->currentop == FDC_OP_WRITE_TRACK;
}

bool __real_loci_dsk_open(loci_t* loci, uint8_t drive, const char* host_path);
bool __wrap_loci_dsk_open(loci_t* loci, uint8_t drive, const char* host_path) {
    bool ok = __real_loci_dsk_open(loci, drive, host_path);
    if (drive < NB_LECTEURS) {
        free(loci->dsk_shadow[drive]);
        loci->dsk_shadow[drive] = NULL;
    }
    return ok;
}

void __wrap_loci_dsk_tick(loci_t* loci, int cycles) {
    fdc_ticktock(&loci->dsk_fdc, (unsigned)cycles);
}

void __real_loci_dsk_write(loci_t* loci, uint16_t address, uint8_t value);
void __wrap_loci_dsk_write(loci_t* loci, uint16_t address, uint8_t value) {
    bool avant = fdc_en_ecriture(&loci->dsk_fdc);
    __real_loci_dsk_write(loci, address, value);
    if (avant || fdc_en_ecriture(&loci->dsk_fdc)) s_loci_ecrit = true;
}

void disquette_service(emulator_t* emu) {
    /* Réécriture finie par le cœur 2 : journal, case libre */
    if (s_travail.etat == TRAVAIL_FINI) recueillir(&s_travail);

    /* Écritures de la dernière trame (drapeaux remis à zéro par le port :
     * la réécriture de LOCI est neutralisée, voir plus haut ; Phosphoric ne
     * lit disk_dirty qu'à la sortie) */
    bool ecrit = false;
    if (emu->card_on[CARD_IDX_loci] && (emu->loci.dsk_fdc.disk_modified || s_loci_ecrit)) {
        emu->loci.dsk_fdc.disk_modified = false;
        s_loci_ecrit = false;
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
    if (ecrit) s_calme = 0;
    /* Réécriture en cours sur le cœur 2 : rien d'autre */
    if (s_travail.etat != TRAVAIL_LIBRE) return;

    /* Disquette nouvellement montée : sa clé est relevée tout de suite */
    for (int d = 0; d < NB_LECTEURS; d++) {
        const loci_t* l = &emu->loci;
        if (emu->card_on[CARD_IDX_loci] && l->dsk_image[d] && !l->dsk_web[d] && l->dsk_host_path[d][0])
            associer(&s_loci[d], l->dsk_image[d], l->dsk_host_path[d]);
        const sedoric_disk_t* disk = emu->disks[d];
        if (emu->card_on[CARD_IDX_microdisc] && disk && disk->data && s_microdisc_path[d])
            associer(&s_microdisc[d], disk->data, s_microdisc_path[d]);
    }
    if (ecrit) return;
    if (s_calme >= 0 && ++s_calme >= DISQUETTE_DELAI_TRAMES) {
        if (preparer_tout(emu)) lancer();
        return;
    }
    /* Pistes en attente d'une clé retirée : nouvel essai régulier */
    bool attente = false;
    for (int d = 0; d < NB_LECTEURS; d++)
        attente = attente || s_loci[d].attente || s_microdisc[d].attente;
    if (attente && ++s_attente >= DISQUETTE_ATTENTE) {
        s_attente = 0;
        if (preparer_tout(emu)) lancer();
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

/* [port] Une disquette montée dans le dossier dir (ou plus bas) ? Pour le
 * déplacement d'un dossier par WebDAV (carte.cpp). */
bool disquette_montee_sous(emulator_t* emu, const char* dir) {
    char cherche[256], monte[256];
    normaliser(dir, cherche, sizeof(cherche) - 1);
    size_t n = strlen(cherche);
    if (n == 0 || cherche[n - 1] != '/') { cherche[n++] = '/'; cherche[n] = '\0'; }
    for (int d = 0; d < NB_LECTEURS; d++) {
        const loci_t* l = &emu->loci;
        if (emu->card_on[CARD_IDX_loci] && l->dsk_image[d] && l->dsk_host_path[d][0]) {
            normaliser(l->dsk_host_path[d], monte, sizeof(monte));
            if (strncmp(cherche, monte, n) == 0) return true;
        }
        if (emu->card_on[CARD_IDX_microdisc] && emu->disks[d] && s_microdisc_path[d]) {
            normaliser(s_microdisc_path[d], monte, sizeof(monte));
            if (strncmp(cherche, monte, n) == 0) return true;
        }
    }
    return false;
}

void disquette_flush_attente(emulator_t* emu) {
    bool attente = s_calme >= 0 || s_travail.etat != TRAVAIL_LIBRE;
    for (int d = 0; d < NB_LECTEURS; d++)
        attente = attente || s_loci[d].attente || s_microdisc[d].attente;
    if (attente) disquette_flush(emu);
}

unsigned disquette_reecritures(void) {
    __sync_synchronize();
    return s_reecritures;
}

void disquette_flush(emulator_t* emu) {
    /* Réécriture du cœur 2 : sa fin attendue (quelques dizaines de ms) */
    while (s_travail.etat == TRAVAIL_DEMANDE) {
    }
    if (s_travail.etat == TRAVAIL_FINI) recueillir(&s_travail);

    /* Sur place : contenu des fichiers lu s'il manque, puis pistes écrites */
    for (int passe = 0; passe < 2; passe++) {
        if (!preparer_tout(emu)) break;
        bool lecture = false;
        for (int i = 0; i < s_travail.nb; i++) lecture = lecture || s_travail.el[i].charger;
        executer(&s_travail);
        recueillir(&s_travail);
        if (!lecture) break;
    }
}
