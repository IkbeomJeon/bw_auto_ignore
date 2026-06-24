@echo off
"C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" "C:\wd\bw_auto_ignore\bw_auto_ignore\bw_auto_ignore.vcxproj" /noautoresponse /p:Configuration=Release /p:Platform=x64 /v:minimal
