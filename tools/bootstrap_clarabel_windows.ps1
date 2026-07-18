<#
.SYNOPSIS
    Idempotent PowerShell bootstrap script to build a correct Windows DLL for Clarabel with SDP support.
.DESCRIPTION
    Builds the Clarabel.cpp rust_wrapper as a cdylib on Windows, linking statically to OpenBLAS,
    and avoiding upstream archive overflow issue.
.PARAMETER ClarabelSourceRoot
    Path to the canonical Clarabel source root.
.PARAMETER OpenBlasLibPath
    Path to Conan OpenBLAS library file (openblas.lib) or the directory containing it.
.PARAMETER BuildTempRoot
    Optional temporary build directory root. Defaults to C:\Temp\clarabel_build.
#>
param(
    [Parameter(Position = 0, Mandatory = $true)]
    [string]$ClarabelSourceRoot,

    [Parameter(Position = 1, Mandatory = $true)]
    [string]$OpenBlasLibPath,

    [Parameter(Position = 2, Mandatory = $false)]
    [string]$BuildTempRoot = "C:\Temp\clarabel_build"
)

$ErrorActionPreference = "Stop"

# Helper to log messages
function Write-Log($msg) {
    Write-Host "[Clarabel Bootstrap] $msg" -ForegroundColor Cyan
}

function Write-Err($msg) {
    Write-Error "[Clarabel Bootstrap] ERROR: $msg"
}

# Helper to invoke native commands robustly and fail-fast
function Invoke-NativeCommand {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Executable,
        [Parameter(Mandatory = $true)]
        [string[]]$ArgumentList,
        [Parameter(Mandatory = $false)]
        [bool]$RequireStdout = $false
    )

    $global:LASTEXITCODE = 0
    $output = $null
    try {
        if ($RequireStdout) {
            $output = & $Executable $ArgumentList
        } else {
            & $Executable $ArgumentList
        }
    }
    catch {
        throw "Failed to start or run native command '$Executable' with arguments '$($ArgumentList -join ' ')'. Error: $_"
    }

    $exitCode = $LASTEXITCODE
    if ($null -ne $exitCode -and $exitCode -ne 0) {
        throw "Native command failed: '$Executable' with exit code $exitCode. Arguments: $($ArgumentList -join ' ')"
    }

    if ($RequireStdout) {
        if ($null -eq $output -or ($output -is [string] -and [string]::IsNullOrWhiteSpace($output)) -or ($output -is [array] -and $output.Length -eq 0)) {
            throw "Required stdout was empty for command: '$Executable'. Arguments: $($ArgumentList -join ' ')"
        }

        # Trim and return
        if ($output -is [array]) {
            return ($output -join "`n").Trim()
        } else {
            return $output.ToString().Trim()
        }
    }

    return $null
}

# 1. Verify environment and tools
Write-Log "Checking tools and paths..."

# Resolve Git executable path
$gitPath = ""
$pfGit = "C:\Program Files\Git\bin\git.exe"
if (Test-Path $pfGit) {
    $gitPath = $pfGit
} else {
    $pfGitEnv = Join-Path $env:ProgramFiles "Git\bin\git.exe"
    if (Test-Path $pfGitEnv) {
        $gitPath = $pfGitEnv
    }
}

if ($gitPath -eq "") {
    $gitCommands = Get-Command git -All -ErrorAction SilentlyContinue
    foreach ($cmd in $gitCommands) {
        $path = $cmd.Source
        if ($null -eq $path) { $path = $cmd.Definition }
        if ($path -and $path -notmatch '\\scoop\\shims\\' -and $path -notmatch '\\shims\\' -and $path -notmatch '\\cmd\\git(\.exe)?$') {
            $gitPath = $path
            break
        }
    }
}

if ($gitPath -eq "") {
    Write-Err "Could not resolve a usable real Git executable (no Program Files Git bin or non-shim Git in PATH)."
    exit 1
}
Write-Log "Resolved Git to: $gitPath"

# Resolve Cargo executable path
$cargoPath = ""
$cargoDefault = Join-Path $env:USERPROFILE ".cargo\bin\cargo.exe"
if (Test-Path $cargoDefault) {
    $cargoPath = $cargoDefault
} else {
    $cargoCommands = Get-Command cargo -All -ErrorAction SilentlyContinue
    foreach ($cmd in $cargoCommands) {
        $path = $cmd.Source
        if ($null -eq $path) { $path = $cmd.Definition }
        if ($path -and $path -notmatch '\\scoop\\shims\\' -and $path -notmatch '\\shims\\') {
            $cargoPath = $path
            break
        }
    }
}

if ($cargoPath -eq "") {
    Write-Err "Could not resolve a usable cargo executable (no default %USERPROFILE%\.cargo\bin\cargo.exe or non-shim cargo in PATH)."
    exit 1
}
Write-Log "Resolved Cargo to: $cargoPath"

