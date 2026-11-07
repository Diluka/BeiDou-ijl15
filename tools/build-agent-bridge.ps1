# Build the fixed source checkout with the Windows Visual Studio toolchain.
# The upstream project keeps its v142 default; this entry uses installed v143.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Set-Location (Join-Path $PSScriptRoot '..')

$commit = (& git rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0) { throw 'Cannot identify the source commit.' }
$tree = (& git rev-parse 'HEAD^{tree}').Trim()
if ($LASTEXITCODE -ne 0) { throw 'Cannot identify the source tree.' }
$dirty = & git status --porcelain --untracked-files=normal
if ($LASTEXITCODE -ne 0 -or $dirty) {
    throw 'Build requires a clean committed source checkout.'
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vsRoot = & $vswhere -latest -products '*' -version '[17.0,18.0)' `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($LASTEXITCODE -ne 0 -or -not $vsRoot) {
    throw 'Visual Studio 2022 with C++ x86/x64 tools is required.'
}
$msbuild = Join-Path $vsRoot 'MSBuild/Current/Bin/MSBuild.exe'
$msvcVersion = (Get-Content (Join-Path $vsRoot 'VC/Auxiliary/Build/Microsoft.VCToolsVersion.default.txt')).Trim()
$dumpbin = Join-Path $vsRoot "VC/Tools/MSVC/$msvcVersion/bin/Hostx64/x86/dumpbin.exe"
$sdkInclude = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits/10/Include'
$sdkVersion = Get-ChildItem $sdkInclude -Directory |
    Where-Object { $_.Name -match '^10\.0\.\d+\.\d+$' } |
    Sort-Object { [version]$_.Name } -Descending |
    Select-Object -First 1 -ExpandProperty Name
if (-not $sdkVersion) { throw 'Windows 10 SDK is required.' }
$msbuildVersion = ((& $msbuild -nologo -version) | Select-Object -Last 1).Trim()
if ($LASTEXITCODE -ne 0) { throw 'Cannot identify MSBuild.' }

New-Item -ItemType Directory -Force 'out' | Out-Null
# BOM-marked UTF-8 files keep their encoding; unmarked upstream headers are GBK.
# The client interprets narrow Chinese strings in CP936, including on this runner.
$previousCl = [Environment]::GetEnvironmentVariable('CL')
$env:CL = '/source-charset:.936 /execution-charset:.936'
try {
    & $msbuild ezorsia.sln /m /t:Rebuild /nologo `
        /p:Configuration=Release /p:Platform=x86 /p:PlatformToolset=v143 `
        "/p:WindowsTargetPlatformVersion=$sdkVersion" `
        /verbosity:minimal /fileLogger '/fileLoggerParameters:LogFile=out/build.log;Verbosity=normal'
    if ($LASTEXITCODE -ne 0) { throw "MSBuild failed with exit code $LASTEXITCODE." }
} finally {
    [Environment]::SetEnvironmentVariable('CL', $previousCl, 'Process')
}

$dll = 'out/Release/ijl15.dll'
$image = [IO.File]::ReadAllBytes((Join-Path (Get-Location) $dll))
if ($image.Length -lt 64 -or [BitConverter]::ToUInt16($image, 0) -ne 0x5A4D) {
    throw 'Build did not produce a Windows PE image.'
}
$peOffset = [BitConverter]::ToInt32($image, 0x3C)
if ($peOffset -lt 0 -or $peOffset + 26 -gt $image.Length `
    -or [BitConverter]::ToUInt32($image, $peOffset) -ne 0x00004550 `
    -or [BitConverter]::ToUInt16($image, $peOffset + 4) -ne 0x014C `
    -or ([BitConverter]::ToUInt16($image, $peOffset + 22) -band 0x2000) -eq 0 `
    -or [BitConverter]::ToUInt16($image, $peOffset + 24) -ne 0x010B) {
    throw 'Build artifact must be a Win32 x86 DLL.'
}
$exports = & $dumpbin /nologo /exports $dll
if ($LASTEXITCODE -ne 0) { throw 'Cannot inspect DLL exports.' }
foreach ($name in 'ijlGetLibVersion', 'ijlInit', 'ijlFree', 'ijlRead', 'ijlWrite', 'ijlErrorStr') {
    if (($exports -join "`n") -notmatch "\b$name\b") {
        throw "DLL is missing the upstream image-library export $name."
    }
}
$dependencies = & $dumpbin /nologo /dependents $dll
if ($LASTEXITCODE -ne 0) { throw 'Cannot inspect DLL dependencies.' }

