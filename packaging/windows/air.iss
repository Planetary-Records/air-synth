; Internal preview installer recipe. No instrument compilation, download or publication.
; Build through tools/release_installer.py from a hash-verified preparation ZIP.
#ifndef StageDir
  #error StageDir must identify the verified payload directory
#endif
#ifndef PackageVersion
  #error PackageVersion must be supplied by the preparation tool
#endif
#ifndef EmbeddedVersion
  #error EmbeddedVersion must match the package manifest
#endif
#ifndef OutputDir
  #error OutputDir must identify a new private output directory
#endif

[Setup]
#ifdef IsolatedTestRoot
  #ifndef IsolatedTestId
    #error An isolated test must have a distinct uninstall identity
  #endif
AppId=Planetary.air.Windows.VST3.IsolatedTest.{#IsolatedTestId}
AppName=air (isolated installer test)
UninstallDisplayName=air (isolated installer test)
DefaultDirName={#IsolatedTestRoot}\app
PrivilegesRequired=lowest
OutputBaseFilename=air-{#PackageVersion}-Windows-x64-Isolated-Test-Setup
#else
AppId=Planetary.air.Windows.VST3
AppName=air
UninstallDisplayName=air (VST3)
DefaultDirName={commonpf64}\Planetary\air
PrivilegesRequired=admin
OutputBaseFilename=air-{#PackageVersion}-Windows-x64-Setup
#endif
AppVersion={#EmbeddedVersion}
AppVerName=air {#PackageVersion}
AppPublisher=Planetary Records
AppPublisherURL=https://planetary.music
DisableDirPage=yes
DisableProgramGroupPage=yes
ArchitecturesAllowed=x64compatible and not arm64
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir={#OutputDir}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
CloseApplications=no
RestartApplications=no
LicenseFile={#StageDir}\LICENSE
InfoBeforeFile={#StageDir}\INSTALLER_README.txt

[Files]
; Preserve the legacy bundle/module/companion names used by existing projects.
#ifdef IsolatedTestRoot
Source: "{#StageDir}\AI Rack Synth.vst3\*"; DestDir: "{#IsolatedTestRoot}\VST3\AI Rack Synth.vst3"; Flags: ignoreversion recursesubdirs createallsubdirs
#else
Source: "{#StageDir}\AI Rack Synth.vst3\*"; DestDir: "{commoncf64}\VST3\AI Rack Synth.vst3"; Flags: ignoreversion recursesubdirs createallsubdirs
#endif
Source: "{#StageDir}\*.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\release.json"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\licenses\*"; DestDir: "{app}\licenses"; Flags: ignoreversion recursesubdirs createallsubdirs

; No process termination, startup task, provider/model setup or user-data deletion.
; Inno's uninstall log removes only installed files; no broad UninstallDelete section.
