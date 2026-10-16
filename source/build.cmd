@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat"
if errorlevel 1 exit /b 1
cl /nologo /O2 /W4 /MT /EHsc /utf-8 /std:c++17 /DUNICODE /D_UNICODE UninstallWorker.cpp InstallCommon.cpp /Fe:UninstallWorker.exe /link /SUBSYSTEM:WINDOWS user32.lib shell32.lib bcrypt.lib ole32.lib
if errorlevel 1 exit /b 1
rc /nologo /fo Ui.res Ui.rc
if errorlevel 1 exit /b 1
cl /nologo /O2 /W4 /MT /EHsc /utf-8 /std:c++17 /DUNICODE /D_UNICODE /LD Proxy.cpp Engine.cpp EngineLayout.cpp InstallCommon.cpp Ui.cpp Guide.cpp Features.cpp FeatureUi.cpp BattleRuntime.cpp BattleInfo.cpp SaveEnhancer.cpp BattleStats.cpp Ui.res /Fe:Koeicda.dll /link /BASE:0x63000000 /DYNAMICBASE /MAP:Koeicda.map user32.lib comctl32.lib gdi32.lib winmm.lib shell32.lib bcrypt.lib ole32.lib oleaut32.lib
if errorlevel 1 exit /b 1
cl /nologo /O2 /W4 /MT /EHsc /std:c++17 /LD GearNtKe.cpp /Fe:GearNtKe.dll /link /BASE:0x64000000 /DYNAMICBASE /DEF:GearNtKe.def /MAP:GearNtKe.map user32.lib winmm.lib
if errorlevel 1 exit /b 1
rc /nologo /fo Installer.res Installer.rc
if errorlevel 1 exit /b 1
cl /nologo /O2 /W4 /MT /EHsc /utf-8 /std:c++17 /DUNICODE /D_UNICODE Installer.cpp InstallCommon.cpp Installer.res /Fe:"加速补丁6.6安装器.exe" /link /SUBSYSTEM:WINDOWS user32.lib shell32.lib bcrypt.lib ole32.lib
if errorlevel 1 exit /b 1
