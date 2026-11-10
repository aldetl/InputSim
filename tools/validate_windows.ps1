param([switch]$BuildOnly, [switch]$ReceiverOnly)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
Push-Location -LiteralPath $projectRoot
try {
    cargo fmt --all --check
    if ($LASTEXITCODE -ne 0) { throw 'Rust formatting check failed' }
    cargo test --locked --offline
    if ($LASTEXITCODE -ne 0) { throw 'Rust tests failed' }
    cargo clippy --locked --offline --all-targets -- -D warnings
    if ($LASTEXITCODE -ne 0) { throw 'Rust lint check failed' }
    cargo build --release --locked --offline
    if ($LASTEXITCODE -ne 0) { throw 'Rust release build failed' }

    $locator = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $visualStudio = & $locator -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $visualStudio) { throw 'Visual C++ x64 tools unavailable' }
    $toolset = Get-ChildItem -LiteralPath (Join-Path $visualStudio 'VC/Tools/MSVC') -Directory |
        Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
    $sdkRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits/10'
    $sdk = Get-ChildItem -LiteralPath (Join-Path $sdkRoot 'Include') -Directory |
        Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'um/Windows.h') } |
        Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
    if (-not $toolset -or -not $sdk) { throw 'MSVC toolset or Windows SDK unavailable' }
    $compiler = Join-Path $toolset.FullName 'bin/Hostx64/x64/cl.exe'
    $includes = @("/I$($toolset.FullName)/include",
        "/I$($sdk.FullName)/ucrt", "/I$($sdk.FullName)/shared", "/I$($sdk.FullName)/um")
    $libraries = @("/LIBPATH:$($toolset.FullName)/lib/x64",
        "/LIBPATH:$sdkRoot/Lib/$($sdk.Name)/ucrt/x64",
        "/LIBPATH:$sdkRoot/Lib/$($sdk.Name)/um/x64")

    & $compiler /nologo /W4 /WX /TC /std:c11 /MT /X @includes tools/windows_validate.c /Fotarget/windows_validate.obj /Fetarget/windows_validate.exe /link @libraries user32.lib
    if ($LASTEXITCODE -ne 0) { throw 'C validation client build failed' }
    $headerCheck = @'
#include "../include/inputsim.h"
static_assert(sizeof(inputsim_event_v1) == 16, "event size");
static_assert(alignof(inputsim_event_v1) == 4, "event alignment");
static_assert(sizeof(inputsim_character_v1) == 68, "character size");
static_assert(alignof(inputsim_character_v1) == 4, "character alignment");
static_assert(sizeof(uintptr_t) == 8, "tag size");
'@
    Set-Content -LiteralPath 'target/header_check.cpp' -Value $headerCheck -Encoding utf8
    & $compiler /nologo /Zs /W4 /WX /TP /std:c++17 /X @includes target/header_check.cpp
    if ($LASTEXITCODE -ne 0) { throw 'C++ header check failed' }
    & ./target/windows_validate.exe --abi ./target/release/inputsim.dll
    if ($LASTEXITCODE -ne 0) { throw 'DLL ABI checks failed' }
    if (-not $BuildOnly) {
        if ($ReceiverOnly) {
            & ./target/windows_validate.exe --receiver ./target/release/inputsim.dll ./target/windows-validation.log
        } else {
            & ./target/windows_validate.exe ./target/release/inputsim.dll ./target/windows-validation.log
        }
        if ($LASTEXITCODE -ne 0) { throw 'Live Windows validation failed. See target/windows-validation.log' }
        if ($ReceiverOnly) { Get-Content -LiteralPath './target/windows-validation.log' }
    }
} finally {
    Pop-Location
}
