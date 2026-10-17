#Requires -Version 5.1
<#
.SYNOPSIS
    Checks a PEMF installation and says, in one run, why it is not loading.

.DESCRIPTION
    PEMF is silent when it fails to load, which makes "nothing happened" the
    hardest report to act on: not installed correctly, blocked by Windows, and
    loaded-but-refused all look identical from the outside. This script reads the
    install and reports which of those it is.

    It is read-only. It does not need administrator privileges, it does not
    modify the game folder, and it changes nothing on the system. A mod that
    asks you to run an elevated script off the internet has not earned that, so
    the two checks that would need elevation are named and skipped rather than
    demanded.

    Send the whole output with a bug report. That is what it is for.

.PARAMETER GameDir
    The folder containing Pirates!.exe. Left out, we look in the current folder,
    then beside this script, then at the usual Steam and GOG locations.

.PARAMETER Deep
    Also search the disks for stray copies of the game and of PEMF's logs. Slow
    (a minute or two), and only worth it when the quick checks come back clean.

.EXAMPLE
    .\check_install.ps1

.EXAMPLE
    .\check_install.ps1 -GameDir "D:\games\Steam\steamapps\common\Sid Meier's Pirates!"
#>

[CmdletBinding()]
param(
    [string] $GameDir,
    [switch] $Deep
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Continue'

# ---------------------------------------------------------------- findings
# Everything the script learns lands here with a severity, and the verdict at
# the end is assembled from it. The point of the run is the verdict: a wall of
# raw data is what we are trying to save the reader from.

$script:findings = New-Object System.Collections.ArrayList

function Add-Finding {
    param(
        [ValidateSet('fail', 'warn', 'info', 'ok')] [string] $Level,
        [string] $Message,
        [string] $NextAction = '',
        # Lower ranks are more specific. Several things can be wrong at once and
        # the reader deserves the one that actually explains their symptom, not
        # whichever check happened to run first.
        [int] $Rank = 50
    )
    $null = $script:findings.Add([pscustomobject]@{
        Level      = $Level
        Message    = $Message
        NextAction = $NextAction
        Rank       = $Rank
    })
}

function Write-Head {
    param([string] $Text)
    Write-Host ''
    Write-Host "-- $Text " -ForegroundColor Cyan -NoNewline
    Write-Host ('-' * [Math]::Max(0, 66 - $Text.Length)) -ForegroundColor DarkCyan
}

function Write-Line {
    param([string] $Label, [string] $Value, [string] $Colour = 'Gray')
    Write-Host ("  {0,-22} " -f $Label) -NoNewline
    Write-Host $Value -ForegroundColor $Colour
}

# ------------------------------------------------------------- known hashes
# Published on each release's own page. We list only releases whose per-file
# hashes we have published, and we always print what we measured, so a reader
# on a release that is not in this table can still check it themselves against
# the releases page. A table that is missing an entry is a nuisance; a table
# that confidently gives the wrong answer is worse.

$knownHashes = @{
    'D5498ED9B373EFDB9E9A752F42058A04B82B26CFDC27C0433EDEC03CAF28E3B6' = 'version.dll 0.3.0'
    'BC3CD55365913A1B1AA183931A6D9C2BED82C4D3173F9A23C25C8C0A97628DBB' = 'pemf_core.dll 0.3.0'
    '62E2BD9A6959A10DBC67638EDB73A956898F8B75CCE25193BDA9407F4D4072B3' = 'version.dll 0.2.6'
    '3B220098E12E7B1165CEAAC479A6D8E2136231789EB7222A5EB1B8EB53A57F7A' = 'pemf_core.dll 0.2.6'
    'B0AB5F2B0AD88625C9B2FC239AD0DE808CB48406977753A793122E7F429A2E12' = 'version.dll 0.2.4'
    'A095B7471D282ED905844A53B85A2B2143C195938072DFA55C4AD3F5E5AC3519' = 'pemf_core.dll 0.2.4'
    '4A97CF1D1812CF1CEC5C639AB83FB62F2E3AFAF76FD7F284834BAB8208DD8175' = 'version.dll 0.2.0'
    '4997313EA02A4496285A94F82AC17216031CCF7BA7559ED9F97A0625059AE8FE' = 'pemf_core.dll 0.2.0'
}

# Sizes are enough to tell the three shipped builds apart, and unlike a hash
# they survive the game being patched in ways that do not matter to us.
$knownExes = @{
    3323288 = 'GOG (stock) -- fully supported'
    1189888 = 'Steam (DRM-packed) -- supported'
}

# --------------------------------------------------------------- game folder

function Find-GameDir {
    param([string] $Hint)

    $candidates = New-Object System.Collections.ArrayList
    if ($Hint) { $null = $candidates.Add($Hint) }
    $null = $candidates.Add((Get-Location).Path)
    if ($PSScriptRoot) {
        $null = $candidates.Add($PSScriptRoot)
        $null = $candidates.Add((Split-Path -Parent $PSScriptRoot))
    }

    # Steam records its library roots in a file we can read without elevation.
    foreach ($steamRoot in @('C:\Program Files (x86)\Steam', 'C:\Program Files\Steam')) {
        $vdf = Join-Path $steamRoot 'steamapps\libraryfolders.vdf'
        if (Test-Path -LiteralPath $vdf) {
            foreach ($m in (Select-String -LiteralPath $vdf -Pattern '"path"\s+"(.+?)"' -AllMatches).Matches) {
                $lib = $m.Groups[1].Value -replace '\\\\', '\'
                $null = $candidates.Add((Join-Path $lib "steamapps\common\Sid Meier's Pirates!"))
            }
        }
    }
    $null = $candidates.Add("C:\Program Files (x86)\Steam\steamapps\common\Sid Meier's Pirates!")
    $null = $candidates.Add("C:\GOG Games\Sid Meier's Pirates")

    foreach ($c in $candidates) {
        if ($c -and (Test-Path -LiteralPath (Join-Path $c 'Pirates!.exe'))) { return (Resolve-Path -LiteralPath $c).Path }
    }
    return $null
}

# ------------------------------------------------------------------- PE bits
# We read the machine word out of the header ourselves rather than shelling out
# to a toolchain the reader will not have. Thirty-two bit is 0x014C, and it is
# the only answer the game or PEMF should ever give.

function Get-PeMachine {
    param([string] $Path)
    try {
        $fs = [System.IO.File]::Open($Path, 'Open', 'Read', 'ReadWrite')
        try {
            $br = New-Object System.IO.BinaryReader($fs)
            $fs.Position = 0x3C
            $peOffset = $br.ReadInt32()
            $fs.Position = $peOffset
            if ($br.ReadUInt32() -ne 0x00004550) { return $null }   # 'PE\0\0'
            return $br.ReadUInt16()
        } finally { $fs.Dispose() }
    } catch { return $null }
}

function Format-Machine {
    param($Machine)
    switch ($Machine) {
        0x014C  { '32-bit x86' }
        0x8664  { '64-bit x64' }
        0xAA64  { '64-bit ARM' }
        default { if ($null -eq $Machine) { 'unreadable' } else { ('unknown 0x{0:X4}' -f $Machine) } }
    }
}

# =============================================================== run starts

Write-Host ''
Write-Host '  PEMF install check' -ForegroundColor White
Write-Host '  Read-only. Nothing here is modified. No administrator rights needed.' -ForegroundColor DarkGray

$dir = Find-GameDir -Hint $GameDir
if (-not $dir) {
    Write-Host ''
    Write-Host '  Could not find Pirates!.exe.' -ForegroundColor Red
    Write-Host '  Run this from your game folder, or pass it:' -ForegroundColor Gray
    Write-Host '      .\check_install.ps1 -GameDir "D:\...\Sid Meier''s Pirates!"' -ForegroundColor Gray
    Write-Host '  In Steam: right-click the game -> Manage -> Browse local files.' -ForegroundColor Gray
    Write-Host ''
    exit 1
}

$exe      = Join-Path $dir 'Pirates!.exe'
$proxy    = Join-Path $dir 'version.dll'
$core     = Join-Path $dir 'pemf_core.dll'
$proxyLog = Join-Path $dir 'pemf_proxy.log'
$coreLog  = Join-Path $dir 'pemf.log'

# ---------------------------------------------------------------- the game
Write-Head 'The game'
Write-Line 'Folder' $dir 'White'

$exeItem = Get-Item -LiteralPath $exe
$exeMach = Get-PeMachine -Path $exe
Write-Line 'Pirates!.exe' ("{0:N0} bytes, {1}" -f $exeItem.Length, (Format-Machine $exeMach))

if ($knownExes.ContainsKey([int]$exeItem.Length)) {
    Write-Line 'Build' $knownExes[[int]$exeItem.Length] 'Green'
} else {
    Write-Line 'Build' 'not a size we recognise' 'Yellow'
    Add-Finding -Level warn -Message 'Pirates!.exe is not a size we recognise. The Challenge Pack executable is a different build of the game and PEMF refuses to touch it.' -NextAction 'Check pemf.log for a target-verification line. If you are on the Challenge Pack, PEMF cannot support it.'
}

# --------------------------------------------------------------- the files
Write-Head 'PEMF files'

$missing = @()
foreach ($pair in @(@{ N = 'version.dll'; P = $proxy }, @{ N = 'pemf_core.dll'; P = $core })) {
    if (-not (Test-Path -LiteralPath $pair.P)) {
        Write-Line $pair.N 'MISSING' 'Red'
        $missing += $pair.N
        continue
    }

    $item = Get-Item -LiteralPath $pair.P

    # Openability is tested BEFORE anything that reads the file, because every
    # check below needs the contents and each one fails separately on a file
    # that cannot be opened -- which buried the actual answer under a wall of
    # PowerShell exceptions and, worse, reported the DLL as "not 32-bit" when
    # the truth was that its header had never been read at all. A diagnostic
    # that invents a second fault out of the first one is worse than no
    # diagnostic.
    #
    # It matters in the first place because of how the loader fails. Asked for
    # version.dll it searches the executable's own folder first, and if it
    # cannot open what it finds there it does not stop and it does not
    # complain -- it carries on down the search order and loads Windows' own
    # copy out of System32. The game then starts perfectly, with no PEMF and no
    # log of any kind, which looks exactly like never having installed it.
    #
    # It is also why this kind of fault tracks the FOLDER rather than the file:
    # permissions are inherited from the directory, so the identical DLL copied
    # somewhere else picks up new ones and works, and moving the game to another
    # library "fixes" it without explaining anything. Reported by a tester whose
    # game ran fine from a C: library and loaded System32's version.dll from a
    # D: one.
    $openError = $null
    try {
        $fs = [System.IO.File]::Open($pair.P, [System.IO.FileMode]::Open,
                                     [System.IO.FileAccess]::Read,
                                     [System.IO.FileShare]::ReadWrite)
        $fs.Close()
    } catch {
        $openError = $_.Exception.Message
    }

    if ($openError) {
        Write-Line $pair.N ("{0:N0} bytes, CANNOT BE OPENED FOR READING" -f $item.Length) 'Red'
        Write-Host ('      ' + $openError) -ForegroundColor DarkGray
        $owner = $null
        try { $owner = (Get-Acl -LiteralPath $pair.P).Owner } catch { }
        if ($owner) { Write-Line '  Owner' $owner 'Yellow' }
        try {
            $exeOwner = (Get-Acl -LiteralPath $exe).Owner
            if ($exeOwner -and $owner -and $exeOwner -ne $owner) {
                Write-Line '  Pirates!.exe owner' $exeOwner 'Yellow'
                Add-Finding -Level info -Message "$($pair.N) is owned by $owner but Pirates!.exe is owned by $exeOwner. A folder carried over from another Windows install is the usual reason."
            }
        } catch { }

        Add-Finding -Level fail -Rank 6 `
            -Message "$($pair.N) is in the right place but cannot be opened for reading. The loader will skip it in silence and load Windows' own copy instead, which is why the game runs and PEMF does not." `
            -NextAction "Check the FOLDER's permissions, not the file's -- they are inherited. Quickest test: copy the whole game folder somewhere else and run it from there. If that works it is permissions, and to repair in place, from an administrator prompt: icacls `"$dir`" /reset /t"

        # Nothing below can read the file, and guessing at its contents is how
        # the "not 32-bit" nonsense got printed.
        continue
    }

    $hash = $null
    try { $hash = (Get-FileHash -LiteralPath $pair.P -Algorithm SHA256).Hash } catch { }
    $mach = Get-PeMachine -Path $pair.P
    $ver  = $item.VersionInfo.FileVersion
    if (-not $ver) { $ver = 'no version resource' }

    Write-Line $pair.N ("{0:N0} bytes, {1}, v{2}" -f $item.Length, (Format-Machine $mach), $ver)
    if (-not $hash) { $hash = '' }
    if ($knownHashes.ContainsKey($hash)) {
        Write-Line '  SHA256' ("matches published {0}" -f $knownHashes[$hash]) 'Green'
    } else {
        Write-Line '  SHA256' $hash 'Yellow'
        Add-Finding -Level info -Message "$($pair.N) is not one of the release hashes we have published per-file. That is expected on a release we did not tabulate, or on a build you made yourself." -NextAction 'Compare the hash above against the release you downloaded, on its own page: https://github.com/pemfofficial/PEMF/releases'
    }

    if ($mach -ne 0x014C) {
        Add-Finding -Level fail -Message "$($pair.N) is not 32-bit. The game is a 32-bit process and cannot load it." -NextAction 'Download the release again; something has gone wrong with the file.'
    }

    # Mark of the Web. The zip came off the internet, so both DLLs may carry a
    # zone tag. On its own that stops nothing -- plain Windows does not refuse
    # to load a tagged DLL -- but it is free to clear and it removes a variable.
    $ads = Get-Item -LiteralPath $pair.P -Stream 'Zone.Identifier' -ErrorAction SilentlyContinue
    if ($ads) {
        Write-Line '  Zone tag' 'present (downloaded from the internet)' 'Yellow'
        Add-Finding -Level warn -Message "$($pair.N) still carries its downloaded-from-the-internet tag." -NextAction "Clear it: Unblock-File -LiteralPath `"$($pair.P)`""
    } else {
        Write-Line '  Zone tag' 'none' 'DarkGray'
    }
}

if ($missing.Count -gt 0) {
    $verb = if ($missing.Count -gt 1) { 'are' } else { 'is' }
    Add-Finding -Level fail -Rank 20 -Message ("Not installed: {0} {1} not beside Pirates!.exe." -f ($missing -join ' and '), $verb) -NextAction "Copy the contents of the zip into $dir so the DLLs sit next to Pirates!.exe."
}

# -------------------------------------------------------- the subfolder trap
# Windows' own "Extract All" does not put files where you point it -- it makes a
# subfolder named after the zip. This is the commonest install mistake by a wide
# margin, and on an upgrade it is worse than doing nothing, because the old DLLs
# are still in place and still loading.

$stray = @(Get-ChildItem -LiteralPath $dir -Directory -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -ne 'PEMF' } |
    Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'version.dll') })

