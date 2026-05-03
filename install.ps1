<#
.SYNOPSIS
    Download + install ambil on Windows.

.DESCRIPTION
    Downloads the prebuilt release archive matching the host architecture
    from GitHub Releases (or a custom base URL), verifies its SHA-256
    checksum, extracts ambil.exe, installs it to a per-user location, and
    optionally updates the PATH environment variable.

.PARAMETER Version
    Release tag to install. "latest" resolves via the GitHub API.
    Default: latest.

.PARAMETER InstallDir
    Destination directory. Default: $env:LOCALAPPDATA\Programs\ambil.

.PARAMETER Repo
    GitHub repository in owner/name form. Default: sutantodadang/ambil
    (override with $env:AMBIL_REPO).

.PARAMETER BaseUrl
    Alternate asset base URL. Default: GitHub Releases for the chosen tag.

.PARAMETER NoVerify
    Skip SHA-256 verification (not recommended).

.PARAMETER NoPath
    Do not modify the user PATH environment variable.

.PARAMETER Force
    Overwrite an existing installation without prompting.

.EXAMPLE
    iwr -useb https://example.com/install.ps1 | iex

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File install.ps1 -Version v0.2.0

.EXAMPLE
    .\install.ps1 -InstallDir 'C:\tools\ambil' -Force
#>
[CmdletBinding()]
param(
    [string] $Version     = $(if ($env:AMBIL_VERSION)     { $env:AMBIL_VERSION }     else { 'latest' }),
    [string] $InstallDir  = $(if ($env:AMBIL_INSTALL_DIR) { $env:AMBIL_INSTALL_DIR } else { Join-Path $env:LOCALAPPDATA 'Programs\ambil' }),
    [string] $Repo        = $(if ($env:AMBIL_REPO)        { $env:AMBIL_REPO }        else { 'sutantodadang/ambil' }),
    [string] $BaseUrl     = $env:AMBIL_BASE_URL,
    [switch] $NoVerify,
    [switch] $NoPath,
    [switch] $Force
)

$ErrorActionPreference = 'Stop'

# --- helpers ----------------------------------------------------------------

function Write-Step($msg) { Write-Host "==> $msg" -ForegroundColor Cyan }
function Write-Warn($msg) { Write-Host "warn: $msg" -ForegroundColor Yellow }
function Die($msg)        { Write-Host "error: $msg" -ForegroundColor Red; exit 1 }

# Force TLS 1.2+ for older PowerShell (5.x on Windows Server / Win10 LTS).
try {
    [Net.ServicePointManager]::SecurityProtocol = `
        [Net.ServicePointManager]::SecurityProtocol -bor `
        [Net.SecurityProtocolType]::Tls12
} catch {}

function Invoke-Download {
    param([string] $Url, [string] $OutFile)
    Write-Verbose "GET $Url"
    try {
        Invoke-WebRequest -Uri $Url -OutFile $OutFile -UseBasicParsing -ErrorAction Stop |
            Out-Null
    } catch {
        Die "download failed: $Url`n$($_.Exception.Message)"
    }
}

function Resolve-LatestTag {
    param([string] $Repository)
    $api = "https://api.github.com/repos/$Repository/releases/latest"
    try {
        $headers = @{ 'User-Agent' = 'ambil-installer' }
        $resp = Invoke-WebRequest -Uri $api -UseBasicParsing -Headers $headers -ErrorAction Stop
        $obj  = $resp.Content | ConvertFrom-Json
        if (-not $obj.tag_name) { throw 'no tag_name field' }
        return [string] $obj.tag_name
    } catch {
        Die "could not resolve latest release for $Repository ($($_.Exception.Message))"
    }
}

function Get-Arch {
    # PROCESSOR_ARCHITECTURE is the native arch under 64-bit PowerShell;
    # under WOW64 we also consult PROCESSOR_ARCHITEW6432.
    $native = $env:PROCESSOR_ARCHITEW6432
    if (-not $native) { $native = $env:PROCESSOR_ARCHITECTURE }
    switch -Regex ($native) {
        '^(AMD64|x64)$' { return 'x86_64' }
        '^ARM64$'       { return 'aarch64' }
        '^x86$'         { return 'i686' }
        default         { Die "unsupported architecture: $native" }
    }
}

