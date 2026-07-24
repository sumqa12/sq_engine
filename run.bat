@echo off

rem 遅延環境変数の使用を宣言
setlocal enabledelayedexpansion

rem 引数の取得
set p1=%1
set p2=%2

if "!p1!"=="admin" (
    set profile=debug
    set admin=true
) else (
    set profile=%1
    set admin=%2
)

rem 管理者権限で実行するか
if "!admin!"=="true" (
    set admin=true
) else (
    set admin=false
)

rem プロファイルはデフォルトでdebug
if "!profile!"=="release" (
    set profile=release
) else (
    set profile=debug
)

echo プロファイル: !profile!
echo 管理者権限: !admin!

rem カレントフォルダへ移動
cd /d %~dp0

rem 実行ファイルが存在するかチェック
set file_exist=false
set working_dir=build\debug\sandbox_graphics\Debug\
if "!profile!"=="release" (
    if exist "build\release\sandbox_graphics\Release\sandbox_graphics.exe" (
        set file_exist=true
        set target=build\release\sandbox_graphics\Release\sandbox_graphics.exe
        set working_dir=build\release\sandbox_graphics\Release\
    )
) else (
    if exist "build\debug\sandbox_graphics\Debug\sandbox_graphics.exe" (
        set file_exist=true
        set target=build\debug\sandbox_graphics\Debug\sandbox_graphics.exe
    )
)

set exitcode=0

if "!file_exist!"=="true" (
    if "!admin!"=="true" (
        rem --- 管理者権限昇格処理 ---
        net session >NUL 2>nul
        set "net_err=!errorlevel!"
        if not "!net_err!"=="0" (
            echo 管理者権限で起動しています
            powershell -NoProfile -Command "Start-Process -FilePath '!target!' -WorkingDirectory '!working_dir!' -Verb RunAs -Wait"
            set "exitcode=!errorlevel!"
        ) else (
            echo 起動しています
            start /wait /d "!working_dir!" "" "!target!"
            set "exitcode=!errorlevel!"
        )
    ) else (
        echo 起動しています
        start /wait /d "!working_dir!" "" "!target!"
        set "exitcode=!errorlevel!"
    )
) else (
    echo 実行ファイルが存在しません。ビルドをしてください。
    pause
    set "exitcode=1"
)

rem 終了直後のエラーレベルを判定
if "!exitcode!"=="0" (
    echo [成功] プログラムは正常に終了しました。
) else (
    echo [エラー] プログラムが異常終了しました。エラーコード: !exitcode!
)

endlocal
pause