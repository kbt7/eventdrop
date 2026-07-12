// eventdrop Discord ボット
// /drop 確率 … イベント時間から必要な当選確率を求める
// /drop 時間 … 当選確率から目標到達までの所要時間を推定する
require('dotenv').config();

const {
  Client,
  GatewayIntentBits,
  SlashCommandBuilder,
  EmbedBuilder,
} = require('discord.js');
const { execFile } = require('node:child_process');
const path = require('node:path');

const EXE =
  process.env.EVENTDROP_EXE || path.join(__dirname, '..', 'eventdrop.exe');
const DEFAULT_SIMULATIONS = 1000;

// ---- exe 実行 -------------------------------------------------------------

function runEventdrop(args) {
  return new Promise((resolve, reject) => {
    execFile(
      EXE,
      [...args, '--json'],
      { timeout: 120_000, windowsHide: true },
      (err, stdout, stderr) => {
        // 到達不可などは exit 1 でも JSON を出力するので stdout を優先して解析
        const line = (stdout || '').trim().split('\n').pop();
        try {
          resolve(JSON.parse(line));
        } catch {
          reject(new Error(stderr?.trim() || err?.message || '実行に失敗しました'));
        }
      }
    );
  });
}

// ---- 表示整形 ---------------------------------------------------------------

function fmtPercent(p) {
  const text = p >= 1 ? p.toFixed(4) : p.toPrecision(4);
  const odds = Math.round(100 / p);
  return `**${text}%**(約 1/${odds.toLocaleString('ja-JP')})`;
}

function fmtDuration(seconds) {
  const h = Math.floor(seconds / 3600);
  const m = Math.floor((seconds % 3600) / 60);
  const s = seconds % 60;
  let out = '';
  if (h > 0) out += `${h}時間`;
  if (m > 0) out += `${m}分`;
  if (s > 0 || out === '') out += `${s}秒`;
  return out;
}

function conditionFields(o) {
  return [
    { name: 'ユーザー数', value: `${o.users.toLocaleString('ja-JP')}人`, inline: true },
    { name: '1人あたりの上限', value: `${o.limit}個`, inline: true },
    { name: '目標ドロップ数', value: `${o.target.toLocaleString('ja-JP')}個`, inline: true },
  ];
}

// ---- コマンド定義 -----------------------------------------------------------

const dropCommand = new SlashCommandBuilder()
  .setName('drop')
  .setDescription('イベントドロップ率シミュレーター')
  .addSubcommand((sub) =>
    sub
      .setName('確率')
      .setDescription('イベント時間から、必要な当選確率を計算します')
      .addStringOption((opt) =>
        opt
          .setName('時間')
          .setDescription('イベントの長さ (例: 24h、90m、1h30m、数字だけなら時間)')
          .setRequired(true)
      )
      .addIntegerOption((opt) =>
        opt
          .setName('ユーザー数')
          .setDescription('参加ユーザー数 (例: 1000)')
          .setRequired(true)
          .setMinValue(1)
      )
      .addIntegerOption((opt) =>
        opt
          .setName('上限')
          .setDescription('1人が入手できる最大数 (例: 5)')
          .setRequired(true)
          .setMinValue(1)
      )
      .addNumberOption((opt) =>
        opt
          .setName('目標')
          .setDescription('全体で配りたいドロップ総数 (例: 2000)')
          .setRequired(true)
          .setMinValue(1)
      )
      .addIntegerOption((opt) =>
        opt
          .setName('精度')
          .setDescription('シミュレーション回数 (省略時1000。多いほど正確だが遅い)')
          .setMinValue(100)
          .setMaxValue(100000)
      )
  )
  .addSubcommand((sub) =>
    sub
      .setName('時間')
      .setDescription('当選確率から、目標に届くまでの時間を推定します')
      .addNumberOption((opt) =>
        opt
          .setName('確率')
          .setDescription('1回の抽選で当たる確率%(例: 0.05)')
          .setRequired(true)
          .setMinValue(0)
      )
      .addIntegerOption((opt) =>
        opt
          .setName('ユーザー数')
          .setDescription('参加ユーザー数 (例: 1000)')
          .setRequired(true)
          .setMinValue(1)
      )
      .addIntegerOption((opt) =>
        opt
          .setName('上限')
          .setDescription('1人が入手できる最大数 (例: 5)')
          .setRequired(true)
          .setMinValue(1)
      )
      .addNumberOption((opt) =>
        opt
          .setName('目標')
          .setDescription('全体で配りたいドロップ総数 (例: 2000)')
          .setRequired(true)
          .setMinValue(1)
      )
      .addIntegerOption((opt) =>
        opt
          .setName('精度')
          .setDescription('シミュレーション回数 (省略時1000。多いほど正確だが遅い)')
          .setMinValue(100)
          .setMaxValue(100000)
      )
  );

