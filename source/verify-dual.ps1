param([string]$OutputDirectory, [string]$GameRoot64, [string]$GameRoot65, [string]$GameRoot66,
    [string]$BuildDirectory, [string]$GameRootPacked, [string]$GameRootNobuna, [string]$GameRootNobuna12,
    [string]$GameRoot220, [string]$RuntimeImage220)
$ErrorActionPreference = 'Stop'
$sourceRoot = $PSScriptRoot
# The repository is kept beside the three local game fixtures under the
# versioned project directory.  Two parent hops from source resolve to that
# project directory, which contains the default fixture paths.
$workspace = [IO.Path]::GetFullPath((Join-Path $sourceRoot '..\..'))
$buildRoot = if ($BuildDirectory) { [IO.Path]::GetFullPath($BuildDirectory) } else { Join-Path $sourceRoot 'build' }
if (!$OutputDirectory) {
    $projectRoot = Split-Path (Split-Path $sourceRoot -Parent) -Parent
    $OutputDirectory = Join-Path $projectRoot ('双版本验收-20261022\regression-' + (Get-Date -Format 'HHmmss'))
}
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$fixture64 = if ($GameRoot64) { [IO.Path]::GetFullPath($GameRoot64) } else { Join-Path $workspace '基底\洗隋' }
$fixture65 = if ($GameRoot65) { [IO.Path]::GetFullPath($GameRoot65) } else { Join-Path $workspace '基底\加强版6.5未加密版' }
$fixture66 = if ($GameRoot66) { [IO.Path]::GetFullPath($GameRoot66) } else { Join-Path $workspace '基底\复刻版6.6山贼 国庆' }
$fixtureNobuna = if ($GameRootNobuna) { [IO.Path]::GetFullPath($GameRootNobuna) } else { Join-Path $workspace '基底\信奈的野望（15+1关版，密码nobuna）' }
$fixtureNobuna12 = if ($GameRootNobuna12) { [IO.Path]::GetFullPath($GameRootNobuna12) } else { Join-Path $workspace '基底\信奈的野望12关版（密码nobuna）' }
$fixturePacked = if ($GameRootPacked) { [IO.Path]::GetFullPath($GameRootPacked) } else { Join-Path $workspace '基底\大明6.6 发ZQ看似修改其实全是bug但没什么吊影响版本' }
$fixture220 = if ($GameRoot220) { [IO.Path]::GetFullPath($GameRoot220) } else { Join-Path $workspace '基底\加强版2.2.0' }
$runtime220 = if ($RuntimeImage220) { [IO.Path]::GetFullPath($RuntimeImage220) } else { Join-Path $workspace '验证\加速功能修复-20261024\2.2.0-runtime\pid6152-mainimage.rva.bin' }
$exe64 = Join-Path $fixture64 'Ekd5.exe'
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
foreach ($generation in @('64','65','66')) {
    $targetRoot = Join-Path $buildRoot $generation
    $fixture = switch ($generation) { '64' { $fixture64 } '65' { $fixture65 } default { $fixture66 } }
    $exe = Join-Path $fixture 'Ekd5.exe'
    Push-Location $targetRoot
    try {
        foreach ($test in @('EngineTests','ConfigTests','GuideBookFileTests','GuideWindowTests','BattleStatsPersistenceTests','BattleStatsExportTests','StatsDiagnosticsTests','BattleInfoTests','SaveTransactionsTests','GearTests','UiIconTests','ProtectedRuntimeTests','RuntimeStartupTests')) {
            Invoke-Check "$generation-$test" (Join-Path $targetRoot ($test + '.exe')) @()
        }
        # ExtendData.dll is a 6.6 native-extension fixture.  The test is
        # independent of the generated proxy, so run it once in the 6.6
        # pass instead of pretending the same 6.6 module belongs to all
        # three engine generations.
        if ($generation -eq '66') {
            Invoke-Check "$generation-NativeExtensionFixtureTests" (Join-Path $targetRoot 'SaveTransactionsTests.exe') @('--native-extension-fixture',(Join-Path $fixture66 'ExtendData.dll'))
        }
        if ($generation -eq '66') {
            Invoke-Check '66-NoExtensionSaveTransactions-Nobuna15' (Join-Path $targetRoot 'SaveEnhancerTests.exe') @('--extension-variant',(Join-Path $fixtureNobuna 'Ekd5.exe'))
            Invoke-Check '66-NoExtensionSaveTransactions-Nobuna12' (Join-Path $targetRoot 'SaveEnhancerTests.exe') @('--extension-variant',(Join-Path $fixtureNobuna12 'Ekd5.exe'))
            Invoke-Check '66-VerifiedExtensionSaveTransactions' (Join-Path $targetRoot 'SaveEnhancerTests.exe') @('--extension-variant',$exe66)
        }
        if ($generation -eq '65') {
            Invoke-Check '65-Protected220Profile' (Join-Path $targetRoot 'EngineProfileTests.exe') @('--protected-fixture',(Join-Path $fixture220 'Ekd5.exe'))
            Invoke-Check '65-Protected220RuntimeResources' (Join-Path $targetRoot 'BattleRuntimeTests.exe') @('--runtime-rva',$runtime220,$fixture220)
            foreach ($protectedScenario in @('S_00.eex','S_01.eex','S_11.eex','S_58.eex')) {
                Invoke-Check "65-Protected220Guide-$protectedScenario" (Join-Path $targetRoot 'GuideTests.exe') @((Join-Path $fixture220 ('RS\' + $protectedScenario)),$fixture220)
            }
        }
        Invoke-Check "$generation-PeImageTests" (Join-Path $targetRoot 'PeImageTests.exe') @()
        Invoke-Check "$generation-LayoutTests" (Join-Path $targetRoot 'LayoutTests.exe') @($exe)
        Invoke-Check "$generation-EngineProfileTests" (Join-Path $targetRoot 'EngineProfileTests.exe') @($exe64,$exe65,$exe66)
        if ($generation -eq '66' -and (Test-Path -LiteralPath (Join-Path $fixturePacked 'Ekd5.exe'))) {
            Invoke-Check '66-PackedEnigmaEngineProfile' (Join-Path $targetRoot 'EngineProfileTests.exe') @('--packed',(Join-Path $fixturePacked 'Ekd5.exe'))
        }
        Invoke-Check "$generation-BattleRuntimeTests" (Join-Path $targetRoot 'BattleRuntimeTests.exe') @($exe,$fixture)
        Invoke-Check "$generation-BattleStatsTests" (Join-Path $targetRoot 'BattleStatsTests.exe') @($exe)
        Invoke-Check "$generation-BattleRosterRuntimeTests" (Join-Path $targetRoot 'BattleRosterRuntimeTests.exe') @($exe)
        Invoke-Check "$generation-GuideSynthetic" (Join-Path $targetRoot 'GuideTests.exe') @('--self-test')
        Invoke-Check "$generation-SaveEnhancerTests" (Join-Path $targetRoot 'SaveEnhancerTests.exe') @($exe)
        # ScenarioLifecycleTests consumes the historical 6.5, 6.6, then
        # 6.4 image order; EngineProfileTests below intentionally uses the
        # explicit 6.4, 6.5, 6.6 order.
        Invoke-Check "$generation-ScenarioLifecycleTests" (Join-Path $targetRoot 'ScenarioLifecycleTests.exe') @($exe65,$exe66,$exe64)
        Invoke-Check "$generation-IconResourceTests" (Join-Path $targetRoot 'IconResourceTests.exe') @($installer,(Join-Path $targetRoot 'Koeicda.dll'))
        $scenarios = switch ($generation) { '64' { @('S_01.eex','S_61.eex') } '65' { @('S_00.eex','S_58.eex') } default { @('S_00.eex','S_85.eex') } }
        foreach ($scenario in $scenarios) {
            $guideMode = if ($generation -eq '64') {
                if ($scenario -eq 'S_01.eex') { '--xisui-s01' } else { '--xisui-s61' }
            } elseif ($generation -eq '65') {
                if ($scenario -eq 'S_00.eex') { '--standard65-s00' } else { '--standard65-s58' }
            } else {
                if ($scenario -eq 'S_00.eex') { '--shan-ze-s00' } else { '--shan-ze-s85' }
            }
            Invoke-Check "$generation-Guide-$scenario" (Join-Path $targetRoot 'GuideTests.exe') @((Join-Path $fixture ('RS\' + $scenario)),$fixture,$guideMode)
        }
    } finally { Pop-Location }
}
$testRoot = Join-Path $buildRoot '66'
Invoke-Check 'InstallerResources' (Join-Path $testRoot 'InstallerTests.exe') @($installer,(Join-Path $testRoot 'Koeicda.dll'),(Join-Path $testRoot 'GearNtKe.dll'),$originalAudio,(Join-Path $buildRoot '65\Koeicda.dll'),(Join-Path $buildRoot '64\Koeicda.dll'))
Invoke-Check 'InstallerTransactions' (Join-Path $testRoot 'InstallerProfileTests.exe') @($installer,$exe64,$exe65,$exe66,$originalAudio,(Join-Path $OutputDirectory 'transaction-fixtures'))
$script:results | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDirectory 'results.json') -Encoding utf8
$required = @('66-NoExtensionSaveTransactions-Nobuna15','66-NoExtensionSaveTransactions-Nobuna12',
    '66-VerifiedExtensionSaveTransactions','65-Protected220Profile','65-Protected220RuntimeResources',
    '64-SaveEnhancerTests','65-SaveEnhancerTests','66-SaveEnhancerTests',
    '64-SaveTransactionsTests','65-SaveTransactionsTests','66-SaveTransactionsTests',
    '64-GuideSynthetic','65-GuideSynthetic','66-GuideSynthetic','InstallerResources','InstallerTransactions',
    '64-ConfigTests','65-ConfigTests','66-ConfigTests','66-NativeExtensionFixtureTests',
    '65-Protected220Guide-S_00.eex','65-Protected220Guide-S_01.eex',
    '65-Protected220Guide-S_11.eex','65-Protected220Guide-S_58.eex',
    '64-PeImageTests','65-PeImageTests','66-PeImageTests',
    '64-ProtectedRuntimeTests','65-ProtectedRuntimeTests','66-ProtectedRuntimeTests',
    '64-RuntimeStartupTests','65-RuntimeStartupTests','66-RuntimeStartupTests',
    '66-PackedEnigmaEngineProfile')
foreach ($contract in $required) {
    $rows = @($script:results | Where-Object { $_.Test -eq $contract })
    if ($rows.Count -ne 1 -or $rows[0].ExitCode -ne 0) { throw "Required release contract missing or failed: $contract" }
}
@{PackageVersion='1.4.0';Validation='OfflineOnly';GameAcceptance='UserPending';Required=$required;Passed=$true} | ConvertTo-Json -Depth 4 |
    Set-Content -LiteralPath (Join-Path $OutputDirectory 'required-contracts.json') -Encoding utf8
Write-Output "All $($script:results.Count) test groups passed. Evidence: $OutputDirectory"
