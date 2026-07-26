# Code signing PEMF

## The problem

Windows 11's **Smart App Control** (SAC) enforces a kernel code-integrity policy
called `VerifiedAndReputableDesktop`. It refuses to load unsigned DLLs. When it
blocks PEMF the user sees a misleading generic dialog:

> `Pirates!.exe - Bad Image`
> `C:\...\VERSION.dll is either not designed to run on Windows or it contains an
> error. ... Error status 0xc0e90002`

`0xC0E90002` is `STATUS_SYSTEM_INTEGRITY_POLICY_VIOLATION`. Nothing is wrong with
the DLL, the game, or the install — the machine's policy simply refused it.

Confirmed on 2026-07-26, reproduced locally by enabling SAC:

```
Event Viewer -> Applications and Services Logs -> Microsoft -> Windows
            -> CodeIntegrity -> Operational
```

```
Event ID 3077
File Name               ...\Sid Meier's Pirates\version.dll
Status                  0xc0e90002
PolicyName              VerifiedAndReputableDesktop     <- Smart App Control
Requested Signing Level 2
Validated Signing Level 1        <- unsigned
```

Only `version.dll` appears in the log because the process dies before
`pemf_core.dll` is ever reached. **Both DLLs need signing.**

## Who this affects

Not everyone. SAC only turns itself on for *clean* Windows 11 installs using
Defender, and it disables itself permanently the first time it gets in a user's
way. Most players have it off and have never heard of it. Measure the real hit
rate across testers before treating this as urgent.

## Why other mods don't hit it

ReShade, SKSE, Ultimate ASI Loader and friends are mostly unsigned too. They
survive on **reputation** — the byte-identical file exists on millions of
machines and Microsoft's cloud has scored it safe. PEMF is rebuilt constantly and
exists on a handful of PCs. Same technique, opposite reputation.

## The fix

### Free: SignPath Foundation

<https://signpath.io/solutions/open-source-community> signs open-source projects
at no cost. An OV certificate, private key held in their HSM (you never touch
it), signing wired into CI.

Conditions: OSI-approved licence with no commercial dual-licensing, actively
maintained, already publicly released.

### Important caveat

The policy is `Verified` **and** `Reputable`. Signing gives you *verified*.
*Reputable* accrues as downloads happen. A freshly signed build can still be
blocked for a while. Only an EV certificate (paid, hardware token) buys immediate
trust.

So: signing is necessary. It may not be sufficient on day one.

### What does NOT work

- **Self-signed certificates.** SAC does not consult locally installed trusted
  roots. Useful only for testing that the pipeline runs.
- **Version resources alone.** PEMF now ships proper metadata (see `proxy.rc` /
  `core.rc`) — worth having, since blank fields are the worst possible profile
  for every reputation heuristic — but metadata is not a signature and will not
  clear a code-integrity block on its own.
- **Manual mapping / hand-rolled loaders** that bypass the Windows loader. They
  work, but they are what malware does to evade exactly this control, so you
  trade a rare honest "Windows blocked this" for frequent "threat detected".

## Signing a build

Once a certificate exists:

```powershell
# certificate in the current user's store
.\build.ps1 -Sign -CertThumbprint <sha1-thumbprint> -Package

# or a PFX file
.\build.ps1 -Sign -PfxPath key.pfx -PfxPassword <pw> -Package
```

The build prints a `sig:` column per artifact; `sig:Valid` means it took.
Signing happens *before* packaging and deployment, so the zip carries signed
binaries.

Timestamping defaults to DigiCert's RFC3161 server and can be overridden with
`-TimestampUrl`. Always timestamp — without it the signature dies with the
certificate.

## Reproducing the block locally

⚠️ **Turning SAC off is irreversible** through the UI — Microsoft requires a
Windows reinstall to re-enable it. There is a widely used but *unsupported*
registry route back:

```
HKLM\SYSTEM\CurrentControlSet\Control\CI\Policy\VerifiedAndReputablePolicyState
  0 = off   1 = enforced   2 = evaluation
```

Set to `1` and reboot. Treat as "probably works", not a guarantee.
