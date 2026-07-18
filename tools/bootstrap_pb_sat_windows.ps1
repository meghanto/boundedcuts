<#
.SYNOPSIS
    Build the pinned BoundedCuts PB/SAT tools on native Windows.
.DESCRIPTION
    Clones and builds CaDiCaL and DRAT-trim with an MSYS2 MINGW64
    toolchain. The script installs missing MSYS2 build packages unless
    -SkipPackageInstall is supplied. It does not build or run BoundedCuts.
.PARAMETER Destination
    Directory that will contain the three pinned source and build trees.
.PARAMETER Msys2Root
    MSYS2 installation root. When omitted, common installation paths are tried.
.PARAMETER Jobs
    Maximum parallel build jobs.
.PARAMETER SkipPackageInstall
    Do not invoke pacman; fail later if make or the MinGW compiler is absent.
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [string]$Destination = (Join-Path $env:LOCALAPPDATA "BoundedCuts\pb-sat"),

    [string]$Msys2Root = "",

    [ValidateRange(1, 1024)]
    [int]$Jobs = [Math]::Max(1, [Environment]::ProcessorCount),

    [switch]$SkipPackageInstall
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$cadicalCommit = "f13d74439a5b5c963ac5b02d05ce93a8098018b8"
$dratTrimCommit = "2e3b2dc0ecf938addbd779d42877b6ed69d9a985"

function Invoke-Checked {
    param(
        [Parameter(Mandatory = $true)][string]$Executable,
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )
    & $Executable @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code ${LASTEXITCODE}: $Executable $($Arguments -join ' ')"
    }
}

function Sync-PinnedRepository {
    param(
        [Parameter(Mandatory = $true)][string]$Git,
        [Parameter(Mandatory = $true)][string]$Url,
        [Parameter(Mandatory = $true)][string]$Commit,
        [Parameter(Mandatory = $true)][string]$Directory
    )
    if (-not (Test-Path -LiteralPath (Join-Path $Directory ".git"))) {
        if (Test-Path -LiteralPath $Directory) {
            throw "Destination exists but is not a Git checkout: $Directory"
        }
        Invoke-Checked $Git @("clone", $Url, $Directory)
    }
    $dirty = & $Git -C $Directory status --porcelain --untracked-files=no
    if ($LASTEXITCODE -ne 0) { throw "Could not inspect checkout: $Directory" }
    if ($dirty) { throw "Refusing to overwrite modified checkout: $Directory" }
    Invoke-Checked $Git @("-C", $Directory, "fetch", "--quiet", "origin", $Commit)
    Invoke-Checked $Git @("-C", $Directory, "checkout", "--detach", "--quiet", $Commit)
    $actual = (& $Git -C $Directory rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0 -or $actual -ne $Commit) {
        throw "Pin verification failed for $Directory; expected $Commit, got $actual"
    }
}

$gitCandidates = @(
    (Join-Path $env:ProgramFiles "Git\bin\git.exe"),
    (Join-Path $env:ProgramFiles "Git\cmd\git.exe")
)
$git = $gitCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if ([string]::IsNullOrWhiteSpace($git)) {
    $resolvedGit = Get-Command "git.exe" -All -ErrorAction SilentlyContinue |
        Where-Object { $_.Source -notmatch '\\(scoop|shims)\\' } |
        Select-Object -First 1
    if ($null -ne $resolvedGit) { $git = $resolvedGit.Source }
}
if ([string]::IsNullOrWhiteSpace($git)) {
    throw "A real Git for Windows executable was not found; shim executables are not supported."
}

if ([string]::IsNullOrWhiteSpace($Msys2Root)) {
    $candidates = @("C:\msys64", "C:\tools\msys64")
    $Msys2Root = $candidates |
        Where-Object { Test-Path -LiteralPath (Join-Path $_ "msys2_shell.cmd") } |
        Select-Object -First 1
}
if ([string]::IsNullOrWhiteSpace($Msys2Root)) {
    throw "MSYS2 was not found. Install it from https://www.msys2.org/ or pass -Msys2Root."
}
$Msys2Root = (Resolve-Path -LiteralPath $Msys2Root).Path
$msysBash = Join-Path $Msys2Root "usr\bin\bash.exe"
$pacman = Join-Path $Msys2Root "usr\bin\pacman.exe"
if (-not (Test-Path -LiteralPath $msysBash) -or -not (Test-Path -LiteralPath $pacman)) {
    throw "Invalid MSYS2 installation root: $Msys2Root"
}

if (-not $SkipPackageInstall) {
    $pacmanLock = Join-Path $Msys2Root "var\lib\pacman\db.lck"
    if (Test-Path -LiteralPath $pacmanLock) {
        $activePacman = Get-Process -Name "pacman" -ErrorAction SilentlyContinue
        if ($activePacman) {
            throw "MSYS2 package manager is already running; retry after it exits."
        }
        Remove-Item -LiteralPath $pacmanLock -Force
    }
    Invoke-Checked $pacman @(
        "-S", "--needed", "--noconfirm", "make", "mingw-w64-x86_64-gcc"
    )
}

New-Item -ItemType Directory -Force -Path $Destination | Out-Null
$Destination = (Resolve-Path -LiteralPath $Destination).Path

