// eventdrop.cpp
// nextadventure_event.cpp の高速化版 + 時間指定/所要時間推定
//
// 高速化の内訳:
//  1. アルゴリズム: 1試行ずつの抽選をやめ、「次の成功までの間隔」を幾何分布で
//     まとめてスキップする。1ユーザーあたりの計算量が O(試行回数) → O(成功上限) に減る。
//     ※ 元コードの「成功数 = min(上限, 全試行中の成功数)」と分布は完全に同一。
//  2. 並列化: シミュレーションを全コアに分割(スレッドごとに独立した乱数器)。
//
// 抽選は20秒に1回(180回/時)。時間は20秒刻みで丸められる。
//
// 使い方:
//   対話モード:  eventdrop.exe
//   引数モード:
//     確率を求める: eventdrop.exe <ユーザー数> <時間>   <上限> <目標成功数> <シミュレーション数> [--json]
//     時間を推定  : eventdrop.exe <ユーザー数> <確率%>  <上限> <目標成功数> <シミュレーション数> [--json]
//       ※ 第2引数の末尾に % を付けると時間推定モードになる (例: 0.05%)
//     <時間> の書式: 24 / 24h / 90m / 30s / 1h30m / 1h30m20s (数値のみ=時間)
//     --json を付けると進捗バーを出さず、結果を1行のJSONで出力する(Discord連携用)。

#include <iostream>
#include <iomanip>
#include <random>
#include <thread>
#include <atomic>
#include <vector>
#include <cmath>
#include <cstdint>
#include <string>
#include <sstream>
#include <chrono>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace std;

// 抽選間隔(秒): 180回/時 = 20秒に1回
static const long long TRIAL_INTERVAL_SEC = 20;

// 所要時間推定の探索上限 (100年分の試行回数)
static const long long MAX_TRIALS =
    100LL * 365 * 24 * 3600 / TRIAL_INTERVAL_SEC;

struct Params
{
    int    users;
    int    limit;
    double target;
    int    simulations;
};

// ---- 時間書式 ----------------------------------------------------------

// "24" "24h" "90m" "30s" "1h30m20s" などを秒に変換。失敗時は -1
static double parseDuration(const string &s)
{
    if (s.empty())
        return -1;

    // 単位なし = 時間
    bool plain = true;
    for (char c : s)
        if (!isdigit(static_cast<unsigned char>(c)) && c != '.')
            plain = false;

    if (plain)
    {
        try { return stod(s) * 3600.0; }
        catch (...) { return -1; }
    }

    double total = 0;
    size_t i = 0;

    while (i < s.size())
    {
        size_t j = i;
        while (j < s.size() &&
               (isdigit(static_cast<unsigned char>(s[j])) || s[j] == '.'))
            j++;

        if (j == i || j >= s.size())
            return -1;

        double v;
        try { v = stod(s.substr(i, j - i)); }
        catch (...) { return -1; }

        char unit = static_cast<char>(tolower(s[j]));
        if (unit == 'h')      total += v * 3600.0;
        else if (unit == 'm') total += v * 60.0;
        else if (unit == 's') total += v;
        else                  return -1;

        i = j + 1;
    }

    return total;
}

// 秒数を "XX時間XX分XX秒" 形式に
static string formatDuration(long long seconds)
{
    long long h = seconds / 3600;
    long long m = (seconds % 3600) / 60;
    long long s = seconds % 60;

    ostringstream oss;
    if (h > 0) oss << h << "時間";
    if (m > 0) oss << m << "分";
    if (s > 0 || (h == 0 && m == 0)) oss << s << "秒";
    return oss.str();
}

// ---- シミュレーション ---------------------------------------------------