if ($stray.Count -gt 0) {
    Write-Head 'Extracted into a subfolder'
    foreach ($s in $stray) { Write-Line 'Found' ("{0}\version.dll" -f $s.Name) 'Red' }
    Add-Finding -Level fail -Rank 10 -Message 'A copy of version.dll is sitting in a subfolder of the game folder. Windows'' "Extract All" does that by default, and the game never looks there.' -NextAction ("Move version.dll, pemf_core.dll and PEMF out of {0} and into {1}." -f $stray[0].Name, $dir)
}

# ------------------------------------------------------------------- logs
# These two files are the whole diagnosis. version.dll writes pemf_proxy.log
# before it does anything else, and pemf_core.dll writes pemf.log once it is
# running. Which of them exists tells you where the chain broke.

Write-Head 'Logs'

$sawProxyLog = Test-Path -LiteralPath $proxyLog
$sawCoreLog  = Test-Path -LiteralPath $coreLog

foreach ($pair in @(@{ N = 'pemf_proxy.log'; P = $proxyLog }, @{ N = 'pemf.log'; P = $coreLog })) {
    if (-not (Test-Path -LiteralPath $pair.P)) {
        Write-Line $pair.N 'not present' 'Yellow'
        continue
    }
    $item = Get-Item -LiteralPath $pair.P
    Write-Line $pair.N ("last written {0}" -f $item.LastWriteTime) 'Green'
    $tail = @(Get-Content -LiteralPath $pair.P -Tail 3 -ErrorAction SilentlyContinue)
    foreach ($l in $tail) { Write-Host ("      {0}" -f $l) -ForegroundColor DarkGray }
}

