**Windows says PEMF is a virus. It isn't — here's the fix.**

Defender may block `PEMF-0.2.0.zip` as `Trojan:Win32/Wacatac.B!ml`. It's a false positive. We've reported it to Microsoft, and it should clear on its own in a few days.

**Why it happens**
PEMF is unsigned — we applied for free open-source code signing and were turned down. So what you have is a brand-new, unsigned file that hooks into a game process. That's how every PC game mod has worked for twenty years, and it's also exactly the shape Defender's machine learning is trained to distrust. The `!ml` on the end of that name means it's a cloud guess, not a match against a known virus. Scanned against Defender's actual signature list, the file comes back clean.

**Getting it anyway**
1. Try GitHub first — it often works where Discord doesn't:
<https://github.com/pemfofficial/PEMF/releases/latest>
2. Still blocked? **Windows Security → Virus & threat protection → Protection history →** find PEMF **→ Actions → Allow**. Then download again.
3. Check you got the real file. In PowerShell:
`Get-FileHash .\PEMF-0.2.0.zip -Algorithm SHA256`
Should be `AF66FB5F0581AF8321725C13DC1311C78FBB9756EE69C29E5C78F2E2080AF690`

Don't skip step 3. We can't sign the files, so that hash is the only way to tell our build from someone else's.

For the record: no network code, no persistence, no autostart, nothing touched outside your game folder. Every line of source is public.

**Different problem: the game won't start**
If Pirates! itself fails with `Bad Image … 0xC0E90002`, that's **Smart App Control**, not antivirus. The only fix is turning it off — Windows Security → App & browser control → Smart App Control settings → Off.
⚠️ You can't turn it back on again without reinstalling Windows. If you'd rather not, that's a fair call.

Full write-up: <https://github.com/pemfofficial/PEMF/blob/main/docs/WINDOWS_SECURITY.md>
