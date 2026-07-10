@echo off
setlocal
set MSVC=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207
set SDK=C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0
set SDKLIB=C:\Program Files (x86)\Windows Kits\10\Lib\10.0.26100.0
set HOSTX64=%MSVC%\bin\Hostx64\x64
set PATH=%HOSTX64%;%PATH%
set INCLUDE=%MSVC%\include;%SDK%\ucrt;%SDK%\um;%SDK%\shared
set LIB=%MSVC%\lib\x64;%SDKLIB%\ucrt\x64;%SDKLIB%\um\x64
"%HOSTX64%\cl.exe" /EHsc /LD /std:c++17 /MD /O2 /I libhelix-aac /DUSE_DEFAULT_STDLIB dllmain.cpp libhelix-aac\*.c /link /OUT:main.dll
