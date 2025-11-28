@echo off
call setup_env.bat
if %errorlevel% neq 0 exit /b %errorlevel%
nmake %*
