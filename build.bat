@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64
cl.exe /EHsc /LD /std:c++17 /MD /O2 dllmain.cpp /Fe:main.dll