$cadicalRoot = Join-Path $Destination "cadical"
$dratTrimRoot = Join-Path $Destination "drat-trim"

Sync-PinnedRepository $git "https://github.com/arminbiere/cadical.git" $cadicalCommit $cadicalRoot
Sync-PinnedRepository $git "https://github.com/marijnheule/drat-trim.git" $dratTrimCommit $dratTrimRoot

$temporaryName = "boundedcuts-pb-$([Guid]::NewGuid().ToString('N')).sh"
$temporaryWindowsPath = Join-Path (Join-Path $Msys2Root "tmp") $temporaryName
$temporaryMsysPath = "/tmp/$temporaryName"
$buildScript = @'
#!/usr/bin/env bash
set -euo pipefail
export PATH=/mingw64/bin:/usr/bin:$PATH
root="$(cygpath -u "$BOUNDEDCUTS_PB_DESTINATION")"
git_directory="$(cygpath -u "$BOUNDEDCUTS_PB_GIT_DIRECTORY")"
export PATH="$git_directory:$PATH"
jobs="$BOUNDEDCUTS_PB_JOBS"

cd "$root/cadical"
cadical_rebuild=0
if [[ ! -x build/cadical.exe || ! -f build/libcadical.a ]]; then
    cadical_rebuild=1
elif ldd build/cadical.exe | grep -Eq 'lib(gcc|stdc\+\+|winpthread)'; then
    cadical_rebuild=1
fi
if [[ "$cadical_rebuild" == 1 ]]; then
    rm -rf build
    ./configure -static
    make -j"$jobs" cadical
fi

cd "$root/drat-trim"
if [[ ! -x drat-trim.exe ]]; then
    gcc drat-trim.c -std=c99 -O2 -Dgetc_unlocked=getc -o drat-trim.exe
fi
'@
Set-Content -LiteralPath $temporaryWindowsPath -Value $buildScript -Encoding Ascii

$oldDestination = $env:BOUNDEDCUTS_PB_DESTINATION
$oldGitDirectory = $env:BOUNDEDCUTS_PB_GIT_DIRECTORY
$oldJobs = $env:BOUNDEDCUTS_PB_JOBS
$oldMsystem = $env:MSYSTEM
$oldChereInvoking = $env:CHERE_INVOKING
try {
    $env:BOUNDEDCUTS_PB_DESTINATION = $Destination
    $env:BOUNDEDCUTS_PB_GIT_DIRECTORY = Split-Path -Parent $git
    $env:BOUNDEDCUTS_PB_JOBS = $Jobs.ToString()
    $env:MSYSTEM = "MINGW64"
    $env:CHERE_INVOKING = "1"
    Invoke-Checked $msysBash @($temporaryMsysPath)
}
finally {
    $env:BOUNDEDCUTS_PB_DESTINATION = $oldDestination
    $env:BOUNDEDCUTS_PB_GIT_DIRECTORY = $oldGitDirectory
    $env:BOUNDEDCUTS_PB_JOBS = $oldJobs
    $env:MSYSTEM = $oldMsystem
    $env:CHERE_INVOKING = $oldChereInvoking
    Remove-Item -LiteralPath $temporaryWindowsPath -Force -ErrorAction SilentlyContinue
}

$cadical = Join-Path $cadicalRoot "build\cadical.exe"
$dratTrim = Join-Path $dratTrimRoot "drat-trim.exe"
foreach ($artifact in @($cadical, $dratTrim)) {
    if (-not (Test-Path -LiteralPath $artifact)) { throw "Expected build artifact is missing: $artifact" }
}

$cadicalVersionOutput = & $cadical --version 2>&1
$cadicalExitCode = $LASTEXITCODE
$cadicalVersion = ($cadicalVersionOutput -join "`n").Trim()
if ($cadicalExitCode -ne 0 -or $cadicalVersion -ne "2.1.3") {
    throw "Unexpected CaDiCaL version: $cadicalVersion"
}

$selfTestDirectory = Join-Path ([System.IO.Path]::GetTempPath()) "boundedcuts-pb-$([Guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Force -Path $selfTestDirectory | Out-Null
try {
    $cnf = Join-Path $selfTestDirectory "contradiction.cnf"
    $proof = Join-Path $selfTestDirectory "contradiction.drat"
    Set-Content -LiteralPath $cnf -Encoding Ascii -Value @(
        "p cnf 1 2", "1 0", "-1 0"
    )
    $null = & $cadical $cnf $proof 2>&1
    if ($LASTEXITCODE -ne 20 -or -not (Test-Path -LiteralPath $proof)) {
        throw "CaDiCaL did not produce the expected UNSAT proof in the self-test."
    }
    $checkerOutput = & $dratTrim $cnf $proof 2>&1
    $checkerExitCode = $LASTEXITCODE
    if ($checkerExitCode -ne 0 -or ($checkerOutput -join "`n") -notmatch "VERIFIED") {
        throw "DRAT-trim did not verify the CaDiCaL self-test proof."
    }
}
finally {
    Remove-Item -LiteralPath $selfTestDirectory -Recurse -Force -ErrorAction SilentlyContinue
}

[pscustomobject]@{
    CaDiCaL = $cadical
    DratTrim = $dratTrim
    CaDiCaLCommit = $cadicalCommit
    DratTrimCommit = $dratTrimCommit
} | Format-List