$package = 'out/agent-bridge'
New-Item -ItemType Directory -Force $package | Out-Null
Copy-Item $dll "$package/ijl15.dll"
if (Test-Path 'out/Release/ijl15.pdb') {
    Copy-Item 'out/Release/ijl15.pdb' "$package/ijl15.pdb"
}
Copy-Item LICENSE "$package/LICENSE"
Copy-Item 'out/build.log' "$package/build.log"
$luaSource = Get-Content -Raw 'third_party/lua/source.json' | ConvertFrom-Json
New-Item -ItemType Directory -Force "$package/licenses/lua" | Out-Null
Copy-Item 'third_party/lua/LICENSE' "$package/licenses/lua/LICENSE"
Copy-Item 'third_party/lua/NOTICE.md' "$package/licenses/lua/NOTICE.md"
Copy-Item 'third_party/lua/source.json' "$package/licenses/lua/source.json"
$exports | Set-Content -Encoding utf8 "$package/exports.txt"
$dependencies | Set-Content -Encoding utf8 "$package/dependencies.txt"

$repository = if ($env:GITHUB_REPOSITORY) { $env:GITHUB_REPOSITORY } else { 'Diluka/BeiDou-ijl15' }
$manifest = [ordered]@{
    schema_version = 1
    source = [ordered]@{
        repository = $repository
        commit_sha = $commit
        tree_sha = $tree
        url = "https://github.com/$repository/tree/$commit"
        upstream_repository = 'BeiDouMS/BeiDou-ijl15'
        upstream_commit_sha = 'e559663b9d3bbf13acbc9e3a9499151f43109a47'
    }
    target = [ordered]@{
        executable = 'BeiDou.exe'
        executable_sha256 = '1198fa57ca5a7c489bae43ec13c69681d9cabe0f96762f3dc0357facf2e7d4df'
        proxy = '2ijl15.dll'
        proxy_sha256 = 'f991f9f77545b479758641dbf61c9bcd011008556ade775257b29e7282dc2558'
    }
    artifact = [ordered]@{
        file = 'ijl15.dll'
        sha256 = (Get-FileHash "$package/ijl15.dll" -Algorithm SHA256).Hash.ToLowerInvariant()
        machine = 'I386'
        configuration = 'Release'
    }
    toolchain = [ordered]@{
        platform_toolset = 'v143'
        msvc_version = $msvcVersion
        msbuild_version = $msbuildVersion
        windows_sdk_version = $sdkVersion
        source_charset_without_bom = 'CP936'
        execution_charset = 'CP936'
        detours_lib_sha256 = (Get-FileHash 'detours/detours.lib' -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    dependencies = [ordered]@{
        lua = [ordered]@{
            version = $luaSource.version
            source_url = $luaSource.url
            archive_sha256 = $luaSource.sha256
            license = 'MIT'
            linkage = 'compiled-as-C-into-ijl15'
        }
    }
    ci = [ordered]@{
        workflow = $env:GITHUB_WORKFLOW
        run_id = $env:GITHUB_RUN_ID
        run_attempt = $env:GITHUB_RUN_ATTEMPT
        image_os = $env:ImageOS
        image_version = $env:ImageVersion
    }
    built_at_utc = [DateTime]::UtcNow.ToString('o')
}
$manifest | ConvertTo-Json -Depth 5 | Set-Content -Encoding utf8 "$package/manifest.json"
Write-Host "Built Win32 ijl15.dll from $commit"
Write-Host "SHA256 $($manifest.artifact.sha256)"
