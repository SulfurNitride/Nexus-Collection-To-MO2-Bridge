@echo off
REM NexusBridge Windows Build Script
REM Usage: build_windows.bat [options]
REM Options:
REM   -Clean     Clean all build artifacts first
REM   -Package   Create distribution zip after building

powershell -ExecutionPolicy Bypass -File "%~dp0build_windows.ps1" %*
