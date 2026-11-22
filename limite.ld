/*
 * limite.ld — Contrôle ajouté à l'édition de liens (Makefile) : le noyau
 * (code, données et BSS, jusqu'à _end) doit tenir dans KERNEL_MAX_SIZE à
 * partir de MEM_KERNEL_START (0x8000, circle/memorymap.h). Au-delà, la
 * BSS recouvre la pile du cœur 0 : le noyau s'arrête avant l'écran (mire
 * multicolore figée). Écrit pour le port.
 */
ASSERT (_end <= 0x8000 + PORT_KERNEL_MAX_SIZE,
	"noyau trop gros : _end depasse MEM_KERNEL_END (KERNEL_MAX_SIZE), reduire la BSS")