if ($sawCoreLog) {
    $mixed = @(Select-String -LiteralPath $coreLog -Pattern 'MIXED INSTALL' -SimpleMatch -ErrorAction SilentlyContinue)
    if ($mixed.Count -gt 0) {
        Add-Finding -Level fail -Rank 15 -Message 'pemf.log says MIXED INSTALL: the two DLLs are from different releases.' -NextAction 'Delete version.dll and pemf_core.dll, then extract one zip cleanly over the game folder.'
    } else {
        Add-Finding -Level ok -Message 'Both halves of PEMF loaded and pemf.log was written. Whatever is wrong is not the loader.' -NextAction 'Read pemf.log from the top -- it says what it detected and what it refused.'
    }
} elseif ($sawProxyLog) {
    Add-Finding -Level fail -Rank 18 -Message 'version.dll loaded and wrote pemf_proxy.log, but pemf_core.dll never wrote pemf.log. The loader is working; the core is being refused.' -NextAction 'Read the last lines of pemf_proxy.log above. A GetLastError of 126 means pemf_core.dll is missing or one of its dependencies is; 5 means something blocked it.'
} else {
    Add-Finding -Level fail -Rank 30 -Message 'Neither log exists, so version.dll was never loaded at all. The game did not read the DLL you installed.' -NextAction 'Almost always this means the files are not in the folder the game actually runs from. See "Where the game really runs from" below.'
}

