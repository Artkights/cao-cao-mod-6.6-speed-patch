@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat"
if errorlevel 1 exit /b 1
cl /nologo /O2 /MT /W4 /WX /EHsc /utf-8 /std:c++17 /DUNICODE /D_UNICODE /DXA66_TEST Engine.cpp EngineLayout.cpp Features.cpp EngineTests.cpp /Fe:EngineTests.exe /link winmm.lib user32.lib
if errorlevel 1 exit /b 1
cl /nologo /O2 /MT /W4 /WX /EHsc /utf-8 /std:c++17 /DUNICODE /D_UNICODE ConfigTests.cpp InstallCommon.cpp Guide.cpp Features.cpp FeatureUi.cpp BattleRuntime.cpp BattleInfo.cpp SaveEnhancer.cpp BattleStats.cpp Ui.res /Fe:ConfigTests.exe /link user32.lib comctl32.lib gdi32.lib shell32.lib bcrypt.lib ole32.lib oleaut32.lib
if errorlevel 1 exit /b 1
cl /nologo /O2 /MT /W4 /WX /EHsc /utf-8 /std:c++17 /DUNICODE /D_UNICODE InstallerTests.cpp InstallCommon.cpp /Fe:InstallerTests.exe /link shell32.lib bcrypt.lib ole32.lib
if errorlevel 1 exit /b 1
cl /nologo /O2 /MT /W4 /WX /EHsc /utf-8 /std:c++17 /DUNICODE /D_UNICODE IconResourceTests.cpp /Fe:IconResourceTests.exe /link user32.lib gdi32.lib
if errorlevel 1 exit /b 1
cl /nologo /O2 /MT /W4 /WX /EHsc /utf-8 /std:c++17 /DUNICODE /D_UNICODE UiIconTests.cpp Ui.cpp Ui.res /Fe:UiIconTests.exe /link user32.lib comctl32.lib gdi32.lib
if errorlevel 1 exit /b 1
cl /nologo /O2 /MT /W4 /WX /EHsc /utf-8 /std:c++17 /DUNICODE /D_UNICODE MenuUninstallHarness.cpp /Fe:MenuUninstallHarness.exe /link user32.lib
if errorlevel 1 exit /b 1
cl /nologo /O2 /MT /W4 /WX /EHsc /utf-8 /std:c++17 /DUNICODE /D_UNICODE InstallerLocationHarness.cpp /Fe:InstallerLocationHarness.exe /link user32.lib
if errorlevel 1 exit /b 1
cl /nologo /O2 /MT /W4 /WX /EHsc /utf-8 /std:c++17 /DUNICODE /D_UNICODE Guide.cpp BattleRuntime.cpp GuideTests.cpp Ui.res /Fe:GuideTests.exe /link user32.lib comctl32.lib gdi32.lib shell32.lib ole32.lib oleaut32.lib
if errorlevel 1 exit /b 1
cl /nologo /O2 /MT /W4 /WX /EHsc /utf-8 /std:c++17 /DUNICODE /D_UNICODE GuideWindowHarness.cpp /Fe:GuideWindowHarness.exe /link user32.lib gdi32.lib
if errorlevel 1 exit /b 1
cl /nologo /O2 /MT /W4 /WX /EHsc /utf-8 /std:c++17 /DUNICODE /D_UNICODE BattleRuntimeTests.cpp BattleRuntime.cpp /Fe:BattleRuntimeTests.exe /link user32.lib
if errorlevel 1 exit /b 1
cl /nologo /O2 /MT /W4 /WX /EHsc /utf-8 /std:c++17 /DUNICODE /D_UNICODE BattleStatsTests.cpp BattleStats.cpp BattleRuntime.cpp /Fe:BattleStatsTests.exe /link user32.lib
if errorlevel 1 exit /b 1
cl /nologo /O2 /MT /W4 /WX /EHsc /utf-8 /std:c++17 /DUNICODE /D_UNICODE AudioExportTests.cpp /Fe:AudioExportTests.exe /link user32.lib
if errorlevel 1 exit /b 1
cl /nologo /O2 /MT /W4 /WX /EHsc /std:c++17 GearTests.cpp /Fe:GearTests.exe /link user32.lib winmm.lib
if errorlevel 1 exit /b 1