# Resolve ClarabelSourceRoot absolute path
if (!(Test-Path $ClarabelSourceRoot)) {
    Write-Err "Clarabel source root does not exist: $ClarabelSourceRoot"
    exit 1
}
$canonicalClarabelRoot = (Get-Item $ClarabelSourceRoot).FullName

# Resolve OpenBLAS path
if (!(Test-Path $OpenBlasLibPath)) {
    Write-Err "OpenBLAS path does not exist: $OpenBlasLibPath"
    exit 1
}
$resolvedOpenBlas = Get-Item $OpenBlasLibPath
$openBlasLibDir = ""
if ($resolvedOpenBlas.PSIsContainer) {
    $openBlasLibDir = $resolvedOpenBlas.FullName
    $libFile = Join-Path $openBlasLibDir "openblas.lib"
    if (!(Test-Path $libFile)) {
        Write-Err "Could not find openblas.lib in directory: $openBlasLibDir"
        exit 1
    }
} else {
    $openBlasLibDir = $resolvedOpenBlas.DirectoryName
    if ($resolvedOpenBlas.Name -ne "openblas.lib") {
        Write-Log "Warning: OpenBLAS library file name is '$($resolvedOpenBlas.Name)', expected 'openblas.lib'. Linking may fail if not named openblas.lib."
    }
}

# 2. Verify Pins
Write-Log "Verifying Clarabel Git pins..."
$expectedCppPin = "0de6259a3edfd5cc041ec42b2148599ce63e73cb"
$expectedRsPin = "25540f559592068d0c8a80e46ded1b21760212a1"

# Check Clarabel.cpp pin
$cppPin = Invoke-NativeCommand -Executable $gitPath -ArgumentList @("-C", $canonicalClarabelRoot, "rev-parse", "HEAD") -RequireStdout $true
if ($cppPin -ne $expectedCppPin) {
    Write-Err "Clarabel.cpp pin mismatch. Expected: $expectedCppPin, Got: $cppPin"
    exit 1
}

# Check Clarabel.rs pin
$rsPath = Join-Path $canonicalClarabelRoot "Clarabel.rs"
if (!(Test-Path $rsPath)) {
    Write-Err "Clarabel.rs submodule directory not found at $rsPath"
    exit 1
}
$rsPin = Invoke-NativeCommand -Executable $gitPath -ArgumentList @("-C", $rsPath, "rev-parse", "HEAD") -RequireStdout $true
if ($rsPin -ne $expectedRsPin) {
    Write-Err "Clarabel.rs submodule pin mismatch. Expected: $expectedRsPin, Got: $rsPin"
    exit 1
}
Write-Log "Git pins verified successfully."

# 3. Create disposable C: build tree and copy sources
Write-Log "Preparing disposable build tree..."
# Ensure BuildTempRoot exists on C:
# Check if BuildTempRoot starts with a drive letter, if not default to C:\Temp\clarabel_build
if ($BuildTempRoot -notmatch '^[a-zA-Z]:\\') {
    $BuildTempRoot = "C:\Temp\clarabel_build"
} else {
    # Ensure it's C: drive
    if ($BuildTempRoot -notmatch '^[cC]:\\') {
        Write-Log "Build temp root '$BuildTempRoot' is not on C: drive. Defaulting to C:\Temp\clarabel_build."
        $BuildTempRoot = "C:\Temp\clarabel_build"
    }
}

if (!(Test-Path $BuildTempRoot)) {
    New-Item -ItemType Directory -Path $BuildTempRoot -Force | Out-Null
}

$buildId = [Guid]::NewGuid().ToString("N").Substring(0, 8)
$disposableCopy = Join-Path $BuildTempRoot "src_$buildId"
New-Item -ItemType Directory -Path $disposableCopy -Force | Out-Null

Write-Log "Copying source files to disposable tree (excluding .git, target, and build directories)..."
function Copy-SourceTree {
    param([string]$src, [string]$dst)
    if (!(Test-Path $dst)) { New-Item -ItemType Directory -Path $dst -Force | Out-Null }
    Get-ChildItem -Path $src -Force | ForEach-Object {
        $name = $_.Name
        if ($name -eq ".git" -or $name -eq "target" -or $name -eq "build") {
            return
        }
        $targetPath = Join-Path $dst $name
        if ($_.PSIsContainer) {
            Copy-SourceTree $_.FullName $targetPath
        } else {
            Copy-Item $_.FullName $targetPath -Force
        }
    }
}

Copy-SourceTree $canonicalClarabelRoot $disposableCopy
Write-Log "Source copy complete at $disposableCopy."

# 4. Modify crate-type in Cargo.toml
Write-Log "Modifying crate-type in Cargo.toml..."
$cargoTomlPath = Join-Path $disposableCopy "rust_wrapper\Cargo.toml"
if (!(Test-Path $cargoTomlPath)) {
    Write-Err "Could not find Cargo.toml in disposable copy at $cargoTomlPath"
    exit 1
}

