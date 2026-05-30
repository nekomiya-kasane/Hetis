# coca-env.ps1 — Set up coca-toolchain-p2996 environment for the current shell.
# Source this script in PowerShell: . .\scripts\coca-env.ps1
# Or let VS Code / Visual Studio auto-source it via their terminal profile configs.

$COCA_ROOT = "T:\toolchains\coca-toolchain-p2996"

# ── PATH entries ──────────────────────────────────────────────────────────────
$pathDirs = @(
    "$COCA_ROOT\bin"
    "$COCA_ROOT\tools\ninja"
    "$COCA_ROOT\tools\cmake\bin"
    "$COCA_ROOT\tools\git\cmd"
    "$COCA_ROOT\tools\python"
    "$COCA_ROOT\tools\nodejs"
    "$COCA_ROOT\tools\coca"
    "$COCA_ROOT\tools\conan"
    "$COCA_ROOT\tools\jfrog"
    "$COCA_ROOT\tools\rust\cargo\bin"
    "$COCA_ROOT\tools\dotnet"
    "$COCA_ROOT\tools\perl\perl\bin"
    "$COCA_ROOT\tools\graphviz\bin"
    "$COCA_ROOT\tools\doxygen"
    "$COCA_ROOT\tools\renderdoc"
    "$COCA_ROOT\lib\clang\21\lib\windows"   # ASan runtime DLLs
)

foreach ($d in $pathDirs) {
    if ((Test-Path $d) -and ($env:PATH -notlike "*$d*")) {
        $env:PATH = "$d;$env:PATH"
    }
}

# ── Compiler / toolchain variables ────────────────────────────────────────────
$env:CC  = "$COCA_ROOT\bin\clang.exe"
$env:CXX = "$COCA_ROOT\bin\clang++.exe"
$env:CMAKE_GENERATOR = "Ninja"
$env:CMAKE_MAKE_PROGRAM = "$COCA_ROOT\tools\ninja\ninja.exe"

# Prevent MSVC headers from leaking into builds that use the sysroot.
$env:INCLUDE = ""
$env:LIB     = ""
$env:LIBPATH = ""

# ── Prompt decoration (optional) ─────────────────────────────────────────────
function global:prompt {
    Write-Host "[coca-p2996] " -NoNewline -ForegroundColor Cyan
    return "$(Get-Location)> "
}

Write-Host "[coca-p2996] Environment loaded: $COCA_ROOT" -ForegroundColor Green
