@echo off
rem LOCI + modem Wi-Fi (picowifi) : ACIA sans cadence, file de 256, contre-pression ; la cle devient 2:
cd /d "%~dp0"
set PHOSPHORIC_PICOWIFI_NVRAM=%~dp0loci\picowifi.cfg
start "" oric1-emu.exe -r loci\locirom --loci --loci-flash loci --loci-usb usb --serial picowifi:Maison --serial-buffer 256 --serial-rx-backpressure --serial-no-cadence -k fr --pixels tv --colors tv %*
