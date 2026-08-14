# Optional: build a single-file Windows .exe of the RiptOPL SMBv1 server.
#
# Power users should just run `python smbserver_opl.py` -- it's pure stdlib, zero deps, and a bare
# .py has no antivirus false-positives. This script is only for shipping a double-clickable binary.
#
# We use Nuitka (compiles to C) rather than PyInstaller: PyInstaller's self-extracting bootloader
# is heuristically flagged by Windows Defender / SmartScreen far more often. The single biggest
# false-positive reducer is CODE SIGNING the resulting .exe with an OV/EV certificate (signtool).
#
# Requires: a C compiler (MSVC build tools or MinGW) + `pip install nuitka`.
#
# Usage (from this folder, in PowerShell):
#   .\build_exe.ps1

$ErrorActionPreference = 'Stop'
python -m pip install --upgrade nuitka | Out-Null

python -m nuitka `
    --onefile `
    --standalone `
    --assume-yes-for-downloads `
    --output-filename=riptopl-smbserver.exe `
    --company-name=RiptOPL `
    --product-name="RiptOPL SMBv1 Server" `
    --file-description="Minimal SMBv1 server for Open-PS2-Loader" `
    smbserver_opl.py

Write-Host ""
Write-Host "Built riptopl-smbserver.exe" -ForegroundColor Green
Write-Host "STRONGLY recommended: sign it to avoid antivirus false-positives, e.g." -ForegroundColor Yellow
Write-Host '  signtool sign /fd SHA256 /a /tr http://timestamp.digicert.com /td SHA256 riptopl-smbserver.exe'
