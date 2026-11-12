@echo off
rem Builds the DX9 vertex range benchmark.
rem
rem   git submodule update --init --recursive   (once, gets Dear ImGui)
rem   build.cmd
rem   dx9_vertex_range_bench.exe                 (software vertex processing: the failing case)
rem   dx9_vertex_range_bench.exe --hwvp          (hardware vertex processing: the control)
rem
rem Dear ImGui comes from the submodule in this directory. Set IMGUI_DIR to build against a
rem different checkout (e.g. one with the DX9 patch applied).
setlocal
if "%IMGUI_DIR%"=="" set IMGUI_DIR=%~dp0imgui

set VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat
if not exist "%VCVARS%" (
    echo Could not find vcvarsall.bat at "%VCVARS%"; set VCVARS or edit this script.
    exit /b 1
)
if not exist "%IMGUI_DIR%\imgui.cpp" (
    echo Could not find Dear ImGui at "%IMGUI_DIR%"; set IMGUI_DIR or edit this script.
    exit /b 1
)

call "%VCVARS%" x86 >nul || exit /b 1

cl /nologo /O2 /EHsc /MD /W3 /I"%IMGUI_DIR%" /Fd"%~dp0bench.pdb" ^
   /Fe"%~dp0dx9_vertex_range_bench.exe" ^
   "%~dp0main.cpp" ^
   "%IMGUI_DIR%\imgui.cpp" ^
   "%IMGUI_DIR%\imgui_draw.cpp" ^
   "%IMGUI_DIR%\imgui_tables.cpp" ^
   "%IMGUI_DIR%\imgui_widgets.cpp" ^
   /link d3d9.lib