$content = Get-Content $cargoTomlPath -Raw
# Replace ["cdylib", "staticlib"] with ["cdylib"]
$pattern1 = 'crate-type\s*=\s*\[\s*["'']cdylib["'']\s*,\s*["'']staticlib["'']\s*\]'
$pattern2 = 'crate-type\s*=\s*\[\s*["'']staticlib["'']\s*,\s*["'']cdylib["'']\s*\]'

if ($content -match $pattern1) {
    $content = $content -replace $pattern1, 'crate-type = ["cdylib"]'
} elseif ($content -match $pattern2) {
    $content = $content -replace $pattern2, 'crate-type = ["cdylib"]'
} else {
    # General replacement if whitespace or quotes vary
    $content = $content -replace '\[\s*["'']cdylib["'']\s*,\s*["'']staticlib["'']\s*\]', '["cdylib"]'
    $content = $content -replace '\[\s*["'']staticlib["'']\s*,\s*["'']cdylib["'']\s*\]', '["cdylib"]'
}
Set-Content $cargoTomlPath $content -NoNewline
Write-Log "Cargo.toml updated."

# 5. Invoke Cargo build with correct flags and environment variables
Write-Log "Running cargo build..."
$oldRustFlags = $env:RUSTFLAGS
$oldCargoHome = $env:CARGO_HOME
$pushed = $false
$buildFailed = $false
$failureMessage = ""

try {
    # Set environment variables
    $env:RUSTFLAGS = "-L native=$openBlasLibDir -l static=openblas"
    # Ensure cargo cache is also on C: to avoid R: disk issues if cargo home is default
    if ($env:CARGO_HOME -match '^[rR]:') {
        $env:CARGO_HOME = "C:\Temp\.cargo_cache"
        Write-Log "Overriding CARGO_HOME to C:\Temp\.cargo_cache because it was set to R:"
    }

    Push-Location (Join-Path $disposableCopy "rust_wrapper")
    $pushed = $true

    # Run cargo
    Invoke-NativeCommand -Executable $cargoPath -ArgumentList @("build", "--release", "--features", "clarabel/sdp,clarabel/blas-src,clarabel/lapack-src,sdp")
}
catch {
    $buildFailed = $true
    $failureMessage = $_.ToString()
}
finally {
    # Ensure Push-Location is always balanced even when Cargo fails
    if ($pushed) {
        Pop-Location
    }
    # Restore environment variables
    if ($oldRustFlags -ne $null) {
        $env:RUSTFLAGS = $oldRustFlags
    } else {
        Remove-Item env:RUSTFLAGS -ErrorAction SilentlyContinue
    }
    if ($oldCargoHome -ne $null) {
        $env:CARGO_HOME = $oldCargoHome
    } else {
        Remove-Item env:CARGO_HOME -ErrorAction SilentlyContinue
    }
}

if ($buildFailed) {
    Write-Err "Cargo build failed: $failureMessage"
    exit 1
}

# 6. Locate and copy build artifacts
Write-Log "Verifying build artifacts..."
$disposableTargetRelease = Join-Path $disposableCopy "rust_wrapper\target\release"
if (!(Test-Path $disposableTargetRelease)) {
    # fallback in case workspace-level target was used
    $disposableTargetRelease = Join-Path $disposableCopy "target\release"
}

$dllFile = Join-Path $disposableTargetRelease "clarabel_c.dll"
$libFile = Join-Path $disposableTargetRelease "clarabel_c.dll.lib"
$pdbFile = Join-Path $disposableTargetRelease "clarabel_c.pdb"

if (!(Test-Path $dllFile) -or !(Test-Path $libFile) -or !(Test-Path $pdbFile)) {
    Write-Err "Missing expected cdylib build artifacts in $disposableTargetRelease. DLL, LIB, or PDB not found."
    exit 1
}

$canonicalTargetRelease = Join-Path $canonicalClarabelRoot "rust_wrapper\target\release"
if (!(Test-Path $canonicalTargetRelease)) {
    New-Item -ItemType Directory -Path $canonicalTargetRelease -Force | Out-Null
}

Write-Log "Copying artifacts to canonical destination: $canonicalTargetRelease"
Copy-Item $dllFile $canonicalTargetRelease -Force
Copy-Item $libFile $canonicalTargetRelease -Force
Copy-Item $pdbFile $canonicalTargetRelease -Force

# 7. Write marker / manifest file
$manifestPath = Join-Path $canonicalTargetRelease "clarabel_sdp_manifest.txt"
$manifestContent = @"
CLARABEL_CPP_COMMIT=$expectedCppPin
CLARABEL_RS_COMMIT=$expectedRsPin
SDP=true
OPENBLAS=true
CARGO_FEATURES=clarabel/sdp,clarabel/blas-src,clarabel/lapack-src,sdp
"@
Set-Content -Path $manifestPath -Value $manifestContent -NoNewline
Write-Log "Wrote marker manifest to $manifestPath"

# Clean up disposable tree
Write-Log "Cleaning up temporary build tree..."
Remove-Item -Path $disposableCopy -Recurse -Force -ErrorAction SilentlyContinue

Write-Log "Clarabel SDP dependency build completed successfully!"