// 1ユーザー分の成功数 = min(limit, Binomial(trials, p)) を幾何分布スキップで求める
// logq = log(1 - p/100)
static inline int simulateUser(long long trials, double logq, int limit,
                               mt19937_64 &rng,
                               uniform_real_distribution<double> &uni)
{
    int success = 0;
    long long pos = 0;

    while (success < limit)
    {
        double u = uni(rng);                       // [0,1)
        double gap = floor(log(u) / logq) + 1.0;   // 次の成功までの試行数 (幾何分布)

        if (!(gap <= static_cast<double>(trials - pos)))
            break;                                 // u==0 (log=-inf) もここで弾かれる

        pos += static_cast<long long>(gap);
        success++;
    }

    return success;
}

// シミュレーション本体(スレッド並列)
// 戻り値: 全ユーザー成功数合計の平均
// searchStep/searchCount は進捗バー用 (quiet時は未使用)
static double simulate(const Params &prm, long long trials, double percent,
                       int searchStep, int searchCount, bool quiet)
{
    if (percent <= 0.0 || trials <= 0)
        return 0.0;
    if (percent >= 100.0)
        return static_cast<double>(prm.users) *
               static_cast<double>(min<long long>(prm.limit, trials));

    const double logq = log1p(-percent / 100.0);

    const unsigned hw = max(1u, thread::hardware_concurrency());
    const int numThreads =
        static_cast<int>(min<unsigned>(hw, prm.simulations));

    atomic<long long> totalSuccess{0};
    atomic<int> doneSims{0};

    random_device rd;
    const uint64_t baseSeed =
        (static_cast<uint64_t>(rd()) << 32) ^ rd();

    vector<thread> workers;
    workers.reserve(numThreads);

    for (int t = 0; t < numThreads; t++)
    {
        workers.emplace_back([&, t]()
        {
            seed_seq seq{
                static_cast<uint32_t>(baseSeed),
                static_cast<uint32_t>(baseSeed >> 32),
                static_cast<uint32_t>(t)};
            mt19937_64 rng(seq);
            uniform_real_distribution<double> uni(0.0, 1.0);

            // シミュレーションをスレッド数で均等分割
            const int begin = static_cast<int>(
                static_cast<long long>(prm.simulations) * t / numThreads);
            const int end = static_cast<int>(
                static_cast<long long>(prm.simulations) * (t + 1) / numThreads);

            long long localTotal = 0;

            for (int sim = begin; sim < end; sim++)
            {
                long long successAll = 0;

                for (int user = 0; user < prm.users; user++)
                    successAll += simulateUser(trials, logq, prm.limit, rng, uni);

                localTotal += successAll;
                doneSims.fetch_add(1, memory_order_relaxed);
            }

            totalSuccess.fetch_add(localTotal, memory_order_relaxed);
        });
    }

    // メインスレッドは進捗表示を担当
    const int BAR_WIDTH = 40;
    const long long grandTotal =
        static_cast<long long>(searchCount) * prm.simulations;

    auto printBar = [&](int done)
    {
        long long current =
            static_cast<long long>(searchStep) * prm.simulations + done;
        double progress = current * 100.0 / grandTotal;
        int filled = static_cast<int>(progress / 100.0 * BAR_WIDTH);

        cout << "\r[";
        for (int i = 0; i < BAR_WIDTH; i++)
            cout << (i < filled ? '#' : '-');
        cout << "] "
             << fixed << setprecision(1)
             << setw(5) << progress << "% ("
             << current << "/" << grandTotal << ")"
             << flush;
    };

    if (!quiet)
    {
        while (doneSims.load(memory_order_relaxed) < prm.simulations)
        {
            printBar(doneSims.load(memory_order_relaxed));
            this_thread::sleep_for(chrono::milliseconds(30));
        }
    }

    for (auto &w : workers)
        w.join();

    if (!quiet)
        printBar(prm.simulations);

    return static_cast<double>(totalSuccess.load()) / prm.simulations;
}

// ---- モード1: 必要な当選確率を求める ------------------------------------

