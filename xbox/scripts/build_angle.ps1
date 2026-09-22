$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$angleCommit = 'bec384b13132ec5768cb42cdfe0795a67f8860b5'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$work = Join-Path $env:RUNNER_TEMP 'shadps4-angle'
$tools = Join-Path $work 'depot_tools'
$source = Join-Path $work 'angle'
$output = Join-Path $root 'xbox\angle-artifacts'
New-Item -ItemType Directory -Force $work, $output | Out-Null

$env:DEPOT_TOOLS_WIN_TOOLCHAIN = '0'
$env:PATH = "$tools;$env:PATH"
git clone --filter=blob:none https://chromium.googlesource.com/chromium/tools/depot_tools.git $tools
if ($LASTEXITCODE -ne 0) { throw 'Falha ao obter depot_tools.' }
# gclient_scm.py invokes git.bat on Windows; a fresh depot_tools clone does
# not contain that generated shim yet. Use the runner's Git for Windows.
$gitShim = Join-Path $tools 'git.bat'
if (-not (Test-Path $gitShim)) {
    Set-Content $gitShim "@echo off`r`ngit.exe %*`r`n" -Encoding ascii
}
git clone --filter=blob:none https://chromium.googlesource.com/angle/angle $source
if ($LASTEXITCODE -ne 0) { throw 'Falha ao obter ANGLE.' }
git -C $source checkout --detach $angleCommit
if ($LASTEXITCODE -ne 0) { throw 'Commit ANGLE indisponível.' }

Push-Location $source
try {
    python scripts/bootstrap.py
    if ($LASTEXITCODE -ne 0) { throw 'Bootstrap ANGLE falhou.' }
    gclient sync --no-history
    if ($LASTEXITCODE -ne 0) { throw 'Sincronização ANGLE falhou.' }
    $gnArgs = 'target_os="winuwp" target_cpu="x64" is_component_build=false is_clang=false is_debug=false angle_enable_d3d11=true angle_enable_vulkan=false'
    gn gen out/uwp --args=$gnArgs
    if ($LASTEXITCODE -ne 0) { throw 'Geração GN falhou.' }
    autoninja -C out/uwp libEGL libGLESv2
    if ($LASTEXITCODE -ne 0) { throw 'Compilação ANGLE falhou.' }
    foreach ($file in @('libEGL.dll', 'libGLESv2.dll', 'libEGL.lib', 'libGLESv2.lib')) {
        $built = Join-Path $source "out\uwp\$file"
        if (-not (Test-Path $built)) { throw "Artefato ausente: $file" }
        Copy-Item $built $output
    }
    Copy-Item LICENSE $output
    Copy-Item -Recurse include (Join-Path $output 'include')
    $manifest = [ordered]@{
        angle_commit = $angleCommit
        target_os = 'winuwp'
        target_cpu = 'x64'
        renderer = 'd3d11'
        gn_args = $gnArgs
        binaries = @{}
    }
    foreach ($file in @('libEGL.dll', 'libGLESv2.dll')) {
        $manifest.binaries[$file] = (Get-FileHash (Join-Path $output $file) -Algorithm SHA256).Hash
    }
    $manifest | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $output 'build-info.json') -Encoding utf8
} finally {
    Pop-Location
}