# ------------------------------------------------ where the game really runs
# The single most useful check in this script when neither log exists. If the
# game is running we can ask the process itself, which settles in one line what
# an afternoon with Process Monitor may not.

Write-Head 'Where the game really runs from'

$procs = @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -eq 'Pirates!' })
if ($procs.Count -eq 0) {
    Write-Line 'Game running' 'no' 'DarkGray'
    Write-Host '      Start the game the way that fails -- through Steam, if that is how you' -ForegroundColor DarkGray
    Write-Host '      play -- leave it at the title screen, and run this script again.' -ForegroundColor DarkGray
} else {
    foreach ($p in $procs) {
        $imagePath = $null
        try { $imagePath = $p.MainModule.FileName } catch { }
        if (-not $imagePath) {
            Write-Line 'Running image' 'could not be read' 'Yellow'
            continue
        }
        Write-Line 'Running image' $imagePath 'White'

        $runDir = Split-Path -Parent $imagePath
        if ($runDir -ne $dir) {
            Add-Finding -Level fail -Rank 5 -Message "The game that is running is not the copy you installed PEMF into. It is running from $runDir." -NextAction "Install PEMF into $runDir instead, or point this script at that folder."
        }

        $loaded = $null
        try { $loaded = @($p.Modules | Where-Object { $_.ModuleName -ieq 'version.dll' }) } catch { }
        if ($loaded -and $loaded.Count -gt 0) {
            $from = $loaded[0].FileName
            $isOurs = ($from -and (Split-Path -Parent $from) -ieq $runDir)
            Write-Line 'version.dll loaded' $from ($(if ($isOurs) { 'Green' } else { 'Red' }))
            if (-not $isOurs) {
                Add-Finding -Level fail -Rank 8 -Message "The game loaded Windows' own version.dll from $from, not PEMF's." -NextAction 'Confirm version.dll is beside Pirates!.exe in the folder named as the running image above. If it is, something in the process loaded version.dll before the game did -- close overlays and injectors (RivaTuner, MSI Afterburner, Discord, recording software) and try again.'
            }
        } else {
            Write-Line 'version.dll loaded' 'not yet -- try again once the title screen is up' 'Yellow'
        }
    }
}

