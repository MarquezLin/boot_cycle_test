@echo off
setlocal EnableExtensions DisableDelayedExpansion
rem Override these environment variables if the build tools move.
if not defined QT_ROOT set "QT_ROOT=C:\Qt\6.10.2\msvc2022_64"
if not defined CMAKE_EXE set "CMAKE_EXE=C:\Qt\Tools\CMake_64\bin\cmake.exe"
if not defined NINJA_EXE set "NINJA_EXE=C:\Qt\Tools\Ninja\ninja.exe"
set "SOURCE_ROOT=%~dp0"
set "BUILD_ROOT=%SOURCE_ROOT%build\package-release"
set "RELEASE_ROOT=%SOURCE_ROOT%release"
set "LOG_FILE=%SOURCE_ROOT%build\package_release.log"
set "RESULT=1"
if not exist "%SOURCE_ROOT%build" mkdir "%SOURCE_ROOT%build"
if not exist "%SOURCE_ROOT%build" goto finish
> "%LOG_FILE%" echo GVFG Release packaging

if not exist "%QT_ROOT%\lib\cmake\Qt6\Qt6Config.cmake" (
    echo ERROR: Qt MSVC x64 kit not found: %QT_ROOT%
    goto finish
)
if not exist "%CMAKE_EXE%" (
    echo ERROR: CMake not found: %CMAKE_EXE%
    goto finish
)
if not exist "%NINJA_EXE%" (
    echo ERROR: Ninja not found: %NINJA_EXE%
    goto finish
)
for %%F in (tesseract.exe libtesseract-5.dll libleptonica-6.dll tessdata\eng.traineddata tessdata\chi_tra.traineddata tessdata\chi_tra_vert.traineddata tessdata\LICENSE) do (
    if not exist "%SOURCE_ROOT%third_party\ocr\%%F" (
        echo ERROR: Missing portable OCR file: third_party\ocr\%%F
        goto finish
    )
)
if not exist "%SOURCE_ROOT%distribution\licenses\THIRD_PARTY.md" (
    echo ERROR: Missing distribution\licenses\THIRD_PARTY.md
    goto finish
)

echo [1/4] Loading Visual Studio x64 build environment...
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not defined VS_ROOT (
    if not exist "%VSWHERE%" (
        echo ERROR: Install Visual Studio C++ Build Tools, or set VS_ROOT.
        goto finish
    )
)
if not defined VS_ROOT for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_ROOT=%%I"
if not exist "%VS_ROOT%\VC\Auxiliary\Build\vcvars64.bat" (
    echo ERROR: Visual Studio C++ x64 build tools not found.
    goto finish
)
call "%VS_ROOT%\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 goto failed

echo [2/4] Configuring Release...
"%CMAKE_EXE%" -S "%SOURCE_ROOT%." -B "%BUILD_ROOT%" -G Ninja "-DCMAKE_BUILD_TYPE=Release" "-DCMAKE_PREFIX_PATH=%QT_ROOT%" "-DCMAKE_MAKE_PROGRAM=%NINJA_EXE%" >> "%LOG_FILE%" 2>&1
if errorlevel 1 goto failed

echo [3/4] Building the latest source...
"%CMAKE_EXE%" --build "%BUILD_ROOT%" --parallel >> "%LOG_FILE%" 2>&1
if errorlevel 1 goto failed

echo [4/4] Collecting runtime files and creating ZIP. Please wait...
for %%I in ("%CMAKE_EXE%") do set "CPACK_EXE=%%~dpIcpack.exe"
"%CPACK_EXE%" --config "%BUILD_ROOT%\CPackConfig.cmake" -B "%RELEASE_ROOT%" >> "%LOG_FILE%" 2>&1
if errorlevel 1 goto failed

set "RESULT=0"
echo.
echo SUCCESS: Latest Release ZIP is in:
echo %RELEASE_ROOT%
echo Version comes from project VERSION in CMakeLists.txt.
echo A ZIP with the same version is replaced. Older versions are kept.
goto finish

:failed
echo.
echo ERROR: Packaging failed. Existing ZIP files may be from an earlier build.
type "%LOG_FILE%"

:finish
echo.
echo Build log: %LOG_FILE%
if not "%RESULT%"=="0" echo Packaging did not complete successfully.
if /i not "%~1"=="/nopause" pause
exit /b %RESULT%
