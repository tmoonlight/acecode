@echo off
setlocal
pushd "%~dp0..\.."
python -X utf8 scripts\channel_lab\server.py --acecode build\Release\acecode.exe --open %*
popd
endlocal
