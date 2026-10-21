# Prebuilt binary

`Glassy.exe` is a prebuilt 64-bit Windows executable of the current source, kept here so you can attach it to a GitHub Release by hand.
Normally you do not need to: pushing a version tag (for example `v1.0.0`) makes the release workflow build it and publish a Release automatically.

Verify a download:

```powershell
Get-FileHash .\Glassy.exe -Algorithm SHA256
```
