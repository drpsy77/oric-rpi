//
// journal.h — Journal de Phosphoric dans un fichier de la carte SD
//
// Les messages (log_info, log_warning...) ne s'affichent plus par-dessus
// l'image : ils sont gardés en mémoire, depuis n'importe quel cœur, puis
// écrits sur la carte SD par le cœur 0 (JournalFlush). Écrit pour le port.
//
#ifndef _journal_h
#define _journal_h

#include <stdio.h>

// Ouvre le journal : l'ancien pFileName devient pPrevName (journal de la
// session précédente, utile après un débranchement). Retourne le flux à
// donner à Phosphoric (log_set_stream), 0 en cas d'échec.
FILE *JournalOpen (const char *pFileName, const char *pPrevName);

// Message du port (n'importe quel cœur), sans passer par le flux de
// Phosphoric : deux cœurs ne doivent pas écrire dans le même FILE.
void JournalPrintf (const char *pFormat, ...);

// Ajoute au fichier ce qui est en attente (cœur 0 seulement).
void JournalFlush (void);

#endif
