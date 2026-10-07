# build-installer.ps1 - compile the single-file JACoop update installer.
# Embeds the engine exe, both dlls and zz_jacoop.pk3 as resources.
#
# Defaults assume a CMake Release build in <repo>/build and the pk3 at
# <repo>/build/zz_jacoop.pk3. CI calls this with -Out dist/JACoop-Setup.exe.
param(
    [string]$FilesDir = "$PSScriptRoot\..\..\build\Release",
    [string]$Pk3      = "$PSScriptRoot\..\..\build\zz_jacoop.pk3",
    [string]$Out      = "$PSScriptRoot\..\..\JACoop-Setup.exe"
)

$ErrorActionPreference = "Stop"

$csc = "C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe"
if (-not (Test-Path $csc)) { $csc = "C:\Windows\Microsoft.NET\Framework\v4.0.30319\csc.exe" }
if (-not (Test-Path $csc)) { throw "csc.exe (.NET Framework 4.x) not found" }

foreach ($f in @("$FilesDir\openjk_sp.x86_64.exe", "$FilesDir\jagamex86_64.dll",
                 "$FilesDir\rdsp-vanilla_x86_64.dll", $Pk3)) {
    if (-not (Test-Path $f)) { throw "payload missing: $f" }
}

# NOTE: write each -res argument by concatenation. -res:"$path",name passes
# the quote characters to csc literally (native arg parsing quirk).
& $csc -nologo -target:winexe -platform:anycpu -out:$Out `
    -r:System.Windows.Forms.dll -r:System.Drawing.dll `
    "$PSScriptRoot\Installer.cs" `
    ('-res:' + "$FilesDir\openjk_sp.x86_64.exe"    + ',openjk_sp.x86_64.exe') `
    ('-res:' + "$FilesDir\jagamex86_64.dll"       + ',jagamex86_64.dll') `
    ('-res:' + "$FilesDir\rdsp-vanilla_x86_64.dll"+ ',rdsp-vanilla_x86_64.dll') `
    ('-res:' + "$Pk3"                              + ',zz_jacoop.pk3')
if ($LASTEXITCODE -ne 0) { throw "csc failed ($LASTEXITCODE)" }
Write-Host "built $Out"
