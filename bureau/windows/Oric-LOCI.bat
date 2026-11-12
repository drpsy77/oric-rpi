@echo off
rem Oric Atmos avec la cartouche LOCI : menu, flash 0: = loci\, cle 1: = usb\
cd /d "%~dp0"
set PHOSPHORIC_PICOWIFI_NVRAM=%~dp0loci\picowifi.cfg
start "" oric1-emu.exe -r loci\locirom --loci --loci-flash loci --loci-usb usb -k fr --pixels tv --colors tv %*
