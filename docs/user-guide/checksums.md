# Check a download on Windows

Use the checksum file from the **same release** as your installer or ZIP. The alpha installer is unsigned; a matching checksum checks that the download matches the release's file.

1. Put the download and `SHA256SUMS.txt` in the same folder.
2. Open that folder in PowerShell. You can right-click the folder's empty space and choose **Open in Terminal**, then select PowerShell if needed.
3. Run the command below, replacing the example name with the exact filename you downloaded:

```powershell
Get-FileHash -Algorithm SHA256 '.\air-0.1.0-alpha.1-Windows-x64-Setup.exe'
```

4. Open `SHA256SUMS.txt`. Find the line for that filename and compare the entire 64-character hash. Uppercase and lowercase letters mean the same thing.

If they differ, keep the file closed and download it again from the release. If the checksum file is missing or you cannot establish the correct release, [ask for help](../SUPPORT.md).

A checksum verifies a file against the release record; it does not independently identify the publisher. Use the release linked by the project, not a checksum from an unrelated download site.
