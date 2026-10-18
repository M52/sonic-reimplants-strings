@echo off
setlocal
if defined VSCMD_VER goto ready
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSI=%%i"
if not defined VSI (
    echo Visual Studio with the C++ tools was not found.
    exit /b 1
)
call "%VSI%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul
:ready
cd /d "%~dp0"
if not exist obj mkdir obj
set WP=3rdparty\wavpack-5.9.0
set IG=3rdparty\imgui
set WPSRC=%WP%\src\common_utils.c %WP%\src\decorr_utils.c %WP%\src\entropy_utils.c %WP%\src\extra1.c %WP%\src\extra2.c %WP%\src\open_utils.c %WP%\src\open_filename.c %WP%\src\open_legacy.c %WP%\src\open_raw.c %WP%\src\pack.c %WP%\src\pack_dns.c %WP%\src\pack_floats.c %WP%\src\pack_utils.c %WP%\src\read_words.c %WP%\src\tags.c %WP%\src\tag_utils.c %WP%\src\unpack.c %WP%\src\unpack_floats.c %WP%\src\unpack_seek.c %WP%\src\unpack_utils.c %WP%\src\write_words.c
set CPPSRC=src\sr_main.cpp src\sr_core.cpp src\sr_gui.cpp src\sr_window_win32.cpp %IG%\imgui.cpp %IG%\imgui_draw.cpp %IG%\imgui_tables.cpp %IG%\imgui_widgets.cpp %IG%\backends\imgui_impl_win32.cpp 3rdparty\imgui_sw\imgui_sw.cpp
set INC=/I "%WP%" /I "%WP%\src" /I 3rdparty\monocypher /I "%IG%" /I "%IG%\backends" /I 3rdparty\imgui_sw /I 3rdparty
cl.exe /nologo /c /O2 /MT /W3 /DNDEBUG /D_CRT_SECURE_NO_WARNINGS %INC% %WPSRC% 3rdparty\monocypher\monocypher.c /Fo"obj\\"
if errorlevel 1 exit /b 1
cl.exe /nologo /c /O2 /MT /W3 /EHsc /std:c++17 /DNDEBUG /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /DIMGUI_USE_BGRA_PACKED_COLOR %INC% %CPPSRC% /Fo"obj\\"
if errorlevel 1 exit /b 1
link.exe /nologo /OUT:sr_install.exe /SUBSYSTEM:WINDOWS obj\*.obj user32.lib gdi32.lib shell32.lib bcrypt.lib advapi32.lib dwmapi.lib
if errorlevel 1 exit /b 1
rmdir /s /q obj
echo Built sr_install.exe
