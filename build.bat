@echo off
setlocal

REM Очистка предыдущих сборок
del *.obj 2>nul
del *.res 2>nul
del *.exe 2>nul

REM Попытка настроить окружение для 64-битной компиляции
echo Setting up 64-bit environment...
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" 2>nul
if errorlevel 1 (
    call "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat" 2>nul
    if errorlevel 1 (
        call "C:\Program Files (x86)\Microsoft Visual Studio\2017\Community\VC\Auxiliary\Build\vcvars64.bat" 2>nul
        if errorlevel 1 (
            echo ERROR: Cannot setup 64-bit Visual Studio environment
            echo Please run from "x64 Native Tools Command Prompt for VS"
            pause
            exit /b 1
        )
    )
)

echo Compiling resources...
rc /fo resource.res resource.rc
if errorlevel 1 (
    echo RESOURCE COMPILATION FAILED
    pause
    exit /b 1
)

echo Compiling 64-bit application...
REM Используем /Zc:wchar_t для совместимости wchar_t
cl /nologo /W4 /EHsc /O2 /DUNICODE /D_UNICODE /D_WIN64 /Zc:wchar_t main.cpp resource.res ^
/link /MACHINE:X64 user32.lib gdi32.lib pdh.lib shell32.lib dwmapi.lib ^
/SUBSYSTEM:WINDOWS /OUT:TaskbarWidgetPDH.exe

if errorlevel 1 (
    echo COMPILATION FAILED
    pause
    exit /b 1
)

echo BUILD SUCCESSFUL!
echo Verifying architecture...
dumpbin /headers TaskbarWidgetPDH.exe | findstr "machine"
if errorlevel 1 (
    echo WARNING: Cannot verify architecture
)

pause