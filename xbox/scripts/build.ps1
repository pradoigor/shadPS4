# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding()]
param([string]$SdkVersion = '10.0.22621.0')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$project = Split-Path $PSScriptRoot -Parent
$root = Split-Path $project -Parent
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (!(Test-Path $vswhere)) { throw 'Visual Studio Installer/vswhere not found.' }
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'Visual Studio C++ x64 tools are required.' }
$msbuild = Join-Path $vs 'MSBuild\Current\Bin\MSBuild.exe'
$sdk = "${env:ProgramFiles(x86)}\Windows Kits\10"
foreach ($path in @("$sdk\Include\$SdkVersion\um\windows.h", "$sdk\bin\$SdkVersion\x64\makeappx.exe", "$sdk\bin\$SdkVersion\x64\signtool.exe")) {
    if (!(Test-Path $path)) { throw "Required SDK component missing: $path" }
}
$storeLibs = @(Get-ChildItem "$vs\VC\Tools\MSVC\*\lib\x64\store\vcruntime.lib" -ErrorAction SilentlyContinue)
if (!$storeLibs.Count) { throw 'C++ UWP runtime libraries are missing. Install Microsoft.VisualStudio.ComponentGroup.UWP.VC.' }
$artifacts = Join-Path $project 'artifacts'
New-Item $artifacts -ItemType Directory -Force | Out-Null
$commit = (& git -C $root rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0) { throw 'Cannot identify git commit.' }
$commitInfo = @"
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#define XBOX_BUILD_COMMIT L"$commit"
#define XBOX_UPSTREAM_COMMIT L"42c555b7ab5d0678f531a7e4d505560ccc0f8add"
"@
Set-Content "$project\BuildInfo.h" $commitInfo -Encoding utf8
& nuget install Microsoft.Windows.CppWinRT -Version 2.0.250303.1 -OutputDirectory "$project\packages" -Source https://api.nuget.org/v3/index.json -NonInteractive
if ($LASTEXITCODE -ne 0) { throw 'C++/WinRT restore failed.' }
& python "$PSScriptRoot\generate_assets.py"
if ($LASTEXITCODE -ne 0) { throw 'Asset generation failed.' }
# Index only package assets, never the source tree, NuGet tools, or generated objects.
$priRoot = Join-Path $project 'obj\pri-assets'
New-Item $priRoot -ItemType Directory -Force | Out-Null
Copy-Item "$project\Assets" $priRoot -Recurse -Force
$binaryDir = Join-Path $project 'bin\x64\Release'
New-Item $binaryDir -ItemType Directory -Force | Out-Null
$makepri = "$sdk\bin\$SdkVersion\x64\makepri.exe"
& $makepri createconfig /cf "$project\obj\assets.priconfig.xml" /dq pt-BR /o
if ($LASTEXITCODE -ne 0) { throw 'PRI configuration failed.' }
& $makepri new /pr $priRoot /cf "$project\obj\assets.priconfig.xml" /of "$binaryDir\resources.pri" /in PradoIgor.ShadPS4Xbox.Diagnostics /o
if ($LASTEXITCODE -ne 0) { throw 'Asset resource indexing failed.' }
# Compile shaders before MSBuild evaluates its Content wildcard.
$fxc = "$sdk\bin\$SdkVersion\x64\fxc.exe"
foreach ($shader in @(@('TriangleVS', 'vs_5_0'), @('TrianglePS', 'ps_5_0'), @('ProbeCS', 'cs_5_0'))) {
    & $fxc /nologo /T $shader[1] /E main /Fo "$project\Shaders\$($shader[0]).cso" "$project\Shaders\$($shader[0]).hlsl"
    if ($LASTEXITCODE -ne 0) { throw "Shader compilation failed: $($shader[0])" }
}
& $msbuild "$project\Diagnostics.vcxproj" /m /p:Configuration=Release /p:Platform=x64 "/p:WindowsTargetPlatformVersion=$SdkVersion" /p:UapAppxPackageBuildMode=SideloadOnly /p:AppxBundle=Never /p:AppxPackageSigningEnabled=false "/bl:$artifacts\build.binlog"
if ($LASTEXITCODE -ne 0) { throw 'UWP build failed; see build.binlog.' }
$packages = @(Get-ChildItem "$project\AppPackages" -Recurse -Filter '*.appx' | Where-Object { $_.Name -like 'PradoIgor*' -or $_.Name -like 'Diagnostics*' -or $_.Name -like 'ShadPS4Xbox*' })
if ($packages.Count -ne 1) { throw "Expected one app package, found $($packages.Count). Use a clean build directory." }
# Private key exists only on this Windows build machine and is removed in finally.
$cert = New-SelfSignedCertificate -Type Custom -Subject 'CN=PradoIgor.ShadPS4Xbox' -FriendlyName 'shadPS4 Xbox Lab development' -KeyUsage DigitalSignature -KeyAlgorithm RSA -KeyLength 2048 -HashAlgorithm SHA256 -CertStoreLocation 'Cert:\CurrentUser\My' -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3', '2.5.29.19={text}') -NotAfter (Get-Date).AddMonths(6)
try {
    & "$sdk\bin\$SdkVersion\x64\signtool.exe" sign /fd SHA256 /sha1 $cert.Thumbprint $packages[0].FullName
    if ($LASTEXITCODE -ne 0) { throw 'APPX signing failed.' }
    # Self-signed development cert is not publicly trusted; signature integrity is checked by unpacking and deployment.
    Export-Certificate -Cert $cert -FilePath "$artifacts\development.cer" | Out-Null
    Copy-Item $packages[0].FullName "$artifacts\ShadPS4Xbox.appx"
} finally { Remove-Item "Cert:\CurrentUser\My\$($cert.Thumbprint)" -ErrorAction SilentlyContinue }
Get-ChildItem "$project\AppPackages" -Recurse -Directory -Filter Dependencies | ForEach-Object { Copy-Item $_.FullName "$artifacts\Dependencies" -Recurse -Force }
Copy-Item "$project\bin\x64\Release\*.pdb" $artifacts -ErrorAction SilentlyContinue
Get-ChildItem "$project\AppPackages" -Recurse -Filter '*.appxsym' | Copy-Item -Destination $artifacts
@{commit=$commit; upstream='42c555b7ab5d0678f531a7e4d505560ccc0f8add'; sdk=$SdkVersion; architecture='x64'; emulator_ported=$false; package_sha256=(Get-FileHash "$artifacts\ShadPS4Xbox.appx").Hash; built_at_utc=(Get-Date).ToUniversalTime().ToString('o')} | ConvertTo-Json | Set-Content "$artifacts\build-info.json"
Write-Host "Signed development package: $artifacts\ShadPS4Xbox.appx"
