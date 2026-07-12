# eventdrop Discord ボット

`/drop` コマンドでイベントドロップ率シミュレーターを Discord から実行できます。

## 使い方(Discord 側)

- `/drop 確率 時間:24h ユーザー数:1000 上限:5 目標:2000`
  → イベント時間から必要な当選確率を計算
- `/drop 時間 確率:0.05 ユーザー数:1000 上限:5 目標:2000`
  → 当選確率から目標到達までの所要時間を推定

`/drop` と打つと入力欄が説明付きで順番に表示されるので、書式を覚える必要はありません。
時間は `24h` `90m` `1h30m` のように書けます(数字だけなら時間扱い)。

## 初回セットアップ

1. https://discord.com/developers/applications で「New Application」
2. 左メニュー **Bot** → 「Reset Token」→ トークンをコピー
3. このフォルダの `.env.example` をコピーして `.env` を作り、トークンを貼る
   (GUILD_ID にサーバーIDを入れるとコマンドが即時反映される)
4. 左メニュー **OAuth2 → URL Generator** で
   `bot` と `applications.commands` にチェック → 生成されたURLでサーバーに招待
5. `start.bat` をダブルクリックで起動(初回は自動で npm install が走る)

## 構成

- `index.js` — ボット本体。`../eventdrop.exe` を `--json` 付きで実行し、結果を埋め込みで返信
- `.env` — トークン等の設定(gitに入れないこと)
- `start.bat` — 起動用
