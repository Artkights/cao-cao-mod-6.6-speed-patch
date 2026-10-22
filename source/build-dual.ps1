param([switch]$Tests, [switch]$Publish, [string[]]$TestNames = @())
$ErrorActionPreference = 'Stop'
$sourceRoot = $PSScriptRoot
$buildRoot = Join-Path $sourceRoot 'build'
$vcvars = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat'
if (!(Test-Path -LiteralPath $vcvars)) { throw 'Visual Studio 2022 x86 toolchain not found.' }
$toolEnvironment = & $env:ComSpec /d /c ('call "{0}" >nul && set' -f $vcvars)
if ($LASTEXITCODE) { throw 'Unable to initialize the x86 toolchain.' }
foreach ($line in $toolEnvironment) {
    $separator = $line.IndexOf('=')
    if ($separator -gt 0) {
        [Environment]::SetEnvironmentVariable($line.Substring(0, $separator), $line.Substring($separator + 1), 'Process')
    }
}
function Invoke-Native([string]$Program, [string[]]$ToolArguments) {
    & $Program @ToolArguments
    if ($LASTEXITCODE) { throw "$Program failed with exit code $LASTEXITCODE" }
}
function Source-Paths([string[]]$Names) {
    @($Names | ForEach-Object { Join-Path $sourceRoot $_ })
}
$compilerOptions = @('/nologo','/O2','/W4','/MT','/EHsc','/utf-8','/std:c++17','/DUNICODE','/D_UNICODE')
$libraries = @('user32.lib','comctl32.lib','gdi32.lib','winmm.lib','shell32.lib','bcrypt.lib','ole32.lib','oleaut32.lib')
$installSources = @('InstallCommon.cpp','EngineProfile.cpp','EngineLayout.cpp')
$featureSources = @('Guide.cpp','Features.cpp','FeatureUi.cpp','BattleRuntime.cpp','BattleInfo.cpp','SaveEnhancer.cpp','SaveTransactions.cpp','ScenarioLifecycle.cpp','BattleStats.cpp','BattleStatsPersistence.cpp')
$runtimeSources = @('Proxy.cpp','Engine.cpp') + $installSources + @('Ui.cpp') + $featureSources
$commonRoot = Join-Path $buildRoot 'common'
New-Item -ItemType Directory -Force -Path $commonRoot | Out-Null
Push-Location $commonRoot
try {
    Invoke-Native 'cl.exe' ($compilerOptions + (Source-Paths (@('UninstallWorker.cpp') + $installSources)) + @('/Fe:UninstallWorker.exe','/link','/SUBSYSTEM:WINDOWS') + $libraries)
    Invoke-Native 'cl.exe' ($compilerOptions + @('/LD') + (Source-Paths @('GearNtKe.cpp')) + @('/Fe:GearNtKe.dll','/link','/BASE:0x64000000','/DYNAMICBASE',('/DEF:' + (Join-Path $sourceRoot 'GearNtKe.def'))) + $libraries)
} finally { Pop-Location }

