#
# Makefile — émulateur Oric (Phosphoric) pour Raspberry Pi, bare-metal Circle
#
# Repris de circle-stdlib samples/03-stdio-fatfs/Makefile ; ajouts : chemin de
# circle-stdlib, sources de Phosphoric, bibliothèques son et Wi-Fi de Circle,
# C11, relais réseau (--wrap). Une seule cible : kernel8-32.img.
#
# Prérequis : circle-stdlib cloné avec ses sous-modules, configuré et construit
#   git clone --recursive https://github.com/smuehlst/circle-stdlib.git
#   cd circle-stdlib && ./configure -r 3 -o ARM_ALLOW_MULTI_CORE && make
#   (Pi 3B+ / Zero 2 W ; l'émulation tourne sur le cœur 1)
# puis ici :  make   (ou make CIRCLESTDLIB=/chemin/vers/circle-stdlib)
#

CIRCLESTDLIB ?= /home/pi/Projets/circle-stdlib

include $(CIRCLESTDLIB)/Config.mk

# Sources de Phosphoric copiées par importer_phosphoric.sh (non modifiées)
PHOSPHORIC_SRC = $(sort $(wildcard phosphoric/src/*.c phosphoric/src/*/*.c))
PHOSPHORIC_OBJS = $(PHOSPHORIC_SRC:.c=.o)

OBJS	= main.o kernel.o oric_machine.o phosphoric_stubs.o newlib_compat.o journal.o netrelay.o disquette.o locidisk.o $(PHOSPHORIC_OBJS)

# Phosphoric utilise <stdatomic.h> (C11) ; Circle compile en gnu99 par défaut.
# circle-stdlib place les en-têtes C++ avant ceux de GCC : avec GCC >= 13, un
# fichier C trouve alors le <stdatomic.h> de libstdc++, vide en C. On force
# donc l'inclusion du <stdatomic.h> C de GCC (C_STANDARD ne sert qu'aux .c).
C_STANDARD = -std=gnu11 -include $(STDDEF_INCPATH)/stdatomic.h

EXTRACLEAN = $(PHOSPHORIC_OBJS) $(PHOSPHORIC_OBJS:.o=.d)

include $(CIRCLEHOME)/Rules.mk

INCLUDE := $(CIRCLE_STDLIB_INCLUDES) $(INCLUDE) -I . -I phosphoric/include -I compat

# newlib ne définit CLOCK_MONOTONIC que si _POSIX_MONOTONIC_CLOCK l'est ;
# circle-newlib le gère pourtant (clock_gettime.cpp). Pour serial_backend.c.
phosphoric/src/io/serial_backend.o: C_STANDARD += -D_POSIX_MONOTONIC_CLOCK=200112L

LIBS := $(CIRCLEHOME)/addon/wlan/hostap/wpa_supplicant/libwpa_supplicant.a \
	$(CIRCLEHOME)/addon/wlan/libwlan.a \
	$(CIRCLE_STDLIB_LIBS) $(CIRCLEHOME)/lib/sound/libsound.a

# Sockets et fermetures de fichiers appelés par l'émulation (cœur 1) : exécutés
# par le cœur 0, seul à pouvoir utiliser l'ordonnanceur de Circle (netrelay.cpp).
NETRELAY_WRAP = socket connect bind listen accept getsockname getsockopt \
		setsockopt recv send poll getaddrinfo _read _write _close _fcntl
LDFLAGS += $(foreach f,$(NETRELAY_WRAP),--wrap=$(f))

# ACIA sans cadence, comme le vrai LOCI (oric_machine.c, __wrap_acia_write)
LDFLAGS += --wrap=acia_write

-include $(DEPS)