# --------------------------------------------------------- other installs
# More than one copy of the game is a common and quiet cause of all of this:
# PEMF goes into one, Steam launches the other, and every symptom is silence.

Write-Head 'Other copies of the game'

$others = New-Object System.Collections.ArrayList
foreach ($gog in @("C:\GOG Games\Sid Meier's Pirates", "C:\Program Files (x86)\GOG Galaxy\Games\Sid Meier's Pirates")) {
    $cand = Join-Path $gog 'Pirates!.exe'
    if ((Test-Path -LiteralPath $cand) -and ($gog -ne $dir)) { $null = $others.Add($cand) }
}
foreach ($steamRoot in @('C:\Program Files (x86)\Steam', 'C:\Program Files\Steam')) {
    $vdf = Join-Path $steamRoot 'steamapps\libraryfolders.vdf'
    if (-not (Test-Path -LiteralPath $vdf)) { continue }
    foreach ($m in (Select-String -LiteralPath $vdf -Pattern '"path"\s+"(.+?)"' -AllMatches).Matches) {
        $lib  = $m.Groups[1].Value -replace '\\\\', '\'
        $cand = Join-Path $lib "steamapps\common\Sid Meier's Pirates!\Pirates!.exe"
        if ((Test-Path -LiteralPath $cand) -and ((Split-Path -Parent $cand) -ne $dir)) { $null = $others.Add($cand) }
    }
}
if ($Deep) {
    foreach ($drive in (Get-PSDrive -PSProvider FileSystem | Where-Object { $_.Free -ne $null })) {
        Get-ChildItem -LiteralPath $drive.Root -Filter 'Pirates!.exe' -Recurse -File -Force -ErrorAction SilentlyContinue |
            Where-Object { $_.DirectoryName -ne $dir } |
            ForEach-Object { $null = $others.Add($_.FullName) }
    }
}