static int runPercentSearch(const Params &prm, long long trials, bool jsonMode)
{
    auto t0 = chrono::steady_clock::now();

    double low = 0.0;
    double high = 100.0;

    const int SEARCH_COUNT = 30;

    if (!jsonMode)
        cout << "\n探索中... (スレッド数: "
             << max(1u, thread::hardware_concurrency()) << ")\n";

    for (int i = 0; i < SEARCH_COUNT; i++)
    {
        double mid = (low + high) / 2.0;

        double average = simulate(prm, trials, mid, i, SEARCH_COUNT + 1, jsonMode);

        if (average < prm.target)
            low = mid;
        else
            high = mid;
    }

    double answer = (low + high) / 2.0;

    if (!jsonMode)
        cout << "\n\n最終確認中...\n";

    double average = simulate(prm, trials, answer,
                              SEARCH_COUNT, SEARCH_COUNT + 1, jsonMode);

    double elapsed = chrono::duration<double>(
        chrono::steady_clock::now() - t0).count();

    if (jsonMode)
    {
        cout << fixed << setprecision(8)
             << "{\"mode\":\"percent\""
             << ",\"percent\":" << answer
             << ",\"average\":" << average
             << ",\"users\":" << prm.users
             << ",\"trials\":" << trials
             << ",\"seconds\":" << trials * TRIAL_INTERVAL_SEC
             << ",\"limit\":" << prm.limit
             << ",\"target\":" << prm.target
             << ",\"simulations\":" << prm.simulations
             << ",\"elapsedSec\":" << setprecision(3) << elapsed
             << "}\n";
    }
    else
    {
        cout << "\n\n===== 結果 =====\n";
        cout << "対象時間       : " << formatDuration(trials * TRIAL_INTERVAL_SEC)
             << " (" << trials << "回抽選)\n";
        cout << fixed << setprecision(8);
        cout << "必要な当選確率 : " << answer << "%\n";
        cout << "平均成功数     : " << average << '\n';
        cout << setprecision(3);
        cout << "計算時間       : " << elapsed << " 秒\n";
    }

    return 0;
}

// ---- モード2: 所要時間を推定 --------------------------------------------

static int runTimeSearch(const Params &prm, double percent, bool jsonMode)
{
    auto t0 = chrono::steady_clock::now();

    // 理論上の最大値チェック
    if (prm.target > static_cast<double>(prm.users) * prm.limit ||
        percent <= 0.0)
    {
        if (jsonMode)
            cout << "{\"mode\":\"time\",\"error\":\"unreachable\"}\n";
        else
            cout << "目標成功数 " << prm.target
                 << " は上限 (" << prm.users << "人 × " << prm.limit
                 << " = " << static_cast<long long>(prm.users) * prm.limit
                 << ") を超えているか、確率が0のため到達できません\n";
        return 1;
    }

    if (!jsonMode)
        cout << "\n範囲特定中... (スレッド数: "
             << max(1u, thread::hardware_concurrency()) << ")\n";

    // 倍々で上界を見つける
    long long lo = 0;          // avg < target
    long long hi = 180;        // まず1時間分から

    while (true)
    {
        double avg = simulate(prm, hi, percent, 0, 1, true);

        if (!jsonMode)
            cout << "\r  ~" << formatDuration(hi * TRIAL_INTERVAL_SEC)
                 << " : 平均 " << fixed << setprecision(1) << avg
                 << " / 目標 " << prm.target << "          " << flush;

        if (avg >= prm.target)
            break;

        lo = hi;

        if (hi >= MAX_TRIALS)
        {
            if (jsonMode)
                cout << "{\"mode\":\"time\",\"error\":\"over_100_years\"}\n";
            else
                cout << "\n100年以内には目標に到達できません\n";
            return 1;
        }

        hi = min(hi * 2, MAX_TRIALS);
    }

    const int SEARCH_COUNT = 30;

    if (!jsonMode)
        cout << "\n探索中...\n";

    // 二分探索: 目標平均に達する最小の試行回数
    for (int i = 0; i < SEARCH_COUNT && lo + 1 < hi; i++)
    {
        long long mid = lo + (hi - lo) / 2;

        double average = simulate(prm, mid, percent, i, SEARCH_COUNT + 1,
                                  jsonMode);

        if (average < prm.target)
            lo = mid;
        else
            hi = mid;
    }

    long long answer = hi;

    if (!jsonMode)
        cout << "\n\n最終確認中...\n";

    double average = simulate(prm, answer, percent,
                              SEARCH_COUNT, SEARCH_COUNT + 1, jsonMode);

    long long answerSec = answer * TRIAL_INTERVAL_SEC;

    double elapsed = chrono::duration<double>(
        chrono::steady_clock::now() - t0).count();

    if (jsonMode)
    {
        cout << fixed << setprecision(8)
             << "{\"mode\":\"time\""
             << ",\"percent\":" << percent
             << ",\"seconds\":" << answerSec
             << ",\"trials\":" << answer
             << ",\"timeText\":\"" << formatDuration(answerSec) << "\""
             << ",\"average\":" << average
             << ",\"users\":" << prm.users
             << ",\"limit\":" << prm.limit
             << ",\"target\":" << prm.target
             << ",\"simulations\":" << prm.simulations
             << ",\"elapsedSec\":" << setprecision(3) << elapsed
             << "}\n";
    }
    else
    {
        cout << "\n\n===== 結果 =====\n";
        cout << fixed << setprecision(8);
        cout << "当選確率       : " << percent << "%\n";
        cout << "推定所要時間   : " << formatDuration(answerSec)
             << " (" << answer << "回抽選)\n";
        cout << "平均成功数     : " << average << '\n';
        cout << setprecision(3);
        cout << "計算時間       : " << elapsed << " 秒\n";
    }

    return 0;
}

