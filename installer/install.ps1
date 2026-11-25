# cluau's installer for Windows: downloads a release, checks it against the
# release's SHA256SUMS, installs `cluau` for the current user and puts it on
# the user's PATH. No administrator rights are needed.
#
#   irm https://github.com/Aosiee/cluau/releases/latest/download/install.ps1 | iex
#   .\install.ps1 [-Version vX.Y.Z] [-InstallDir <dir>] [-NoModifyPath] [-AssociateFiles] [-Uninstall]
#
# The command goes to <InstallDir>\bin (InstallDir is
# %LOCALAPPDATA%\Programs\cluau unless -InstallDir names another), the guides
# to <InstallDir>\doc. -AssociateFiles also opens .cluau files with cluau
# (double-click, or `script.cluau` typed in cmd.exe), for the current user.
#
# The environment can stand in for the options when the script is piped to
# iex: CLUAU_VERSION, CLUAU_INSTALL_DIR. CLUAU_DOWNLOAD_URL replaces the
# release's download address (a URL or a local directory holding the
# release's files), for mirrors and for testing a release before it is
# published.

param(
    [string]$Version = $(if ($env:CLUAU_VERSION) { $env:CLUAU_VERSION } else { 'latest' }),
    [string]$InstallDir = $(if ($env:CLUAU_INSTALL_DIR) { $env:CLUAU_INSTALL_DIR } else { Join-Path $env:LOCALAPPDATA 'Programs\cluau' }),
    [switch]$NoModifyPath,
    [switch]$AssociateFiles,
    [switch]$Uninstall
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue' # Invoke-WebRequest is far slower drawing its progress bar
$Repo = 'Aosiee/cluau'
$BinDir = Join-Path $InstallDir 'bin'
$DocDir = Join-Path $InstallDir 'doc'
$FileTypeKey = 'HKCU:\Software\Classes\.cluau'
$ProgIdKey = 'HKCU:\Software\Classes\cluau.script'

function Say([string]$Message) { Write-Host "cluau: $Message" }
function Fail([string]$Message) { throw "cluau: $Message" }

# The user's PATH entries, without empty ones.
function Get-UserPath {
    $value = [Environment]::GetEnvironmentVariable('Path', 'User')
    if (-not $value) { return @() }
    return @($value -split ';' | Where-Object { $_ -ne '' })
}

function Set-UserPath([string[]]$Entries) {
    [Environment]::SetEnvironmentVariable('Path', ($Entries -join ';'), 'User')
}

function Test-SameDir([string]$A, [string]$B) {
    return $A.TrimEnd('\') -ieq $B.TrimEnd('\')
}

if ($Uninstall) {
    $removed = $false
    if (Test-Path $InstallDir) {
        Remove-Item -Recurse -Force $InstallDir
        $removed = $true
    }
    $entries = Get-UserPath
    $kept = @($entries | Where-Object { -not (Test-SameDir $_ $BinDir) })
    if ($kept.Count -ne $entries.Count) {
        Set-UserPath $kept
        Say "removed $BinDir from your PATH"
    }
    foreach ($key in $FileTypeKey, $ProgIdKey) {
        if (Test-Path $key) { Remove-Item -Recurse -Force $key }
    }
    if ($removed) { Say "removed cluau from $InstallDir" } else { Say "cluau is not installed in $InstallDir" }
    return
}

# The release for this system (x64 Windows; Windows on Arm runs the x64 build).
$arch = $env:PROCESSOR_ARCHITECTURE
if ($arch -ne 'AMD64' -and $arch -ne 'ARM64') {
    Fail "there is no release build for $arch Windows: build cluau from source (https://github.com/$Repo#building)"
}
$Platform = 'windows-x86_64'

# Older Windows PowerShell may not offer TLS 1.2 by default.
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

if ($Version -eq 'latest') {
    if ($env:CLUAU_DOWNLOAD_URL) { Fail 'CLUAU_DOWNLOAD_URL needs a version (-Version vX.Y.Z)' }
    try {
        $Version = (Invoke-RestMethod -UseBasicParsing "https://api.github.com/repos/$Repo/releases/latest").tag_name
    } catch {
        Fail "cannot find the latest release (GitHub's API did not answer): $($_.Exception.Message)"
    }
}
if (-not $Version.StartsWith('v')) { $Version = "v$Version" }
$Base = if ($env:CLUAU_DOWNLOAD_URL) { $env:CLUAU_DOWNLOAD_URL } else { "https://github.com/$Repo/releases/download/$Version" }
$Archive = "cluau-$Version-$Platform.zip"

# A file of the release: from the download address, or a local directory.
function Get-ReleaseFile([string]$Name, [string]$To) {
    if ($Base -match '^https?://') {
        try { Invoke-WebRequest -UseBasicParsing "$Base/$Name" -OutFile $To }
        catch { Fail "cannot download $Base/$Name`: $($_.Exception.Message)" }
    } else {
        $from = Join-Path $Base $Name
        if (-not (Test-Path $from)) { Fail "no $Name in $Base" }
        Copy-Item $from $To
    }
}

$Work = Join-Path ([IO.Path]::GetTempPath()) ("cluau-install-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $Work | Out-Null
try {
    Say "downloading cluau $Version ($Platform)"
    Get-ReleaseFile $Archive (Join-Path $Work $Archive)
    Get-ReleaseFile 'SHA256SUMS' (Join-Path $Work 'SHA256SUMS')

    $line = Get-Content (Join-Path $Work 'SHA256SUMS') | Where-Object { $_ -match "^([0-9a-fA-F]{64})\s+\*?$([regex]::Escape($Archive))$" } | Select-Object -First 1
    if (-not $line) { Fail "SHA256SUMS has no line for $Archive" }
    $expected = ($line -split '\s+')[0].ToLowerInvariant()
    $actual = (Get-FileHash -Algorithm SHA256 (Join-Path $Work $Archive)).Hash.ToLowerInvariant()
    if ($expected -ne $actual) { Fail "$Archive does not match its checksum (expected $expected, got $actual): not installing it" }

    Expand-Archive -Path (Join-Path $Work $Archive) -DestinationPath $Work
    $Unpacked = Join-Path $Work "cluau-$Version-$Platform"
    $Exe = Join-Path $Unpacked 'cluau.exe'
    if (-not (Test-Path $Exe)) { Fail "$Archive holds no cluau.exe" }

    New-Item -ItemType Directory -Force $BinDir, $DocDir | Out-Null
    $Target = Join-Path $BinDir 'cluau.exe'
    # A running cluau.exe cannot be overwritten, but it can be renamed aside.
    if (Test-Path $Target) {
        $old = "$Target.old"
        if (Test-Path $old) { Remove-Item -Force $old -ErrorAction SilentlyContinue }
        Move-Item -Force $Target $old
    }
    Copy-Item $Exe $Target
    Remove-Item -Force "$Target.old" -ErrorAction SilentlyContinue
    foreach ($doc in 'README.md', 'LICENSE', 'syntax.md', 'scripting.md') {
        $from = Join-Path $Unpacked $doc
        if (Test-Path $from) { Copy-Item $from $DocDir }
    }
} finally {
    Remove-Item -Recurse -Force $Work -ErrorAction SilentlyContinue
}

$installed = & $Target --version
if ($LASTEXITCODE -ne 0) { Fail "the installed $Target does not run on this system" }
Say "installed $installed to $Target"

if ($AssociateFiles) {
    New-Item -Force $FileTypeKey | Out-Null
    Set-Item $FileTypeKey 'cluau.script'
    New-Item -Force "$ProgIdKey\shell\open\command" | Out-Null
    Set-Item $ProgIdKey 'cluau script'
    Set-Item "$ProgIdKey\shell\open\command" "`"$Target`" `"%1`" %*"
    Say '.cluau files now open with cluau'
}

if (@(Get-UserPath | Where-Object { Test-SameDir $_ $BinDir }).Count -gt 0) {
    Say "$BinDir is on your PATH: run ``cluau`` to start (in a new terminal)"
} elseif ($NoModifyPath) {
    Say "$BinDir is not on your PATH: add it to use ``cluau`` by name"
} else {
    Set-UserPath (@(Get-UserPath) + $BinDir)
    $env:Path = "$env:Path;$BinDir"
    Say "added $BinDir to your PATH: open a new terminal to use ``cluau``"
}
