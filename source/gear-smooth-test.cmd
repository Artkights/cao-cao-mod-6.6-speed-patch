@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat"
if errorlevel 1 exit /b 1
cl /nologo /O2 /W4 /WX /MT /EHsc /std:c++17 /Fo:GearTests_smooth.obj GearTests.cpp /Fe:GearTests_smooth.exe /link user32.lib winmm.lib
if errorlevel 1 exit /b 1
GearTests_smooth.exe
if errorlevel 1 exit /b 1
cl /nologo /O2 /W4 /WX /MT /EHsc /std:c++17 /Fo:GearClockBench.obj GearClockBench.cpp /Fe:GearClockBench.exe /link user32.lib winmm.lib gdiplus.lib
if errorlevel 1 exit /b 1
GearClockBench.exe
exit /b %errorlevel%
