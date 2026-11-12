; Instalador de Windows para el emulador de Game Boy, NES y Super Nintendo (plumnin).
; Se compila con Inno Setup 6:
;   iscc /DAppVersion=0.1.0 /DSourceDir=..\dist installer\plumnin.iss
; SourceDir debe contener el ejecutable (AppExeName) y SDL2.dll.

#ifndef AppName
  #define AppName "plumnin"
#endif
#ifndef AppVersion
  #define AppVersion "0.0.0-dev"
#endif
#ifndef AppExeName
  #define AppExeName "plumnin.exe"
#endif
#ifndef SourceDir
  #define SourceDir "..\dist"
#endif
#ifndef OutputDir
  #define OutputDir "..\dist-installer"
#endif
#define AppPublisher "cochimbo"
#define AppURL "https://github.com/cochimbo/plumnin"

[Setup]
; AppId identifica la instalación para actualizaciones y desinstalación: no cambiar.
AppId={{3DBBB852-3220-40D1-AE2C-9EA9CE4586A7}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppURL}
AppSupportURL={#AppURL}/issues
AppUpdatesURL={#AppURL}/releases
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
UninstallDisplayIcon={app}\{#AppExeName}
OutputDir={#OutputDir}
OutputBaseFilename={#AppName}-{#AppVersion}-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
; La ciruela y la marquesina en la columna izquierda de las páginas de bienvenida y final,
; y la ciruela arriba a la derecha en las demás (100% y 200%).
WizardImageFile=wizard.bmp,wizard-2x.bmp
WizardSmallImageFile=wizard-small.bmp,wizard-small-2x.bmp
; El icono del propio instalador (plumnin-<versión>-setup.exe).
SetupIconFile=..\src\frontend\assets\plumnin.ico
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Permite instalar sin permisos de administrador (en el perfil del usuario).
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ChangesAssociations=yes

[Languages]
Name: "spanish"; MessagesFile: "compiler:Languages\Spanish.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[CustomMessages]
spanish.AssocRom=Añadir {#AppName} a "Abrir con" para las ROMs de Game Boy, NES y Super Nintendo (.gb, .gbc, .nes, .sfc, .smc, .zip)
english.AssocRom=Add {#AppName} to "Open with" for Game Boy, NES and Super Nintendo ROMs (.gb, .gbc, .nes, .sfc, .smc, .zip)
spanish.RomFileDesc=ROM de consola Nintendo
english.RomFileDesc=Nintendo console ROM

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "assocrom"; Description: "{cm:AssocRom}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#SourceDir}\{#AppExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\SDL2.dll"; DestDir: "{app}"; Flags: ignoreversion
; Cualquier otra DLL o recurso que acompañe al ejecutable.
Source: "{#SourceDir}\*"; DestDir: "{app}"; Excludes: "{#AppExeName},SDL2.dll,*.pdb,*.ilk,*.lib,*.exp"; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExeName}"
Name: "{group}\{cm:UninstallProgram,{#AppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExeName}"; Tasks: desktopicon

[Registry]
; Las extensiones solo se añaden a "Abrir con", sin quitar el programa que las abre por
; defecto (.zip son muchas otras cosas). Se
; escriben en HKCU o HKLM según el modo de instalación.
Root: HKA; Subkey: "Software\Classes\.gb\OpenWithProgids"; ValueType: string; ValueName: "plumnin.rom"; ValueData: ""; Flags: uninsdeletevalue; Tasks: assocrom
Root: HKA; Subkey: "Software\Classes\.gbc\OpenWithProgids"; ValueType: string; ValueName: "plumnin.rom"; ValueData: ""; Flags: uninsdeletevalue; Tasks: assocrom
Root: HKA; Subkey: "Software\Classes\.nes\OpenWithProgids"; ValueType: string; ValueName: "plumnin.rom"; ValueData: ""; Flags: uninsdeletevalue; Tasks: assocrom
Root: HKA; Subkey: "Software\Classes\.sfc\OpenWithProgids"; ValueType: string; ValueName: "plumnin.rom"; ValueData: ""; Flags: uninsdeletevalue; Tasks: assocrom
Root: HKA; Subkey: "Software\Classes\.smc\OpenWithProgids"; ValueType: string; ValueName: "plumnin.rom"; ValueData: ""; Flags: uninsdeletevalue; Tasks: assocrom
Root: HKA; Subkey: "Software\Classes\.zip\OpenWithProgids"; ValueType: string; ValueName: "plumnin.rom"; ValueData: ""; Flags: uninsdeletevalue; Tasks: assocrom
Root: HKA; Subkey: "Software\Classes\plumnin.rom"; ValueType: string; ValueName: ""; ValueData: "{cm:RomFileDesc}"; Flags: uninsdeletekey; Tasks: assocrom
Root: HKA; Subkey: "Software\Classes\plumnin.rom\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#AppExeName},0"; Tasks: assocrom
Root: HKA; Subkey: "Software\Classes\plumnin.rom\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#AppExeName}"" ""%1"""; Tasks: assocrom

[Run]
Filename: "{app}\{#AppExeName}"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent
