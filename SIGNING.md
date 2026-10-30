# Code signing (removes Windows' "unknown publisher" / SmartScreen warning)

The CI signs the plugin DLL and the installer **automatically** as soon as two repository secrets exist:

| Secret | Value |
|---|---|
| `SIGN_PFX_BASE64` | your code-signing certificate (.pfx) as base64: `[Convert]::ToBase64String([IO.File]::ReadAllBytes("cert.pfx"))` in PowerShell |
| `SIGN_PFX_PASSWORD` | the .pfx password |

GitHub → repo **Settings → Secrets and variables → Actions → New repository secret**. No code changes needed.

## Getting a certificate
* **OV code-signing certificate** (Sectigo, SSL.com, Certum, …): roughly $200–400/year. Since 2023 these are issued on a
  hardware token / cloud HSM, so a plain .pfx export may not be possible — in that case the signing step must call the
  vendor's cloud signing tool instead of `signtool /f`. Tell us which vendor and we'll adapt the CI step.
* **Microsoft Trusted Signing (Azure Artifact Signing)**: low monthly cost, works great from CI, but requires identity
  validation (individuals: limited regions). Uses an Azure login instead of a .pfx; the CI step would use Microsoft's
  signing action.
* Note: a new certificate still builds SmartScreen "reputation" over time; signing removes "Unknown publisher" right away.
