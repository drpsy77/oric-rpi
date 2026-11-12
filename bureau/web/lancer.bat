@echo off
rem Sert la page sur http://localhost:8000/ (le navigateur refuse le .wasm en file://)
cd /d "%~dp0"
start "" http://localhost:8000/
python -m http.server 8000
