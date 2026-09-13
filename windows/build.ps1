param(
    [string]$CygwinRoot = 'C:\cygwin64',
    [string]$Python = 'python',
    [string]$ISCC = '',
    [string]$Version = '',
    [switch]$SkipDependencies
)
$ErrorActionPreference = 'Stop'
Set-Location (Split-Path $PSScriptRoot -Parent)
$env:CAMERA_CI_ROOT = (Get-Location).Path
if (-not $Version) {
    $Version = (& git describe --tags --always).Trim()
    if ($LASTEXITCODE -ne 0) { throw 'Supply -Version when building a source archive without Git metadata' }
}
function Run-Cygwin([string]$Command) {
    $script = Join-Path $env:CAMERA_CI_ROOT 'build/windows/cygwin-step.sh'
    New-Item -ItemType Directory -Force (Split-Path $script) | Out-Null
    # A script file avoids the different native argument quoting rules in
    # Windows PowerShell 5 and PowerShell 7, including checkout paths with spaces.
    $contents = 'set -eu' + "`n" + 'cd "$(cygpath -u "$CAMERA_CI_ROOT")"' + "`n" + $Command + "`n"
    [System.IO.File]::WriteAllText($script, $contents, (New-Object System.Text.UTF8Encoding($false)))
    & "$CygwinRoot\bin\bash.exe" --login $script
    if ($LASTEXITCODE -ne 0) { throw "Cygwin build failed ($LASTEXITCODE)" }
}
if (-not $SkipDependencies) {
    & $Python -m pip install -r windows/requirements.txt
    if ($LASTEXITCODE -ne 0) { throw 'Python dependency installation failed' }
    # This revision contains the map3d terrain code and video camera-pose helper.
    & $Python -m pip install --no-deps --force-reinstall 'MAVProxy @ git+https://github.com/tridge/MAVProxy.git@718f420d58b242aff9c4cd9940a76fa7b0c0b2e1'
    if ($LASTEXITCODE -ne 0) { throw 'MAVProxy installation failed' }
    Run-Cygwin 'make dependencies'
}
Run-Cygwin 'bash windows/build_native.sh'
& $Python sitl/terrain_video.py --check
if ($LASTEXITCODE -ne 0) { throw 'Terrain renderer dependencies are incomplete' }
& $Python windows/prepare_payload.py
if ($LASTEXITCODE -ne 0) { throw 'Payload preparation failed' }
& $Python -m PyInstaller --noconfirm --clean windows/camera_sitl.spec
if ($LASTEXITCODE -ne 0) { throw 'PyInstaller failed' }
if (-not $ISCC) {
    $installed = Get-Command ISCC.exe -ErrorAction SilentlyContinue
    if ($installed) { $ISCC = $installed.Source }
    else {
        $installer = Join-Path $env:CAMERA_CI_ROOT 'build\windows\innosetup.exe'
        Invoke-WebRequest 'https://github.com/jrsoftware/issrc/releases/download/is-6_7_1/innosetup-6.7.1.exe' -OutFile $installer
        $destination = Join-Path $env:CAMERA_CI_ROOT 'build\windows\inno'
        $setup = Start-Process $installer -ArgumentList @('/verysilent', '/suppressmsgboxes', '/norestart', '/currentuser', "/dir=`"$destination`"") -Wait -PassThru
        if ($setup.ExitCode -ne 0) { throw 'Inno Setup installation failed' }
        $ISCC = Join-Path $destination 'ISCC.exe'
    }
}
Copy-Item windows/README.md dist/CameraGimbalSITL/README.md
& $ISCC "/dMyAppVersion=$Version" windows/camera_sitl.iss
if ($LASTEXITCODE -ne 0) { throw 'Installer compilation failed' }
Compress-Archive -Path dist/CameraGimbalSITL -DestinationPath windows/Output/CameraGimbalSITL-Portable.zip -Force
Write-Host 'Windows installer and portable ZIP are in windows/Output'
