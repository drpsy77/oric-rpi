#
# Makefile — émulateur Oric (Phosphoric) pour Raspberry Pi, bare-metal Circle
#
# Repris de circle-stdlib samples/03-stdio-fatfs/Makefile ; ajouts : chemin de
# circle-stdlib, sources de Phosphoric, bibliothèque son de Circle, C11.
#
# Prérequis : circle-stdlib cloné avec ses sous-modules, configuré et construit
#   git clone --recursive https://github.com/smuehlst/circle-stdlib.git
#   cd circle-stdlib && ./configure -r 1 && make      (Pi Zero / Zero W)
# puis ici :  make CIRCLESTDLIB=/chemin/vers/circle-stdlib
#

CIRCLESTDLIB ?= /home/pi/Projets/circle-stdlib

include $(CIRCLESTDLIB)/Config.mk

# Sources de Phosphoric copiées par importer_phosphoric.sh (non modifiées)
PHOSPHORIC_SRC = $(sort $(wildcard phosphoric/src/*.c phosphoric/src/*/*.c))
PHOSPHORIC_OBJS = $(PHOSPHORIC_SRC:.c=.o)

OBJS	= main.o kernel.o oric_machine.o phosphoric_stubs.o newlib_compat.o $(PHOSPHORIC_OBJS)

# Phosphoric utilise <stdatomic.h> (C11) ; Circle compile en gnu99 par défaut.
# circle-stdlib place les en-têtes C++ avant ceux de GCC : avec GCC >= 13, un
# fichier C trouve alors le <stdatomic.h> de libstdc++, vide en C. On force
# donc l'inclusion du <stdatomic.h> C de GCC (C_STANDARD ne sert qu'aux .c).
C_STANDARD = -std=gnu11 -include $(STDDEF_INCPATH)/stdatomic.h

EXTRACLEAN = $(PHOSPHORIC_OBJS) $(PHOSPHORIC_OBJS:.o=.d)

include $(CIRCLEHOME)/Rules.mk

INCLUDE := $(CIRCLE_STDLIB_INCLUDES) $(INCLUDE) -I . -I phosphoric/include

LIBS := $(CIRCLE_STDLIB_LIBS) $(CIRCLEHOME)/lib/sound/libsound.a

-include $(DEPS)
