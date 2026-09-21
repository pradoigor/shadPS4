$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64'
$out = "$root\xbox\obj\extractor-tests"
New-Item $out -ItemType Directory -Force | Out-Null
Push-Location $out
try {
    & python -m pip install cryptography==45.0.7
    if ($LASTEXITCODE) { throw 'Fixture dependency failed' }
    & python "$PSScriptRoot\make_pkg_fixtures.py" "$out\fixtures"
    if ($LASTEXITCODE) { throw 'Fixture generation failed' }
    & cl /nologo /std:c++20 /EHsc /W4 /utf-8 /DNOMINMAX /DMINIZ_NO_ARCHIVE_APIS /DMINIZ_NO_DEFLATE_APIS /DMINIZ_NO_STDIO "/I$root\xbox" "/I$root\externals\miniz" "$root\xbox\tests\pkg_tests.cpp" "$root\xbox\PkgExtractor.cpp" "$root\xbox\PkgCrypto.cpp" "$root\xbox\PkgBuiltinKeys.cpp" "$root\externals\miniz\miniz.c" "$root\externals\miniz\miniz_tinfl.c" /Fe:pkg_tests.exe /link bcrypt.lib
    if ($LASTEXITCODE) { throw 'Extractor test build failed' }
    $apollo = "$out\apollo-2.3.2.pkg"
    Invoke-WebRequest -UseBasicParsing -Uri 'https://github.com/bucanero/apollo-ps4/releases/download/v2.3.2/IV0000-APOL00004_00-APOLLO0000000PS4.pkg' -OutFile $apollo
    & .\pkg_tests.exe "$out\fixtures" $apollo | Tee-Object "$root\xbox\artifacts\extractor-tests.txt"
    if ($LASTEXITCODE) { throw 'Extractor tests failed' }
    $fixtureArtifact = "$root\xbox\artifacts\synthetic-test"
    New-Item $fixtureArtifact -ItemType Directory -Force | Out-Null
    Copy-Item "$out\fixtures\valid.pkg", "$out\fixtures\keys.json" $fixtureArtifact
    Set-Content "$fixtureArtifact\README.txt" 'Synthetic fixtures only. Random test RSA keys, not console keys. Package contains data, not a runnable PS4 program.'
} finally { Pop-Location }
