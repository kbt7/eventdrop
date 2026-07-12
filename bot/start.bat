@echo off
cd /d %~dp0
if not exist node_modules (
    echo 初回セットアップ中...
    call npm install
)
if not exist .env (
    echo .env がありません。.env.example をコピーしてトークンを設定してください
    pause
    exit /b 1
)
node index.js
pause
