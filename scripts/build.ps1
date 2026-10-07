$ErrorActionPreference = 'Stop'
Set-Location (Split-Path $PSScriptRoot -Parent)
cmake -S . -B build -A x64
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed. Install C++ Desktop Build Tools and Windows SDK.' }
cmake --build build --config Release --parallel
if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
ctest --test-dir build -C Release --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Core tests failed.' }
$target = Join-Path (Get-Location) 'dist/Tiaoyin-Windows-x64'
New-Item -ItemType Directory -Path $target -Force | Out-Null
Copy-Item 'build/Release/Tiaoyin.exe','build/Release/tiaoyin_tests.exe','README.md' $target -Force
Copy-Item docs $target -Recurse -Force
Get-FileHash "$target/Tiaoyin.exe" -Algorithm SHA256 | Format-List | Out-File "$target/SHA256.txt" -Encoding utf8
Write-Host "Portable build: $target"