function Get-FileHashHex {
    param([string] $Path)
    return (Get-FileHash -Path $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Add-ToUserPath {
    param([string] $Dir)
    $user = [Environment]::GetEnvironmentVariable('Path', 'User')
    if (-not $user) { $user = '' }
    $segments = $user -split ';' | Where-Object { $_ -ne '' }
    foreach ($s in $segments) {
        if ([IO.Path]::GetFullPath($s) -ieq [IO.Path]::GetFullPath($Dir)) {
            Write-Verbose "$Dir already on user PATH"
            return $false
        }
    }
    $newPath = if ($user) { "$user;$Dir" } else { $Dir }
    [Environment]::SetEnvironmentVariable('Path', $newPath, 'User')
    # Reflect change in current session too.
    $env:Path = "$env:Path;$Dir"
    return $true
}

# --- resolve version --------------------------------------------------------

if ($Version -eq 'latest') {
    Write-Step "resolving latest release of $Repo"
    $Version = Resolve-LatestTag -Repository $Repo
}
if ($Version -notmatch '^v') { $Version = "v$Version" }

# --- platform ---------------------------------------------------------------

$arch  = Get-Arch
$asset = "ambil-$Version-$arch-windows.zip"
$sums  = 'SHA256SUMS'

if (-not $BaseUrl) {
    $BaseUrl = "https://github.com/$Repo/releases/download/$Version"
}
$assetUrl = "$BaseUrl/$asset"
$sumsUrl  = "$BaseUrl/$sums"

# --- workspace --------------------------------------------------------------

$tmpRoot = Join-Path ([IO.Path]::GetTempPath()) ("ambil-install-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $tmpRoot -Force | Out-Null

try {
    $assetPath = Join-Path $tmpRoot $asset
    $sumsPath  = Join-Path $tmpRoot $sums

    Write-Step "downloading $asset"
    Invoke-Download -Url $assetUrl -OutFile $assetPath

    if (-not $NoVerify) {
        Write-Step "downloading $sums"
        Invoke-Download -Url $sumsUrl -OutFile $sumsPath

        $expected = $null
        foreach ($line in Get-Content -LiteralPath $sumsPath) {
            $parts = $line -split '\s+', 2
            if ($parts.Count -lt 2) { continue }
            $name = $parts[1].TrimStart('*').Trim()
            if ($name -eq $asset) { $expected = $parts[0].ToLowerInvariant(); break }
        }
        if (-not $expected) { Die "checksum for $asset not present in $sums" }

        $actual = Get-FileHashHex -Path $assetPath
        if ($expected -ne $actual) {
            Die "checksum mismatch for $asset`n  expected: $expected`n  actual:   $actual"
        }
        Write-Step 'checksum verified'
    } else {
        Write-Warn 'skipping checksum verification (--NoVerify)'
    }

    # --- extract -----------------------------------------------------------
    $extractDir = Join-Path $tmpRoot 'extract'
    New-Item -ItemType Directory -Path $extractDir -Force | Out-Null
    Write-Step 'extracting'
    Expand-Archive -LiteralPath $assetPath -DestinationPath $extractDir -Force

    $binSrc = Get-ChildItem -Path $extractDir -Filter 'ambil.exe' -Recurse -File |
              Select-Object -First 1
    if (-not $binSrc) { Die 'ambil.exe not found in archive' }

    # --- install -----------------------------------------------------------
    if (-not (Test-Path $InstallDir)) {
        New-Item -ItemType Directory -Path $InstallDir -Force | Out-Null
    }

    $dest = Join-Path $InstallDir 'ambil.exe'
    if ((Test-Path $dest) -and -not $Force) {
        Write-Warn "$dest already exists; pass -Force to overwrite"
        # Still allow re-run if the existing binary is the same file.
        $existingHash = Get-FileHashHex -Path $dest
        $newHash      = Get-FileHashHex -Path $binSrc.FullName
        if ($existingHash -eq $newHash) {
            Write-Step 'existing binary is identical; nothing to do'
        } else {
            Die 'install aborted'
        }
    } else {
        # Atomic-ish: write to .new then move.
        $stage = "$dest.new"
        Copy-Item -LiteralPath $binSrc.FullName -Destination $stage -Force
        if (Test-Path $dest) { Remove-Item -LiteralPath $dest -Force }
        Move-Item -LiteralPath $stage -Destination $dest -Force
        Write-Step "installed to $dest"
    }

    # --- PATH --------------------------------------------------------------
    if (-not $NoPath) {
        if (Add-ToUserPath -Dir $InstallDir) {
            Write-Step "added $InstallDir to user PATH (open a new terminal to pick it up)"
        } else {
            Write-Verbose "PATH already contains $InstallDir"
        }
    }

    # --- verify install ----------------------------------------------------
    try {
        $ver = & $dest --version 2>$null
        if ($LASTEXITCODE -eq 0 -and $ver) {
            Write-Step "installed: $ver"
        }
    } catch {}

    Write-Step 'done. try: ambil --help'
}
finally {
    if (Test-Path $tmpRoot) {
        Remove-Item -LiteralPath $tmpRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}
