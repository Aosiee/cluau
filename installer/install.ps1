# cluau's installer for Windows: downloads a release, checks it against the
# release's SHA256SUMS, installs `cluau` for the current user and puts it on
# the user's PATH. No administrator rights are needed.
#
#   irm https://github.com/Aosiee/cluau/releases/latest/download/install.ps1 | iex
#   & ([scriptblock]::Create((irm https://github.com/Aosiee/cluau/releases/latest/download/install.ps1))) -Uninstall
#   powershell -ExecutionPolicy Bypass -File .\install.ps1 [-Version vX.Y.Z] [-InstallDir <dir>] [-NoModifyPath] [-AssociateFiles] [-AddToPathExt] [-Uninstall]
#
# (Windows runs a downloaded script file only when the execution policy
# allows it: -ExecutionPolicy Bypass, or the scriptblock form, which takes
# the options too.) The command goes to <InstallDir>\bin (InstallDir is
# %LOCALAPPDATA%\Programs\cluau unless -InstallDir names another), the guides
# and the licences to <InstallDir>\doc. -AddToPathExt (which implies -AssociateFiles) adds
# .CLUAU to the user's PATHEXT, so a script on PATH runs by its name alone
# (`deploy` for deploy.cluau) in cmd.exe and PowerShell; -Uninstall takes it
# out again. -AssociateFiles also opens .cluau files with cluau
# (double-click, or `script.cluau` typed in cmd.exe), for the current user.
#
# The environment can stand in for the options when the script is piped to
# iex: CLUAU_VERSION, CLUAU_INSTALL_DIR. CLUAU_REPOSITORY is the repository
# the releases come from (its web address: https://github.com/Aosiee/cluau
# unless it names another, such as a Forgejo or Gitea server's), whose API
# gives the latest release. CLUAU_DOWNLOAD_URL replaces a release's
# download address (a URL or a local directory holding the release's
# files), for mirrors and for testing a release before it is published.

