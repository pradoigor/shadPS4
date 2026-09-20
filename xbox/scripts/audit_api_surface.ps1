# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding()]
param([string]$SdkVersion = '10.0.22621.0')
$ErrorActionPreference = 'Stop'
$project = Split-Path $PSScriptRoot -Parent
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'No C++ toolchain available for API audit.' }
$msbuild = Join-Path $vs 'MSBuild\Current\Bin\MSBuild.exe'
$directory = Join-Path $project 'obj\api-audit'
New-Item $directory -ItemType Directory -Force | Out-Null
$out = Join-Path $project 'artifacts\api-surface'
New-Item $out -ItemType Directory -Force | Out-Null
$probes = [ordered]@{
  baseline = '(void)GetCurrentProcess();'
  VirtualAlloc2 = '(void)VirtualAlloc2(GetCurrentProcess(), nullptr, 65536, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);'
  CreateFileMapping2 = '(void)CreateFileMapping2(INVALID_HANDLE_VALUE, nullptr, FILE_MAP_ALL_ACCESS, PAGE_EXECUTE_READWRITE, SEC_COMMIT, 65536, nullptr, nullptr, 0);'
  MapViewOfFile3 = '(void)MapViewOfFile3(nullptr, GetCurrentProcess(), nullptr, 0, 65536, MEM_REPLACE_PLACEHOLDER, PAGE_EXECUTE_READWRITE, nullptr, 0);'
  VirtualProtect = 'DWORD old{}; (void)VirtualProtect(nullptr, 4096, PAGE_EXECUTE_READ, &old);'
  AddVectoredExceptionHandler = '(void)AddVectoredExceptionHandler(1, nullptr);'
  RtlGetVersionLookup = '(void)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");'
}
$results = @()
foreach ($entry in $probes.GetEnumerator()) {
  $name = $entry.Key
  Set-Content "$directory\$name.cpp" "#include <windows.h>`n#include <memoryapi.h>`nint __stdcall wWinMain(HINSTANCE,HINSTANCE,PWSTR,int) { $($entry.Value) return 0; }"
  $xml = @"
<Project DefaultTargets="Build" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
<ItemGroup Label="ProjectConfigurations"><ProjectConfiguration Include="Release|x64"><Configuration>Release</Configuration><Platform>x64</Platform></ProjectConfiguration></ItemGroup>
<PropertyGroup Label="Globals"><ProjectGuid>{F716BB79-0EA2-443D-9E71-1D6F72870AD8}</ProjectGuid><AppContainerApplication>true</AppContainerApplication><ApplicationType>Windows Store</ApplicationType><ApplicationTypeRevision>10.0</ApplicationTypeRevision><WindowsTargetPlatformVersion>$SdkVersion</WindowsTargetPlatformVersion><WindowsTargetPlatformMinVersion>10.0.19041.0</WindowsTargetPlatformMinVersion></PropertyGroup>
<Import Project="`$(VCTargetsPath)\Microsoft.Cpp.Default.props" />
<PropertyGroup Label="Configuration"><ConfigurationType>Application</ConfigurationType><PlatformToolset>v143</PlatformToolset><UseDebugLibraries>false</UseDebugLibraries></PropertyGroup>
<Import Project="`$(VCTargetsPath)\Microsoft.Cpp.props" />
<PropertyGroup><OutDir>$directory\$name\</OutDir><IntDir>$directory\$name\obj\</IntDir><GenerateAppxPackageOnBuild>false</GenerateAppxPackageOnBuild><AppxPackage>false</AppxPackage></PropertyGroup>
<ItemDefinitionGroup><ClCompile><PrecompiledHeader>NotUsing</PrecompiledHeader><PreprocessorDefinitions>WIN32_LEAN_AND_MEAN;NOMINMAX</PreprocessorDefinitions></ClCompile><Link><SubSystem>Windows</SubSystem><GenerateWindowsMetadata>false</GenerateWindowsMetadata><AdditionalDependencies>WindowsApp.lib;%(AdditionalDependencies)</AdditionalDependencies></Link></ItemDefinitionGroup>
<ItemGroup><ClCompile Include="$name.cpp" /></ItemGroup><Import Project="`$(VCTargetsPath)\Microsoft.Cpp.targets" />
</Project>
"@
  Set-Content "$directory\$name.vcxproj" $xml
  & $msbuild "$directory\$name.vcxproj" /t:Link /p:Configuration=Release /p:Platform=x64 /verbosity:minimal *> "$out\$name.log"
  $passed = $LASTEXITCODE -eq 0
  $results += @{api=$name; compiles_and_links=$passed; log="api-surface/$name.log"; runtime_tested=$false}
  if ($name -eq 'baseline' -and !$passed) {
    $results | ConvertTo-Json -Depth 5 | Set-Content "$project\artifacts\api-surface.json"
    throw 'API audit baseline failed; do not interpret subsequent APIs as unavailable.'
  }
}
$results | ConvertTo-Json -Depth 5 | Set-Content "$project\artifacts\api-surface.json"
Write-Host 'API audit completed. Results indicate compilation/linking only, not Xbox runtime support.'
