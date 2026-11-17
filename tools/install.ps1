<#
.SYNOPSIS
    Install or update Clay on Windows (x86_64) from a GitHub release.

.DESCRIPTION
    Downloads clay-<version>-windows-x86_64.zip, checks it against the release's SHA256SUMS,
    and installs it into -Prefix (default %LOCALAPPDATA%\Programs\clay; no admin rights needed),
    replacing any earlier install there. The old install is kept until the new clay.exe runs and
    is restored if anything fails. Adds <Prefix>\bin to the user PATH unless -NoModifyPath.

    Run it again at any time to update; it does nothing if the install is already current.

.EXAMPLE
    # Install or update to the latest release
    irm https://raw.githubusercontent.com/mseminatore/clay/main/tools/install.ps1 | iex

.EXAMPLE
    # With options (a specific version, another folder, ...)
    & ([scriptblock]::Create((irm https://raw.githubusercontent.com/mseminatore/clay/main/tools/install.ps1))) -Version 0.3.5

.EXAMPLE
    # From a clone of the repo
    powershell -ExecutionPolicy Bypass -File tools\install.ps1 -Force
#>
[CmdletBinding()]
param(
    # Release to install, e.g. 0.3.5 (default: latest)
    [string]$Version,
    # Install folder; gets bin\, lib\clay\ and share\ underneath
    [string]$Prefix = (Join-Path $env:LOCALAPPDATA 'Programs\clay'),
    # Reinstall even if this version is already installed
    [switch]$Force,
    # Don't add <Prefix>\bin to the user PATH
    [switch]$NoModifyPath,
    [string]$Repo = 'mseminatore/clay'
)

# Everything runs in a child scope so `irm | iex` doesn't change the caller's preferences.
& {
    $ErrorActionPreference = 'Stop'
    $ProgressPreference = 'SilentlyContinue'   # Invoke-WebRequest is very slow with a progress bar
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

    if (-not [Environment]::Is64BitOperatingSystem) { throw 'Clay releases are built for 64-bit Windows only.' }
    if (-not $Prefix) { throw 'No install folder: pass -Prefix (LOCALAPPDATA is not set).' }
    $Prefix = [IO.Path]::GetFullPath($Prefix)

    # Runs clay.exe --version; returns @(firstLine, exitCode). Collect all output before taking
    # the first line: `| Select-Object -First 1` can stop the process early and leave a bogus
    # nonzero $LASTEXITCODE.
    function Get-ClayVersion([string]$exe) {
        $all = @(& $exe --version 2>&1)
        return @("$($all | Select-Object -First 1)", $LASTEXITCODE)
    }

    # --- which version? -----------------------------------------------------------------------
    if (-not $Version) {
        try {
            $rel = Invoke-RestMethod "https://api.github.com/repos/$Repo/releases/latest" -Headers @{ 'User-Agent' = 'clay-installer' }
        }
        catch { throw "Could not look up the latest release (GitHub API rate limit?). Pass -Version x.y.z instead. $($_.Exception.Message)" }
        $Version = $rel.tag_name
    }
    $Version = $Version.TrimStart('v')

    $exe = Join-Path $Prefix 'bin\clay.exe'
    $current = $null
    if (Test-Path $exe) {
        $line = (Get-ClayVersion $exe)[0]
        if ($line -match '(\d+\.\d+\.\d+)') { $current = $Matches[1] }
    }
    Write-Host "Installed: $(if ($current) { $current } else { 'none' })   Target: $Version   Prefix: $Prefix"

    if ($current -eq $Version -and -not $Force) {
        Write-Host 'Already up to date (use -Force to reinstall).'
    }
    else {
        # --- download and verify --------------------------------------------------------------
        $name = "clay-$Version-windows-x86_64"
        $base = "https://github.com/$Repo/releases/download/v$Version"
        $work = Join-Path ([IO.Path]::GetTempPath()) ('clay-install-' + [guid]::NewGuid().ToString('N'))
        New-Item -ItemType Directory -Path $work | Out-Null
        $backup = "$Prefix.old-" + [guid]::NewGuid().ToString('N').Substring(0, 8)

        try {
            $zip = Join-Path $work "$name.zip"
            Write-Host "Downloading $base/$name.zip"
            try {
                Invoke-WebRequest "$base/$name.zip" -OutFile $zip -UseBasicParsing
                Invoke-WebRequest "$base/SHA256SUMS" -OutFile (Join-Path $work 'SHA256SUMS') -UseBasicParsing
            }
            catch { throw "Download failed (does release v$Version exist?): $($_.Exception.Message)" }

            $sumLine = Get-Content (Join-Path $work 'SHA256SUMS') | Where-Object { $_ -match ('\s\*?' + [regex]::Escape("$name.zip") + '$') }
            if (-not $sumLine) { throw "SHA256SUMS has no entry for $name.zip" }
            $expected = ($sumLine -split '\s+')[0].ToLower()
            # .NET rather than Get-FileHash/Expand-Archive: those live in modules that can be
            # missing when Windows PowerShell is started from another shell.
            $sha = [Security.Cryptography.SHA256]::Create()
            $fs = [IO.File]::OpenRead($zip)
            try { $actual = -join ($sha.ComputeHash($fs) | ForEach-Object { $_.ToString('x2') }) }
            finally { $fs.Dispose(); $sha.Dispose() }
            if ($expected -ne $actual) { throw "Checksum mismatch for $name.zip`n  expected $expected`n  got      $actual" }
            Write-Host 'Checksum OK'

            $extract = Join-Path $work 'x'
            Add-Type -AssemblyName System.IO.Compression.FileSystem
            [IO.Compression.ZipFile]::ExtractToDirectory($zip, $extract)
            $src = Join-Path $extract $name
            if (-not (Test-Path (Join-Path $src 'bin\clay.exe'))) { throw "Unexpected archive layout: no $name\bin\clay.exe" }

            # --- swap installs ----------------------------------------------------------------
            $parent = Split-Path $Prefix -Parent
            if (-not (Test-Path $parent)) { New-Item -ItemType Directory -Path $parent | Out-Null }
            if (Test-Path $Prefix) {
                try { Move-Item $Prefix $backup }
                catch { throw "Could not move $Prefix aside (is clay.exe running?): $($_.Exception.Message)" }
            }
            try {
                Move-Item $src $Prefix
                $v = Get-ClayVersion $exe
                if ($v[1] -ne 0 -or $v[0] -notmatch [regex]::Escape($Version)) {
                    throw "New clay.exe reported '$($v[0])' (exit $($v[1])), expected $Version"
                }
            }
            catch {
                Write-Warning "Install failed, restoring the previous state: $($_.Exception.Message)"
                if (Test-Path $Prefix) { Remove-Item $Prefix -Recurse -Force }
                if (Test-Path $backup) { Move-Item $backup $Prefix }
                throw
            }
            if (Test-Path $backup) { Remove-Item $backup -Recurse -Force -ErrorAction SilentlyContinue }
            Write-Host "Installed $($v[0])"
        }
        finally {
            Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
        }
    }

    # --- PATH ---------------------------------------------------------------------------------
    $bin = Join-Path $Prefix 'bin'
    $userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
    $onUserPath = @($userPath -split ';' | ForEach-Object { $_.TrimEnd('\') }) -contains $bin
    if (-not $onUserPath) {
        if ($NoModifyPath) {
            Write-Host "Note: $bin is not on your PATH."
        }
        else {
            $newPath = if ($userPath) { "$bin;$userPath" } else { $bin }
            [Environment]::SetEnvironmentVariable('Path', $newPath, 'User')
            Write-Host "Added $bin to your user PATH (new terminals pick it up)."
        }
    }
    if (-not $NoModifyPath -and @($env:Path -split ';' | ForEach-Object { $_.TrimEnd('\') }) -notcontains $bin) {
        $env:Path = "$bin;$env:Path"   # this session too
    }
    $resolved = (Get-Command clay -ErrorAction SilentlyContinue).Source
    if ($resolved -and $resolved -ne $exe) {
        Write-Warning "'clay' resolves to $resolved, which comes before $exe on PATH"
    }
}