// ---- ボット本体 -------------------------------------------------------------

const client = new Client({ intents: [GatewayIntentBits.Guilds] });

client.once('clientReady', async () => {
  console.log(`ログイン完了: ${client.user.tag}`);

  const guildId = process.env.GUILD_ID;
  if (guildId) {
    const guild = await client.guilds.fetch(guildId);
    await guild.commands.set([dropCommand]);
    console.log(`コマンドをサーバー ${guild.name} に登録しました(即時反映)`);
  } else {
    await client.application.commands.set([dropCommand]);
    console.log('コマンドをグローバル登録しました(反映まで最大1時間)');
  }
});

client.on('interactionCreate', async (interaction) => {
  if (!interaction.isChatInputCommand() || interaction.commandName !== 'drop')
    return;

  const sub = interaction.options.getSubcommand();
  const users = interaction.options.getInteger('ユーザー数');
  const limit = interaction.options.getInteger('上限');
  const target = interaction.options.getNumber('目標');
  const sims = interaction.options.getInteger('精度') ?? DEFAULT_SIMULATIONS;

  await interaction.deferReply();

  try {
    if (sub === '確率') {
      const time = interaction.options.getString('時間');
      const r = await runEventdrop([users, time, limit, target, sims]);

      const embed = new EmbedBuilder()
        .setColor(0x4caf50)
        .setTitle('🎯 必要な当選確率')
        .setDescription(
          `${fmtDuration(r.seconds)} のイベントで目標に届かせるには、` +
            `1回の抽選(20秒ごと)の当選確率を ${fmtPercent(r.percent)} にしてください`
        )
        .addFields(
          ...conditionFields(r),
          { name: 'イベント時間', value: fmtDuration(r.seconds), inline: true },
          {
            name: '予想ドロップ総数',
            value: `約${Math.round(r.average).toLocaleString('ja-JP')}個`,
            inline: true,
          },
          { name: '計算時間', value: `${r.elapsedSec}秒`, inline: true }
        );

      await interaction.editReply({ embeds: [embed] });
    } else {
      const percent = interaction.options.getNumber('確率');
      const r = await runEventdrop([users, `${percent}%`, limit, target, sims]);

      if (r.error === 'unreachable') {
        await interaction.editReply(
          `⚠️ 目標 ${target.toLocaleString('ja-JP')}個は上限(${users.toLocaleString('ja-JP')}人 × ${limit}個 = ` +
            `${(users * limit).toLocaleString('ja-JP')}個)を超えているか、確率が0のため到達できません`
        );
        return;
      }
      if (r.error === 'over_100_years') {
        await interaction.editReply(
          '⚠️ その確率では100年以内に目標へ到達できません。確率を上げてください'
        );
        return;
      }

      const embed = new EmbedBuilder()
        .setColor(0x2196f3)
        .setTitle('⏱️ 推定所要時間')
        .setDescription(
          `当選確率 ${fmtPercent(r.percent)} なら、目標到達まで **約${r.timeText}** かかります`
        )
        .addFields(
          ...conditionFields(r),
          { name: '推定所要時間', value: r.timeText, inline: true },
          {
            name: '予想ドロップ総数',
            value: `約${Math.round(r.average).toLocaleString('ja-JP')}個`,
            inline: true,
          },
          { name: '計算時間', value: `${r.elapsedSec}秒`, inline: true }
        );

      await interaction.editReply({ embeds: [embed] });
    }
  } catch (e) {
    console.error(e);
    await interaction.editReply(`❌ エラー: ${e.message}`);
  }
});

const token = process.env.DISCORD_TOKEN;
if (!token || token === 'ここにトークン') {
  console.error(
    'DISCORD_TOKEN が設定されていません。bot フォルダの .env を編集してください'
  );
  process.exit(1);
}

client.login(token);
