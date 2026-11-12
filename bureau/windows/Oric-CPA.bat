@echo off
rem CP/A directement (loci\cpa-en.dsk en A:, sans le menu ; F8 = menu) avec le modem
cd /d "%~dp0"
set PHOSPHORIC_PICOWIFI_NVRAM=%~dp0loci\picowifi.cfg
start "" oric1-emu.exe -r loci\locirom --loci --loci-flash loci --loci-usb usb --loci-disk 0:/cpa-en.dsk --serial picowifi:Maison --serial-buffer 256 --serial-rx-backpressure --serial-no-cadence -k fr --pixels tv --colors tv %*
