<#
.SYNOPSIS
    Fetch the pinned BoundedCuts PB/SAT source dependencies on Windows.
.DESCRIPTION
    Clones and verifies the exact CaDiCaL and DRAT-trim revisions consumed by
    the CMake build. It does not invoke MSYS2 or MinGW and does not build or run
    BoundedCuts. The normal CMake build compiles the embedded backend with the
    active MSVC toolchain.
.PARAMETER Destination
    Directory that will contain the pinned source trees.
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [string]$Destination = (Join-Path $env:LOCALAPPDATA "BoundedCuts\pb-sat")
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
        Invoke-Checked $Git @("clone", "--filter=blob:none", $Url, $Directory)
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

$gitCommand = Get-Command git.exe -ErrorAction Stop
$git = $gitCommand.Source
$root = [IO.Path]::GetFullPath($Destination)
New-Item -ItemType Directory -Force -Path $root | Out-Null

$cadicalRoot = Join-Path $root "cadical"
$dratTrimRoot = Join-Path $root "drat-trim"
Sync-PinnedRepository $git "https://github.com/arminbiere/cadical.git" $cadicalCommit $cadicalRoot
Sync-PinnedRepository $git "https://github.com/marijnheule/drat-trim.git" $dratTrimCommit $dratTrimRoot

[pscustomobject]@{
    CaDiCaLSource = $cadicalRoot
    DratTrimSource = $dratTrimRoot
    CaDiCaLCommit = $cadicalCommit
    DratTrimCommit = $dratTrimCommit
    Toolchain = "MSVC through the consuming CMake build"
}
