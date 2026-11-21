@echo off
chcp 65001 >nul
echo ============================================================
echo  推送 esp32-atc-radar 到 GitHub
echo ============================================================
echo.
echo  前置：请先在浏览器登录 GitHub，网页上手动建一个空仓库，
echo        名字建议 esp32-atc-radar，不要勾选 README/license。
echo.

set /p GHUSER="GitHub 用户名 (默认 wuruiyun330105): "
if "%GHUSER%"=="" set GHUSER=wuruiyun330105
set /p REPONAME="仓库名 (默认 esp32-atc-radar): "
if "%REPONAME%"=="" set REPONAME=esp32-atc-radar

echo.
echo  [1/4] 初始化本地 git ...
git init
if errorlevel 1 goto fail

echo  [2/4] 暂存文件 ...
git add .
if errorlevel 1 goto fail

echo  [3/4] 提交 ...
git -c user.name="wuruiyun330105" -c user.email="wuruiyun330105@users.noreply.github.com" commit -m "Initial commit: ESP32 ATC radar desk station"
if errorlevel 1 goto fail

echo  [4/4] 推送（首次会弹出 GitHub 登录窗，用浏览器授权即可）...
git remote remove origin 2>nul
git remote add origin https://github.com/%GHUSER%/%REPONAME%.git
git branch -M main
git push -u origin main
if errorlevel 1 goto fail

echo.
echo  完成！仓库地址：https://github.com/%GHUSER%/%REPONAME%
echo.
pause
exit /b 0

:fail
echo.
echo  推送失败。常见原因：
echo   1. 仓库没建或名字不对 —— 去 github.com/new 建空仓库
echo   2. 登录窗口没授权 —— 重试本脚本，弹出窗口时完成 GitHub 登录
echo   3. 网络问题 —— 挂代理后重试
echo.
pause
exit /b 1
