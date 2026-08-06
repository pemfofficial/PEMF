# When Windows blocks PEMF

PEMF is **unsigned**, and it will stay unsigned. Two different parts of Windows
react to that, in two different ways, with two different fixes. Work out which
one you're looking at first — the symptoms are not alike.

| What you see | Which one | Jump to |
|---|---|---|
| Download fails, *"Virus detected"*, `Trojan:Win32/Wacatac.B!ml` | Defender antivirus | [Antivirus](#antivirus-false-positive) |
| Game won't start, *"Bad Image"*, `0xC0E90002` | Smart App Control | [Smart App Control](#smart-app-control) |

## Verify what you downloaded

Whatever else you do, check the file is the one we published. In PowerShell:

```powershell
Get-FileHash .\PEMF-0.2.3.zip -Algorithm SHA256
```

**Every release has its own hashes.** These are **0.2.3**, the current one:

| File | SHA256 |
|---|---|
| `PEMF-0.2.3.zip` | `1D7B3880B9489F4FB545F7A9384B6CD56AA6DBE5BBCD53BCC5344B5DA8546EC4` |
| `pemf_core.dll` | `E1A75858F7A37CF728387FC3620D9211E88ECB08697766E788DC2CA7EBFC7E71` |
| `version.dll` | `FE5687E05AB4B90EFE6FC3FCDAC76E1CEC8EF7F3B98D00687DE69286E1291389` |

<details>
<summary>Older releases</summary>

| Release | File | SHA256 |
|---|---|---|
| 0.2.0 | `PEMF-0.2.0.zip` | `AF66FB5F0581AF8321725C13DC1311C78FBB9756EE69C29E5C78F2E2080AF690` |
| 0.2.0 | `pemf_core.dll` | `4997313EA02A4496285A94F82AC17216031CCF7BA7559ED9F97A0625059AE8FE` |
| 0.2.0 | `version.dll` | `4A97CF1D1812CF1CEC5C639AB83FB62F2E3AFAF76FD7F284834BAB8208DD8175` |

</details>

If you are on a different release, check its hashes against that release's own
page rather than this table. If your hash matches nothing we have published,
**don't run it** — you didn't get it from us. Download again from the
[releases page](https://github.com/pemfofficial/PEMF/releases).

## Antivirus false positive

Microsoft Defender may refuse the download with **`Trojan:Win32/Wacatac.B!ml`**.

**It is a false positive.** The specifics matter here:

- The `!ml` suffix means a **machine-learning guess made in Microsoft's cloud**,
  not a match against a known piece of malware. `Wacatac` is Defender's generic
  bucket for "this looked unfamiliar."
- The exact published bytes scan **clean** against Defender's full local
  signature set. Nothing in the file is recognised as malicious. The verdict is
  produced at download time, by reputation and shape alone.

Three things about PEMF make that guess come out wrong, none of which are about
what the code does:

1. **It's unsigned** — see [below](#why-its-not-signed).
2. **Almost nobody has it.** Reputation systems score a file partly on how many
   machines have seen it. A release that's days old, on a few dozen PCs, has the
   same prevalence profile as freshly minted malware.
3. **A mod loader and an injector look identical from orbit.** PEMF proxies
   `version.dll`, hooks import-table entries, and patches a Direct3D vtable. That
   is how every PC game mod loader since the 2000s has worked — ReShade, SKSE,
   the ASI loaders — and it's also, structurally, what malicious injection looks
   like. A classifier that can't see intent sees the same instructions.

What PEMF does **not** do, any of which would be genuine cause for alarm: no
network access of any kind, no autostart or persistence, no injection into
processes other than the game you launched, no privilege escalation, no packing
or obfuscation, no anti-analysis. It reads and writes only inside your game
folder. The source is all here — read it, or build it yourself with
`.\build.ps1 -Package` and run your own build instead of ours.

⚠️ A rebuild will **not** produce our hash. MSVC stamps the build time into the
PE header, so the same source compiled twice gives two different files. Building
from source proves the source is honest; it can't prove our binary came from it.
The hashes above exist to confirm you got *our* published file intact, nothing
more.

A false-positive report has been filed with Microsoft. These usually clear in a
few days, and the detection should stop on its own.

### Getting it anyway

If you're satisfied it's safe — check the hash above first — allow it through:

**Windows Security** → Virus & threat protection → *Protection history* → find
the PEMF entry → **Actions** → **Allow**.

Then download again. If Defender already quarantined it, restore it from the
same screen.

You can instead add an exclusion for your game folder under **Virus & threat
protection settings** → *Exclusions*. That's a broader hole in your protection
and it stays open, so prefer the per-file allow.

Only ever do this for a file whose hash matches the table above.

## Smart App Control

A smaller number of Windows 11 machines refuse to **start the game** at all:

> `Pirates!.exe - Bad Image`
> `C:\...\VERSION.dll is either not designed to run on Windows or it contains an
> error. ... Error status 0xc0e90002`

The message is misleading — nothing is wrong with the DLL, the game, or your
install. `0xC0E90002` is `STATUS_SYSTEM_INTEGRITY_POLICY_VIOLATION`: **Smart App
Control** enforcing a kernel policy called `VerifiedAndReputableDesktop`, which
refuses to load unsigned code. You can confirm it in Event Viewer under
*Applications and Services Logs → Microsoft → Windows → CodeIntegrity →
Operational*, event **3077**:

```
File Name               ...\Sid Meier's Pirates\version.dll
Status                  0xc0e90002
PolicyName              VerifiedAndReputableDesktop     <- Smart App Control
Requested Signing Level 2
Validated Signing Level 1        <- unsigned
```

Only `version.dll` appears, because the process dies before `pemf_core.dll` is
ever reached.

**Most players will never see this.** Smart App Control only switches itself on
for *clean* Windows 11 installs using Defender, and it turns itself off
permanently the first time it gets in a user's way.

### The fix: turn Smart App Control off

There is no other fix. Signing is the only thing that satisfies this policy, and
PEMF isn't signed.

**Windows Security** → *App & browser control* → **Smart App Control settings**
→ **Off**.

⚠️ **This is a one-way door through the UI.** Microsoft does not let you turn
Smart App Control back on afterwards without reinstalling Windows. Decide
accordingly — and if you'd rather not, that's a completely reasonable call.
PEMF is a mod for a twenty-year-old game; it is not worth weakening a security
posture you want.

There is a widely used but **unsupported** registry route back:

```
HKLM\SYSTEM\CurrentControlSet\Control\CI\Policy\VerifiedAndReputablePolicyState
  0 = off   1 = enforced   2 = evaluation
```

Set it and reboot. Treat that as "probably works", not a guarantee.

## Why it's not signed

Code signing is what would fix the Smart App Control block outright and would
take most of the heat out of the antivirus problem.

We applied to the **SignPath Foundation**, which signs open-source projects at no
cost, and **were rejected**. Paid certificates run to hundreds of dollars a year,
which this project isn't going to spend.

So PEMF ships unsigned, and the honest position is: verify the hash, read the
source, and decide for yourself.

For completeness, `build.ps1` does support signing if a certificate ever appears:

```powershell
.\build.ps1 -Sign -CertThumbprint <sha1-thumbprint> -Package
.\build.ps1 -Sign -PfxPath key.pfx -PfxPassword <pw> -Package
```

It signs before packaging, prints a `sig:` column per artifact, and timestamps
against DigiCert's RFC3161 server by default — always timestamp, or the signature
dies with the certificate.

### What does not work

Recorded so nobody spends time on them:

- **Self-signed certificates.** Smart App Control does not consult locally
  installed trusted roots. Useful only for testing that the pipeline runs.
- **Version resources alone.** PEMF ships proper file metadata, which is worth
  having — blank fields are the worst possible profile for every reputation
  heuristic — but metadata is not a signature and clears neither block. This was
  tested: the metadata landed and event 3077 fired anyway, unchanged.
- **A launcher executable.** Smart App Control blocks unsigned code from running
  as a DLL *or* an EXE. An unsigned launcher can't start either.
- **Manual mapping** or hand-rolled loaders that bypass the Windows loader. They
  do dodge the check — and they're exactly what malware does to evade it, so you
  trade a rare honest "Windows blocked this" for a permanent "threat detected."
