param([string]$ZigRoot, [switch]$Test, [string]$OutputDirectory)

$ErrorActionPreference = 'Stop'
$src = Join-Path $PSScriptRoot 'src'
$build = Join-Path $PSScriptRoot 'build'
$out = if ($OutputDirectory) { $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputDirectory) } else { $build }
$null = [IO.Directory]::CreateDirectory($out)
$sources = @('main.cpp', 'desktop.cpp', 'folder.cpp', 'shell.cpp') | ForEach-Object { Join-Path $src $_ }
$libs = @('user32', 'gdi32', 'comctl32', 'shell32', 'ole32', 'uuid', 'advapi32', 'shlwapi')
$exe = Join-Path $out 'DesktopJunction.exe'
$testExe = Join-Path $out 'FolderTests.exe'
$testSources = @((Join-Path $PSScriptRoot 'tests\folder_tests.cpp'), (Join-Path $src 'folder.cpp'))

function Invoke-BuildTool($tool, $arguments) {
    & $tool @arguments
    if ($LASTEXITCODE -ne 0) { throw "$tool exited with code $LASTEXITCODE" }
}

if ($ZigRoot) {
    $zig = Join-Path $ZigRoot 'zig.exe'
    $res = Join-Path $out 'app.res'
    $env:ZIG_GLOBAL_CACHE_DIR = Join-Path $build 'zig-cache'
    $env:ZIG_LOCAL_CACHE_DIR = Join-Path $build 'zig-local'
    $flags = @('c++', '-target', 'x86_64-windows-gnu', '-std=c++20', '-O2', '-Wall', '-Wextra',
        '-DUNICODE', '-D_UNICODE', '-DNOMINMAX')
    Invoke-BuildTool $zig @('rc', '/I', $src, '/FO', $res, (Join-Path $src 'app.rc'))
    $linkLibs = $libs | ForEach-Object { "-l$_" }
    Invoke-BuildTool $zig ($flags + @('-municode', '-Wl,--subsystem,windows', '-s') + $sources + @($res, '-o', $exe) + $linkLibs)
    if ($Test) { Invoke-BuildTool $zig ($flags + $testSources + @('-o', $testExe) + $linkLibs) }
} else {
    if (!(Get-Command cl.exe -ErrorAction SilentlyContinue) -or $env:VSCMD_ARG_TGT_ARCH -ne 'x64') {
        throw 'Use an x64 Native Tools Command Prompt for Visual Studio, or pass -ZigRoot.'
    }
    $res = Join-Path $out 'app.res'
    $flags = @('/nologo', '/std:c++20', '/utf-8', '/O2', '/MT', '/EHsc', '/W4',
        '/DUNICODE', '/D_UNICODE', '/DNOMINMAX', '/D_WIN32_WINNT=0x0A00')
    Invoke-BuildTool 'rc.exe' @('/nologo', '/i', $src, '/fo', $res, (Join-Path $src 'app.rc'))
    $linkLibs = $libs | ForEach-Object { "$_.lib" }
    Push-Location $out
    try {
        Invoke-BuildTool 'cl.exe' ($flags + $sources + @($res, "/Fe$exe", '/link', '/SUBSYSTEM:WINDOWS', '/MANIFEST:NO') + $linkLibs)
        if ($Test) { Invoke-BuildTool 'cl.exe' ($flags + $testSources + @("/Fe$testExe", '/link') + $linkLibs) }
    } finally { Pop-Location }
}
if ($Test) { Invoke-BuildTool $testExe @() }
Write-Output $exe