foreach ($generation in @('65','66')) {
    $targetRoot = Join-Path $buildRoot $generation
    New-Item -ItemType Directory -Force -Path $targetRoot | Out-Null
    foreach ($resource in @('Ui.rc','UiIds.h','ResourceIds.h','Doro-white.ico')) {
        Copy-Item -LiteralPath (Join-Path $sourceRoot $resource) -Destination $targetRoot -Force
    }
    Copy-Item -LiteralPath (Join-Path $commonRoot 'UninstallWorker.exe') -Destination $targetRoot -Force
    Copy-Item -LiteralPath (Join-Path $commonRoot 'GearNtKe.dll') -Destination $targetRoot -Force
    $generationOptions = @('/DCCZ_ENGINE' + $generation)
    Push-Location $targetRoot
    try {
        Invoke-Native 'rc.exe' @('/nologo','/fo','Ui.res','Ui.rc')
        Invoke-Native 'cl.exe' ($compilerOptions + $generationOptions + @('/LD') + (Source-Paths $runtimeSources) + @('Ui.res','/Fe:Koeicda.dll','/link','/BASE:0x63000000','/DYNAMICBASE','/MAP:Koeicda.map') + $libraries)
        if ($Tests) {
            $testSets = @(
                @{Name='EngineTests'; Files=@('Engine.cpp','EngineLayout.cpp','Features.cpp','EngineTests.cpp')},
                @{Name='LayoutTests'; Files=@('Engine.cpp','EngineLayout.cpp','Features.cpp','LayoutTests.cpp')},
                @{Name='EngineProfileTests'; Files=@('EngineProfileTests.cpp','EngineProfile.cpp','EngineLayout.cpp')},
                @{Name='ConfigTests'; Files=@('ConfigTests.cpp') + $installSources + $featureSources; Resource=$true},
                @{Name='InstallerTests'; Files=@('InstallerTests.cpp') + $installSources},
                @{Name='InstallerProfileTests'; Files=@('InstallerProfileTests.cpp') + $installSources},
                @{Name='UninstallWorkerCompatibilityTests'; Files=@('UninstallWorkerCompatibilityTests.cpp') + $installSources},
                @{Name='IconResourceTests'; Files=@('IconResourceTests.cpp')},
                @{Name='UiIconTests'; Files=@('UiIconTests.cpp','Ui.cpp'); Resource=$true},
                @{Name='GuideTests'; Files=@('Guide.cpp','BattleRuntime.cpp','GuideTests.cpp'); Resource=$true},
                @{Name='BattleRuntimeTests'; Files=@('BattleRuntimeTests.cpp','BattleRuntime.cpp')},
                @{Name='BattleStatsTests'; Files=@('BattleStatsTests.cpp','BattleStats.cpp','BattleRuntime.cpp')},
                @{Name='BattleStatsPersistenceTests'; Files=@('BattleStatsPersistenceTests.cpp','BattleStatsPersistence.cpp') + $installSources},
                @{Name='ScenarioLifecycleTests'; Files=@('ScenarioLifecycleTests.cpp','ScenarioLifecycle.cpp')},
                @{Name='SaveTransactionsTests'; Files=@('SaveTransactionsTests.cpp','SaveTransactions.cpp','BattleStats.cpp','BattleStatsPersistence.cpp','BattleRuntime.cpp') + $installSources},
                @{Name='SaveEnhancerTests'; Files=@('SaveEnhancerTests.cpp','SaveEnhancer.cpp','SaveTransactions.cpp','ScenarioLifecycle.cpp','BattleStats.cpp','BattleStatsPersistence.cpp','BattleRuntime.cpp','Guide.cpp','Features.cpp') + $installSources; Resource=$true},
                @{Name='AudioExportTests'; Files=@('AudioExportTests.cpp')},
                @{Name='GearTests'; Files=@('GearTests.cpp')},
                @{Name='MenuUninstallHarness'; Files=@('MenuUninstallHarness.cpp')},
                @{Name='InstallerLocationHarness'; Files=@('InstallerLocationHarness.cpp')}
            )
            foreach ($test in $testSets) {
                if ($TestNames.Count -and $test.Name -notin $TestNames) { continue }
                $testArguments = $compilerOptions + $generationOptions + @('/DXA66_TEST','/WX') + (Source-Paths $test.Files)
                if ($test.Resource) { $testArguments += 'Ui.res' }
                Invoke-Native 'cl.exe' ($testArguments + @(('/Fe:' + $test.Name + '.exe'),'/link') + $libraries)
            }
        }
    } finally { Pop-Location }
}
$packageRoot = Join-Path $buildRoot 'package'
New-Item -ItemType Directory -Force -Path $packageRoot | Out-Null
foreach ($resource in @('Installer.rc','ResourceIds.h','Doro-white.ico','pl_06.ini','features.ini')) {
    Copy-Item -LiteralPath (Join-Path $sourceRoot $resource) -Destination $packageRoot -Force
}
Copy-Item -LiteralPath (Join-Path $buildRoot '65\Koeicda.dll') -Destination (Join-Path $packageRoot 'Koeicda65.dll') -Force
Copy-Item -LiteralPath (Join-Path $buildRoot '66\Koeicda.dll') -Destination $packageRoot -Force
Copy-Item -LiteralPath (Join-Path $commonRoot 'GearNtKe.dll') -Destination $packageRoot -Force
Push-Location $packageRoot
try {
    Invoke-Native 'rc.exe' @('/nologo','/fo','Installer.res','Installer.rc')
    Invoke-Native 'cl.exe' ($compilerOptions + (Source-Paths (@('Installer.cpp') + $installSources)) + @('Installer.res','/Fe:加速补丁安装器.exe','/link','/SUBSYSTEM:WINDOWS') + $libraries)
} finally { Pop-Location }
if ($Publish) {
    $releaseRoot = Join-Path (Split-Path (Split-Path $sourceRoot -Parent) -Parent) '加速补丁'
    New-Item -ItemType Directory -Force -Path $releaseRoot | Out-Null
    Copy-Item -LiteralPath (Join-Path $packageRoot '加速补丁安装器.exe') -Destination $releaseRoot -Force
    Write-Output "Published: $releaseRoot"
}
Write-Output "Build complete: $packageRoot"
