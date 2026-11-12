@echo off
rem Oric Atmos, BASIC 1.1 (ajouter --disk-rom loci\microdis.rom -d usb\x.dsk pour le Microdisc)
cd /d "%~dp0"
set PHOSPHORIC_PICOWIFI_NVRAM=%~dp0loci\picowifi.cfg
start "" oric1-emu.exe -r loci\basic11b.rom -k fr --pixels tv --colors tv %*
