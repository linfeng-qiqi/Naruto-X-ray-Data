@echo off
setlocal
set "MSVC=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.51.36231"
set "SDK=C:\Program Files (x86)\Windows Kits\10"
set "SDKV=10.0.26100.0"
set "INCLUDE=%MSVC%\include;%SDK%\Include\%SDKV%\ucrt;%SDK%\Include\%SDKV%\shared;%SDK%\Include\%SDKV%\um;%SDK%\Include\%SDKV%\winrt;%SDK%\Include\%SDKV%\cppwinrt"
set "LIB=%MSVC%\lib\x64;%SDK%\Lib\%SDKV%\ucrt\x64;%SDK%\Lib\%SDKV%\um\x64"
set "PATH=%MSVC%\bin\Hostx64\x64;%PATH%"

cd /d "%~dp0"
if not exist build mkdir build

cl /nologo /std:c++20 /EHsc /O2 /MD /utf-8 /DUNICODE /D_UNICODE /W3 ^
   /Fo:build\ /Fd:build\vc.pdb ^
   main.cpp zlib\adler32.c zlib\crc32.c zlib\inflate.c zlib\inftrees.c zlib\inffast.c zlib\infback.c zlib\zutil.c zlib\trees.c ^
   /I. ^
   /link /SUBSYSTEM:CONSOLE /OUT:build\huoying.exe ^
   gdiplus.lib user32.lib gdi32.lib shell32.lib advapi32.lib ole32.lib
if errorlevel 1 (
  echo BUILD FAILED
  exit /b 1
)
echo BUILD OK