$others = @($others | Sort-Object -Unique)
if ($others.Count -eq 0) {
    Write-Line 'Found' 'none besides this one' 'DarkGray'
    if (-not $Deep) { Write-Host '      (Steam libraries only. Pass -Deep to search the disks properly.)' -ForegroundColor DarkGray }
} else {
    foreach ($o in $others) { Write-Line 'Also installed' $o 'Yellow' }
    Add-Finding -Level warn -Message 'There is more than one copy of the game on this machine. If Steam launches a different one than you modded, PEMF is silent and everything looks correct.' -NextAction 'In Steam: right-click the game -> Manage -> Browse local files. That is the folder the DLLs must go in.'
}

# ------------------------------------------------------------ Windows policy
# Neither of these is silent when it fires, so a clean reading here does not
# clear Windows entirely -- but a dirty one explains a great deal at once.

Write-Head 'Windows'

$sac = $null
try { $sac = Get-ItemPropertyValue -Path 'HKLM:\SYSTEM\CurrentControlSet\Control\CI\Policy' -Name 'VerifiedAndReputablePolicyState' -ErrorAction Stop } catch { }
switch ($sac) {
    0       { Write-Line 'Smart App Control' 'off' 'Green' }
    1       { Write-Line 'Smart App Control' 'ENFORCED' 'Red'
              Add-Finding -Level fail -Message 'Smart App Control is enforcing. It refuses to load unsigned code, and PEMF is unsigned.' -NextAction 'You would see a "Bad Image ... 0xC0E90002" dialog if this were your fault. See docs/WINDOWS_SECURITY.md -- turning it off cannot be undone.' }
    2       { Write-Line 'Smart App Control' 'evaluation' 'Yellow'
              Add-Finding -Level warn -Message 'Smart App Control is in evaluation mode. It is not blocking yet, but it can switch itself to enforcing.' -NextAction 'See docs/WINDOWS_SECURITY.md.' }
    default { Write-Line 'Smart App Control' 'not present on this machine' 'DarkGray' }
}