param(
    [string]$Version = $(if ($env:CLUAU_VERSION) { $env:CLUAU_VERSION } else { 'latest' }),
    [string]$InstallDir = $(if ($env:CLUAU_INSTALL_DIR) { $env:CLUAU_INSTALL_DIR } else { Join-Path $env:LOCALAPPDATA 'Programs\cluau' }),
    [switch]$NoModifyPath,
    [switch]$AssociateFiles,
    [switch]$AddToPathExt,
    [switch]$Uninstall
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue' # Invoke-WebRequest is far slower drawing its progress bar
$Repository = if ($env:CLUAU_REPOSITORY) { $env:CLUAU_REPOSITORY.TrimEnd('/') } else { 'https://github.com/Aosiee/cluau' }
# owner/name are the address's last two parts; the server is what comes
# before them (a Forgejo or Gitea server may live under a path of its own).
$NameAt = $Repository.LastIndexOf('/')
$OwnerAt = if ($NameAt -gt 0) { $Repository.LastIndexOf('/', $NameAt - 1) } else { -1 }
if ($OwnerAt -lt 0) { throw "cluau: CLUAU_REPOSITORY is not a repository's address (https://host/owner/name): $Repository" }
$Repo = $Repository.Substring($OwnerAt + 1) # owner/name
$Server = $Repository.Substring(0, $OwnerAt) # https://host, or https://host/path
$BinDir = Join-Path $InstallDir 'bin'
$DocDir = Join-Path $InstallDir 'doc'
# The documents a release holds, installed in $DocDir.
$Docs = 'README.md', 'LICENSE', 'THIRD_PARTY_NOTICES.md', 'syntax.md', 'scripting.md'
# The file association, under HKEY_CURRENT_USER.
$FileTypeKey = 'Software\Classes\.cluau'
$ProgIdKey = 'Software\Classes\cluau.script'
$ProgId = 'cluau.script'

function Say([string]$Message) { Write-Host "cluau: $Message" }
function Fail([string]$Message) { throw "cluau: $Message" }

# The user's PATH entries, without empty ones, read from the registry as
# written: an entry such as %USERPROFILE%\bin keeps its variable.
function Get-UserPath {
    $key = Get-Item 'HKCU:\Environment'
    $value = $key.GetValue('Path', '', 'DoNotExpandEnvironmentNames')
    if (-not $value) { return @() }
    return @($value -split ';' | Where-Object { $_ -ne '' })
}

function Set-UserPath([string[]]$Entries) {
    Set-ItemProperty -Path 'HKCU:\Environment' -Name 'Path' -Value ($Entries -join ';') -Type ExpandString
    # Setting (then clearing) a variable through .NET tells running programs,
    # Explorer among them, that the environment changed.
    [Environment]::SetEnvironmentVariable('CLUAU_INSTALLER', '1', 'User')
    [Environment]::SetEnvironmentVariable('CLUAU_INSTALLER', $null, 'User')
}

function Test-InPath([string]$Entry, [string]$Dir) {
    return (Test-SameDir $Entry $Dir) -or (Test-SameDir ([Environment]::ExpandEnvironmentVariables($Entry)) $Dir)
}

function Test-SameDir([string]$A, [string]$B) {
    return $A.TrimEnd('\') -ieq $B.TrimEnd('\')
}

# A free name to rename a command aside to (a running cluau.exe cannot be
# removed or overwritten, but it can be renamed): one left from an earlier
# update while it ran keeps its name.
function Get-OldName([string]$Path) {
    $old = "$Path.old"
    for ($n = 1; (Test-Path $old); $n++) { $old = "$Path.$n.old" }
    return $old
}

# Earlier commands renamed aside (cluau.exe.old, cluau.exe.<n>.old), removed
# when they are no longer running.
function Remove-OldCommands {
    Get-ChildItem -Force -Path $BinDir -Filter 'cluau.exe*.old' -ErrorAction SilentlyContinue |
        ForEach-Object { Remove-Item -Force $_.FullName -ErrorAction SilentlyContinue }
}

# What `cluau --version` prints, or $null with the reason in $script:RunError
# when the program does not run here.
function Get-CommandVersion([string]$Path) {
    $script:RunError = ''
    # Its error output is collected, not raised (in this function only).
    $ErrorActionPreference = 'Continue'
    try {
        $output = & $Path --version 2>&1
        $code = $LASTEXITCODE
    } catch {
        $script:RunError = $_.Exception.Message
        return $null
    }
    if ($code -ne 0) {
        $text = (@($output) | ForEach-Object { "$_" }) -join "`n"
        # A program Windows cannot start ends with an NTSTATUS (0xC0000135:
        # a DLL it needs is missing), clearer in hexadecimal.
        $status = if ($code -lt 0) { '0x{0:X8}' -f $code } else { "$code" }
        $script:RunError = "exit status $status$(if ($text) { ": $text" })"
        return $null
    }
    return "$output".Trim()
}

# PATHEXT: a user's value replaces the machine's (it is not added to it), so
# the user's is the machine's with .CLUAU after it.
function Add-PathExt {
    $current = [Environment]::GetEnvironmentVariable('PATHEXT', 'User')
    if (-not $current) { $current = [Environment]::GetEnvironmentVariable('PATHEXT', 'Machine') }
    $entries = @($current -split ';' | Where-Object { $_ -ne '' })
    if ($entries -inotcontains '.CLUAU') {
        [Environment]::SetEnvironmentVariable('PATHEXT', (($entries + '.CLUAU') -join ';'), 'User')
        $env:PATHEXT = "$env:PATHEXT;.CLUAU"
        Say 'added .CLUAU to your PATHEXT: a script on PATH runs by its name (in a new terminal)'
    }
}

function Remove-PathExt {
    $current = [Environment]::GetEnvironmentVariable('PATHEXT', 'User')
    if (-not $current) { return }
    $entries = @($current -split ';' | Where-Object { $_ -ne '' -and $_ -ine '.CLUAU' })
    $machine = [Environment]::GetEnvironmentVariable('PATHEXT', 'Machine')
    # What is left is the machine's own: the user's value goes.
    $value = if (($entries -join ';') -ieq $machine) { $null } else { $entries -join ';' }
    if ($value -ne $current) {
        [Environment]::SetEnvironmentVariable('PATHEXT', $value, 'User')
        Say 'removed .CLUAU from your PATHEXT'
    }
}

if ($Uninstall) {
    # Only what the installer put there goes; the directories go too once
    # they are empty (the install directory may hold other things).
    $removed = $false
    $Command = Join-Path $BinDir 'cluau.exe'
    if (Test-Path $Command) {
        try { Remove-Item -Force $Command } catch { Move-Item $Command (Get-OldName $Command) }
        $removed = $true
    }
    Remove-OldCommands
    if (Get-ChildItem -Force -Path $BinDir -Filter 'cluau.exe*.old' -ErrorAction SilentlyContinue) {
        Say "a cluau.exe that is still running stays in $BinDir until it ends: remove it then"
    }
    foreach ($doc in $Docs) {
        $path = Join-Path $DocDir $doc
        if (Test-Path $path) {
            Remove-Item -Force $path
            $removed = $true
        }
    }
    foreach ($dir in $BinDir, $DocDir, $InstallDir) {
        if ((Test-Path $dir) -and -not (Get-ChildItem -Force $dir | Select-Object -First 1)) {
            Remove-Item -Force $dir
        }
    }
    $entries = Get-UserPath
    $kept = @($entries | Where-Object { -not (Test-InPath $_ $BinDir) })
    if ($kept.Count -ne $entries.Count) {
        Set-UserPath $kept
        Say "removed $BinDir from your PATH"
    }
    # .cluau's default value, only when it is cluau's (the key may hold other
    # programs' entries, such as OpenWithProgids), and the key itself once
    # it holds nothing else; cluau.script is cluau's own.
    $classes = [Microsoft.Win32.Registry]::CurrentUser
    $fileType = $classes.OpenSubKey($FileTypeKey, $true)
    if ($fileType) {
        try {
            if ($fileType.GetValue('') -eq $ProgId) { $fileType.DeleteValue('', $false) }
            $empty = $fileType.ValueCount -eq 0 -and $fileType.SubKeyCount -eq 0
        } finally {
            $fileType.Close()
        }
        if ($empty) { $classes.DeleteSubKey($FileTypeKey, $false) }
    }
    $progIdEntry = $classes.OpenSubKey($ProgIdKey)
    if ($progIdEntry) {
        $progIdEntry.Close()
        $classes.DeleteSubKeyTree($ProgIdKey, $false)
    }
    Remove-PathExt
    if ($removed) { Say "removed cluau from $InstallDir" } else { Say "cluau is not installed in $InstallDir" }
    return
}

# The release for this system (x64 Windows; Windows on Arm runs the x64 build).
$arch = $env:PROCESSOR_ARCHITECTURE
if ($arch -ne 'AMD64' -and $arch -ne 'ARM64') {
    Fail "there is no release build for $arch Windows: build cluau from source ($Repository#building)"
}
$Platform = 'windows-x86_64'

# Older Windows PowerShell may not offer TLS 1.2 by default.
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

if ($Version -eq 'latest') {
    if ($env:CLUAU_DOWNLOAD_URL) { Fail 'CLUAU_DOWNLOAD_URL needs a version (-Version vX.Y.Z)' }
    # GitHub's API, or a Forgejo or Gitea server's (both answer tag_name).
    $Api = if ($Server -eq 'https://github.com') { "https://api.github.com/repos/$Repo/releases/latest" } else { "$Server/api/v1/repos/$Repo/releases/latest" }
    try {
        $Version = (Invoke-RestMethod -UseBasicParsing $Api).tag_name
    } catch {
        Fail "cannot find the latest release ($Api did not answer): $($_.Exception.Message)"
    }
}
if (-not $Version.StartsWith('v')) { $Version = "v$Version" }
$Base = if ($env:CLUAU_DOWNLOAD_URL) { $env:CLUAU_DOWNLOAD_URL } else { "$Repository/releases/download/$Version" }
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

    # The new command runs here before anything is replaced.
    if (-not (Get-CommandVersion $Exe)) { Fail "cluau $Version does not run on this system ($script:RunError): nothing was changed" }

    New-Item -ItemType Directory -Force $BinDir, $DocDir | Out-Null
    $Target = Join-Path $BinDir 'cluau.exe'
    Remove-OldCommands
    # The earlier command is renamed aside (it may be running) and comes back
    # if the new one does not run once in place.
    $Old = $null
    if (Test-Path $Target) {
        $Old = Get-OldName $Target
        Move-Item $Target $Old
    }
    try {
        Copy-Item $Exe $Target
        $installed = Get-CommandVersion $Target
        if (-not $installed) { Fail "the installed $Target does not run ($script:RunError)" }
    } catch {
        # The earlier command goes back in place.
        Remove-Item -Force $Target -ErrorAction SilentlyContinue
        if ($Old) { Move-Item $Old $Target }
        throw
    }
    if ($Old) { Remove-Item -Force $Old -ErrorAction SilentlyContinue }
    foreach ($doc in $Docs) {
        $from = Join-Path $Unpacked $doc
        if (Test-Path $from) { Copy-Item $from $DocDir }
    }
} finally {
    Remove-Item -Recurse -Force $Work -ErrorAction SilentlyContinue
}
Say "installed $installed to $Target"

if ($AssociateFiles -or $AddToPathExt) {
    # Opened or created, never replaced: the .cluau key may hold other
    # programs' entries.
    $classes = [Microsoft.Win32.Registry]::CurrentUser
    $fileType = $classes.CreateSubKey($FileTypeKey)
    $fileType.SetValue('', $ProgId)
    $fileType.Close()
    $progIdEntry = $classes.CreateSubKey($ProgIdKey)
    $progIdEntry.SetValue('', 'cluau script')
    $progIdEntry.Close()
    $command = $classes.CreateSubKey("$ProgIdKey\shell\open\command")
    $command.SetValue('', "`"$Target`" `"%1`" %*")
    $command.Close()
    Say '.cluau files now open with cluau'
}
if ($AddToPathExt) { Add-PathExt }

if (@(Get-UserPath | Where-Object { Test-InPath $_ $BinDir }).Count -gt 0) {
    Say "$BinDir is on your PATH: run ``cluau`` to start (in a new terminal)"
} elseif ($NoModifyPath) {
    Say "$BinDir is not on your PATH: add it to use ``cluau`` by name"
} else {
    Set-UserPath (@(Get-UserPath) + $BinDir)
    $env:Path = "$env:Path;$BinDir"
    Say "added $BinDir to your PATH: open a new terminal to use ``cluau``"
}
