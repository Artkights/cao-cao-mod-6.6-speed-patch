@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat"
if errorlevel 1 exit /b 1
cl /nologo /O2 /MT /W4 /WX /EHsc /utf-8 /std:c++17 /DXA66_TEST Engine.cpp EngineLayout.cpp Features.cpp LayoutTests.cpp /Fe:LayoutTests.exe /link user32.lib winmm.lib