$ss = $null
try { $ss = Get-ItemPropertyValue -Path 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer' -Name 'SmartScreenEnabled' -ErrorAction Stop } catch { }
Write-Line 'SmartScreen (apps)' $(if ($ss) { $ss } else { 'default' }) 'DarkGray'

Write-Host '      Not checked, because it would need administrator rights and we are not' -ForegroundColor DarkGray
Write-Host '      going to ask for them: Defender quarantine history, and the Prefetch' -ForegroundColor DarkGray
Write-Host '      folder. Neither has ever been shown to cause this.' -ForegroundColor DarkGray

# ------------------------------------------------------------------ verdict
Write-Host ''
Write-Host ('=' * 72) -ForegroundColor DarkCyan
Write-Host '  VERDICT' -ForegroundColor White

$fails = @($script:findings | Where-Object { $_.Level -eq 'fail' } | Sort-Object Rank)
$warns = @($script:findings | Where-Object { $_.Level -eq 'warn' } | Sort-Object Rank)
$oks   = @($script:findings | Where-Object { $_.Level -eq 'ok' })

$headline = $null
if ($fails.Count -gt 0)      { $headline = $fails[0] }
elseif ($oks.Count -gt 0)    { $headline = $oks[0] }
elseif ($warns.Count -gt 0)  { $headline = $warns[0] }

if (-not $headline) {
    Write-Host ''
    Write-Host '  The install looks right and nothing here explains a failure.' -ForegroundColor Green
    Write-Host '  Next: start the game, quit, and run this again -- the logs are the answer.' -ForegroundColor Gray
} else {
    $colour = switch ($headline.Level) { 'fail' { 'Red' } 'ok' { 'Green' } default { 'Yellow' } }
    Write-Host ''
    Write-Host ('  ' + $headline.Message) -ForegroundColor $colour
    if ($headline.NextAction) {
        Write-Host ''
        Write-Host '  Do this next:' -ForegroundColor White
        Write-Host ('    ' + $headline.NextAction) -ForegroundColor Gray
    }
}

$rest = @($script:findings | Where-Object { $_ -ne $headline -and $_.Level -in @('fail', 'warn') } | Sort-Object Rank)
if ($rest.Count -gt 0) {
    Write-Host ''
    Write-Host '  Also worth fixing:' -ForegroundColor White
    foreach ($f in $rest) { Write-Host ('    - ' + $f.Message) -ForegroundColor DarkGray }
}

Write-Host ''
Write-Host '  Full detail: docs/TROUBLESHOOTING.md' -ForegroundColor DarkGray
Write-Host ('=' * 72) -ForegroundColor DarkCyan
Write-Host ''
