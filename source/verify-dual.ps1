param([string]$OutputDirectory, [string]$GameRoot65, [string]$GameRoot66)
$ErrorActionPreference = 'Stop'
$sourceRoot = $PSScriptRoot
$workspace = [IO.Path]::GetFullPath((Join-Path $sourceRoot '..\..\..\..'))
$buildRoot = Join-Path $sourceRoot 'build'
if (!$OutputDirectory) {
    $OutputDirectory = Join-Path (Split-Path $sourceRoot -Parent) ('双版本验收-20261022\regression-' + (Get-Date -Format 'HHmmss'))
}
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$fixture65 = if ($GameRoot65) { [IO.Path]::GetFullPath($GameRoot65) } else { Join-Path $workspace '基底\加强版6.5未加密版' }
$fixture66 = if ($GameRoot66) { [IO.Path]::GetFullPath($GameRoot66) } else { Join-Path $workspace '基底\复刻版6.6山贼 国庆' }
$exe65 = Join-Path $fixture65 'Ekd5.exe'
$exe66 = Join-Path $fixture66 'Ekd5.exe'
$originalAudio = Join-Path $fixture65 'Koeicda_org.dll'
$installer = Join-Path $buildRoot 'package\加速补丁安装器.exe'
$script:results = @()
function Invoke-Check([string]$Label, [string]$Program, [string[]]$TestArguments) {
    $log = Join-Path $OutputDirectory ($Label + '.log')
    & $Program @TestArguments *> $log
    $code = $LASTEXITCODE
    $script:results += [pscustomobject]@{Test=$Label; ExitCode=$code; Log=$log}
    if ($code) {
        Get-Content -LiteralPath $log -Tail 30
        $script:results | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDirectory 'results.json') -Encoding utf8
        throw "$Label failed with exit code $code"
    }
    Write-Output "PASS $Label"
}
foreach ($generation in @('65','66')) {
    $targetRoot = Join-Path $buildRoot $generation
    $fixture = if ($generation -eq '65') { $fixture65 } else { $fixture66 }
    $exe = Join-Path $fixture 'Ekd5.exe'
    Push-Location $targetRoot
    try {
        foreach ($test in @('EngineTests','ConfigTests','BattleStatsPersistenceTests','SaveTransactionsTests','GearTests','UiIconTests')) {
            Invoke-Check "$generation-$test" (Join-Path $targetRoot ($test + '.exe')) @()
        }
        Invoke-Check "$generation-NativeExtensionFixtureTests" (Join-Path $targetRoot 'SaveTransactionsTests.exe') @('--native-extension-fixture',(Join-Path $fixture66 'ExtendData.dll'))
        Invoke-Check "$generation-LayoutTests" (Join-Path $targetRoot 'LayoutTests.exe') @($exe)
        Invoke-Check "$generation-EngineProfileTests" (Join-Path $targetRoot 'EngineProfileTests.exe') @($exe65,$exe66)
        Invoke-Check "$generation-BattleRuntimeTests" (Join-Path $targetRoot 'BattleRuntimeTests.exe') @($exe,$fixture)
        Invoke-Check "$generation-BattleStatsTests" (Join-Path $targetRoot 'BattleStatsTests.exe') @($exe)
        Invoke-Check "$generation-SaveEnhancerTests" (Join-Path $targetRoot 'SaveEnhancerTests.exe') @($exe)
        Invoke-Check "$generation-ScenarioLifecycleTests" (Join-Path $targetRoot 'ScenarioLifecycleTests.exe') @($exe65,$exe66)
        Invoke-Check "$generation-IconResourceTests" (Join-Path $targetRoot 'IconResourceTests.exe') @($installer,(Join-Path $targetRoot 'Koeicda.dll'))
        $scenarios = if ($generation -eq '65') { @('S_00.eex','S_58.eex') } else { @('S_00.eex','S_85.eex') }
        foreach ($scenario in $scenarios) {
            $guideMode = if ($generation -eq '65') {
                if ($scenario -eq 'S_00.eex') { '--standard65-s00' } else { '--standard65-s58' }
            } else {
                if ($scenario -eq 'S_00.eex') { '--shan-ze-s00' } else { '--shan-ze-s85' }
            }
            Invoke-Check "$generation-Guide-$scenario" (Join-Path $targetRoot 'GuideTests.exe') @((Join-Path $fixture ('RS\' + $scenario)),$fixture,$guideMode)
        }
    } finally { Pop-Location }
}
$testRoot = Join-Path $buildRoot '66'
Invoke-Check 'InstallerResources' (Join-Path $testRoot 'InstallerTests.exe') @($installer,(Join-Path $testRoot 'Koeicda.dll'),(Join-Path $testRoot 'GearNtKe.dll'),$originalAudio,(Join-Path $buildRoot '65\Koeicda.dll'))
Invoke-Check 'InstallerTransactions' (Join-Path $testRoot 'InstallerProfileTests.exe') @($installer,$exe65,$exe66,$originalAudio,(Join-Path $OutputDirectory 'transaction-fixtures'))
$script:results | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDirectory 'results.json') -Encoding utf8
Write-Output "All $($script:results.Count) test groups passed. Evidence: $OutputDirectory"