// ---- main ---------------------------------------------------------------

int main(int argc, char *argv[])
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    Params prm{};
    bool jsonMode = false;
    bool timeMode = false;      // true: 確率→時間推定
    double percent = 0;
    long long trials = 0;

    if (argc >= 6)
    {
        string second = argv[2];

        prm.users       = stoi(argv[1]);
        prm.limit       = stoi(argv[3]);
        prm.target      = stod(argv[4]);
        prm.simulations = stoi(argv[5]);

        if (!second.empty() && second.back() == '%')
        {
            timeMode = true;
            percent = stod(second.substr(0, second.size() - 1));
        }
        else
        {
            double sec = parseDuration(second);
            if (sec < 0)
            {
                cerr << "時間の書式が不正です: " << second
                     << " (例: 24, 24h, 90m, 1h30m20s)\n";
                return 1;
            }
            trials = llround(sec / TRIAL_INTERVAL_SEC);
        }

        for (int i = 6; i < argc; i++)
            if (string(argv[i]) == "--json")
                jsonMode = true;
    }
    else
    {
        int mode;
        cout << "モード (1=必要確率を求める, 2=所要時間を推定): ";
        cin >> mode;
        timeMode = (mode == 2);

        cout << "ユーザー数: ";
        cin >> prm.users;

        if (timeMode)
        {
            cout << "当選確率(%): ";
            cin >> percent;
        }
        else
        {
            string s;
            cout << "時間 (例: 24, 24h, 90m, 1h30m20s): ";
            cin >> s;

            double sec = parseDuration(s);
            if (sec < 0)
            {
                cerr << "時間の書式が不正です\n";
                return 1;
            }
            trials = llround(sec / TRIAL_INTERVAL_SEC);
        }

        cout << "1人あたりの成功上限: ";
        cin >> prm.limit;

        cout << "目標成功数: ";
        cin >> prm.target;

        cout << "シミュレーション数(1000推奨): ";
        cin >> prm.simulations;
    }

    if (prm.users <= 0 || prm.simulations <= 0 || prm.limit <= 0)
    {
        cerr << "入力値が不正です\n";
        return 1;
    }

    if (timeMode)
        return runTimeSearch(prm, percent, jsonMode);
    else
        return runPercentSearch(prm, trials, jsonMode);
}
