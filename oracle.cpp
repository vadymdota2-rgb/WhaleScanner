#include "oracle.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <sqlite3.h>
#include "json.hpp"
#include "hyperliquid.h"
#include "hyperliquid_internal.h"
#include "utils.h"

using json = nlohmann::json;

extern sqlite3* db;
extern std::mutex dbMutex;

namespace {

constexpr int ORACLE_BINS = 64;          // столбцов в гистограмме признака
constexpr int ORACLE_DEPTH = 3;          // глубина дерева: 8 листьев
constexpr int ORACLE_MAX_TREES = 300;
constexpr double ORACLE_LR = 0.05;
constexpr double ORACLE_L2 = 1.0;        // регуляризация листа
constexpr int ORACLE_MIN_LEAF = 20;      // примеров в листе
constexpr double ORACLE_MIN_H = 4.0;     // суммарный гессиан в листе
constexpr int ORACLE_PATIENCE = 25;      // деревьев без улучшения
/* Раньше этого порога ранняя остановка не срабатывает. Взаимодействие
   признаков — «приток помогает только при низкой волатильности» — жадное
   дерево не видит на первом же разбиении: по отдельности ни поток, ни
   волатильность класс не разделяют. Модель находит такое через несколько
   деревьев, а проверка на четырёхстах примерах шумит и успевает остановить
   обучение на первом. */
constexpr int ORACLE_MIN_TREES = 60;
constexpr int ORACLE_MIN_SAMPLES = 300;  // меньше — обучать нечего
constexpr double ORACLE_MIN_MOVE = 0.02; // движение меньше — шум, как в ai.cpp

double sigmoid(double z) {
    if (z > 30) return 1.0;
    if (z < -30) return 0.0;
    return 1.0 / (1.0 + std::exp(-z));
}

double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* Порог «движение, а не шум» зависит от окна: за шесть часов цена проходит
   меньше, чем за сутки, и требовать от неё тех же двух процентов — значит
   оставить шестичасовому горизонту одни только обвалы. Масштаб — корень из
   времени, как у волатильности случайного блуждания: на шести часах порог
   получается ровно вдвое мягче. */
inline double oracleMinMove(long long horizon) {
    const double k = std::sqrt(static_cast<double>(horizon) /
                               static_cast<double>(ORACLE_H24));
    return ORACLE_MIN_MOVE * clampd(k, 0.25, 1.0);
}


double usdOf(long long nanos) { return static_cast<double>(nanos) / 1e9; }

/* ---------------------------------------------------------------- лес --- */

struct Node {
    int feat = -1;       // < 0 — лист
    float thr = 0;       // граница по значению признака
    int left = -1;
    int right = -1;
    float leaf = 0;      // уже умножен на скорость обучения
};

struct Tree {
    std::vector<Node> nodes;

    double predict(const float* f) const {
        if (nodes.empty()) return 0;
        int i = 0;
        for (int guard = 0; guard < 64; guard++) {
            const Node& n = nodes[static_cast<size_t>(i)];
            if (n.feat < 0) return n.leaf;
            i = (f[n.feat] <= n.thr) ? n.left : n.right;
            if (i < 0 || i >= static_cast<int>(nodes.size())) return 0;
        }
        return 0;
    }
};

struct Forest {
    bool squared = false;            // квадратичная функция потерь — лес про величину
    double base = 0;                 // логит базовой ставки
    double calA = 1.0, calB = 0.0;   // калибровка Платта поверх суммы
    std::vector<Tree> trees;
    /* Срединные значения признаков на обучении. Нужны, чтобы объяснить
       отдельную оценку: подставляем середину вместо признака и смотрим, куда
       уехала вероятность. Ноль вместо середины не годится — у объёма и числа
       кошельков ноль означает «сделок не было», а не «обычный день». */
    std::array<double, ORACLE_NF> med{};

    double raw(const float* f) const {
        double z = base;
        for (const Tree& t : trees) z += t.predict(f);
        return z;
    }
    double p(const float* f) const { return sigmoid(calA * raw(f) + calB); }
    /* Для леса про величину калибровка не нужна: он и так предсказывает само
       число, а не логит. Отрицательный ход невозможен — это максимум
       отклонения, он не бывает меньше нуля. */
    double value(const float* f) const { return std::max(0.0, raw(f)); }
};

/* Одна строка журнала — три ответа сразу: пошла ли цена в сторону сигнала и
   как далеко она уходила в обе стороны за сутки. Первое учит направление,
   второе и третье — уровни. */
struct Sample {
    std::array<float, ORACLE_NF> f{};
    float y = 0;        // 1 — выросла за сутки
    float up = 0;       // максимум хода вверх от входа, доля
    float dn = 0;       // максимум хода вниз от входа, доля
    float w = 1;
    long long ts = 0;
};

/* Какую величину учим. У направления логистическая функция потерь, у хода —
   квадратичная: дерево то же, меняются только градиент с гессианом. */
enum class Task { Dir, Up, Dn };

float targetOf(const Sample& s, Task t) {
    switch (t) {
        case Task::Up: return s.up;
        case Task::Dn: return s.dn;
        default: return s.y;
    }
}

/* ------------------------------------------------------------ метрики --- */

double aucOf(const std::vector<double>& p, const std::vector<float>& y) {
    const size_t n = p.size();
    if (n < 2) return 0.5;
    std::vector<size_t> idx(n);
    for (size_t i = 0; i < n; i++) idx[i] = i;
    std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return p[a] < p[b]; });
    // средние ранги на связках — иначе константный прогноз даёт AUC 1 или 0
    std::vector<double> rank(n, 0);
    size_t i = 0;
    while (i < n) {
        size_t j = i;
        while (j + 1 < n && p[idx[j + 1]] == p[idx[i]]) j++;
        const double r = (static_cast<double>(i) + static_cast<double>(j)) / 2.0 + 1.0;
        for (size_t k = i; k <= j; k++) rank[idx[k]] = r;
        i = j + 1;
    }
    double sumPos = 0;
    size_t nPos = 0;
    for (size_t k = 0; k < n; k++) {
        if (y[k] > 0.5) { sumPos += rank[k]; nPos++; }
    }
    const size_t nNeg = n - nPos;
    if (nPos == 0 || nNeg == 0) return 0.5;
    const double a = (sumPos - static_cast<double>(nPos) * (static_cast<double>(nPos) + 1) / 2.0)
                   / (static_cast<double>(nPos) * static_cast<double>(nNeg));
    return clampd(a, 0.0, 1.0);
}

double loglossOf(const std::vector<double>& p, const std::vector<float>& y) {
    if (p.empty()) return 0;
    double s = 0;
    for (size_t i = 0; i < p.size(); i++) {
        const double q = clampd(p[i], 1e-6, 1 - 1e-6);
        s += y[i] > 0.5 ? -std::log(q) : -std::log(1 - q);
    }
    return s / static_cast<double>(p.size());
}

double brierOf(const std::vector<double>& p, const std::vector<float>& y) {
    if (p.empty()) return 0;
    double s = 0;
    for (size_t i = 0; i < p.size(); i++) {
        const double d = p[i] - static_cast<double>(y[i]);
        s += d * d;
    }
    return s / static_cast<double>(p.size());
}

double accOf(const std::vector<double>& p, const std::vector<float>& y) {
    if (p.empty()) return 0;
    size_t hit = 0;
    for (size_t i = 0; i < p.size(); i++)
        if ((p[i] > 0.5) == (y[i] > 0.5)) hit++;
    return static_cast<double>(hit) / static_cast<double>(p.size());
}

/* Калибровка Платта: сумму деревьев тянем к настоящей частоте. Бустинг
   уверен сильнее, чем имеет право, и без этой поправки «85%» на экране
   означало бы 60% на деле. */
void fitPlatt(const std::vector<double>& z, const std::vector<float>& y, double& a, double& b) {
    a = 1.0; b = 0.0;
    if (z.size() < 30) return;
    for (int it = 0; it < 60; it++) {
        double ga = 0, gb = 0, haa = 0, hab = 0, hbb = 0;
        for (size_t i = 0; i < z.size(); i++) {
            const double p = sigmoid(a * z[i] + b);
            const double e = p - static_cast<double>(y[i]);
            const double h = std::max(1e-6, p * (1 - p));
            ga += e * z[i];
            gb += e;
            haa += h * z[i] * z[i];
            hab += h * z[i];
            hbb += h;
        }
        const double det = haa * hbb - hab * hab;
        if (std::fabs(det) < 1e-12) break;
        const double da = (ga * hbb - gb * hab) / det;
        const double dbb = (gb * haa - ga * hab) / det;
        a -= da; b -= dbb;
        if (!std::isfinite(a) || !std::isfinite(b)) { a = 1.0; b = 0.0; return; }
        if (std::fabs(da) < 1e-9 && std::fabs(dbb) < 1e-9) break;
    }
    a = clampd(a, 0.05, 5.0);
    b = clampd(b, -5.0, 5.0);
}

/* ---------------------------------------------------- обучение деревьев --- */

/* Границы столбцов по квантилям обучающей части. Квантили, а не равные
   отрезки: у объёма и ликвидности хвост в тысячи раз длиннее середины, и
   равные отрезки собрали бы девяносто процентов примеров в один столбец. */
struct Bins {
    std::array<std::vector<float>, ORACLE_NF> edge;   // границы, по возрастанию

    void build(const std::vector<Sample>& xs) {
        std::vector<float> col;
        for (int f = 0; f < ORACLE_NF; f++) {
            col.clear();
            col.reserve(xs.size());
            for (const Sample& s : xs) {
                const float v = s.f[static_cast<size_t>(f)];
                if (std::isfinite(v)) col.push_back(v);
            }
            std::sort(col.begin(), col.end());
            col.erase(std::unique(col.begin(), col.end()), col.end());
            auto& e = edge[static_cast<size_t>(f)];
            e.clear();
            if (col.size() < 2) continue;
            const int want = std::min<int>(ORACLE_BINS - 1, static_cast<int>(col.size()) - 1);
            for (int k = 1; k <= want; k++) {
                const size_t at = col.size() * static_cast<size_t>(k) / static_cast<size_t>(want + 1);
                const float v = col[std::min(at, col.size() - 1)];
                if (e.empty() || v > e.back()) e.push_back(v);
            }
        }
    }

    uint8_t binOf(int f, float v) const {
        const auto& e = edge[static_cast<size_t>(f)];
        if (e.empty() || !std::isfinite(v)) return 0;
        const size_t at = static_cast<size_t>(
            std::upper_bound(e.begin(), e.end(), v) - e.begin());
        return static_cast<uint8_t>(std::min<size_t>(at, e.size()));
    }
    int count(int f) const { return static_cast<int>(edge[static_cast<size_t>(f)].size()) + 1; }
    float edgeOf(int f, int b) const {
        const auto& e = edge[static_cast<size_t>(f)];
        if (e.empty()) return 0;
        const size_t i = std::min<size_t>(static_cast<size_t>(b), e.size() - 1);
        return e[i];
    }
};

struct Hist { double g = 0, h = 0; int n = 0; };

struct Grower {
    const std::vector<Sample>* xs = nullptr;
    const std::vector<uint8_t>* bin = nullptr;   // n × ORACLE_NF
    const Bins* bins = nullptr;
    const std::vector<double>* g = nullptr;
    const std::vector<double>* h = nullptr;
    Tree tree;

    uint8_t at(size_t row, int f) const {
        return (*bin)[row * static_cast<size_t>(ORACLE_NF) + static_cast<size_t>(f)];
    }

    bool squared = false;
    double leafValue(const std::vector<size_t>& rows) const {
        double sg = 0, sh = 0;
        for (size_t r : rows) { sg += (*g)[r]; sh += (*h)[r]; }
        const double v = -sg / (sh + ORACLE_L2);
        // У логита ±4 это край разумного; у доли хода такой зажим ничего не
        // значит, зато мешает: ход в десять процентов лес бы не выучил.
        return (squared ? clampd(v, -1.0, 1.0) : clampd(v, -4.0, 4.0)) * ORACLE_LR;
    }

    /* Лучшее разбиение узла: перебор по столбцам гистограммы. Выигрыш —
       обычный для бустинга: сумма квадратов градиента по половинам минус то
       же по целому. */
    bool bestSplit(const std::vector<size_t>& rows, int& outF, int& outB, double& outGain) const {
        double totG = 0, totH = 0;
        for (size_t r : rows) { totG += (*g)[r]; totH += (*h)[r]; }
        const double whole = totG * totG / (totH + ORACLE_L2);
        outGain = 0; outF = -1; outB = -1;
        std::array<Hist, ORACLE_BINS> hist{};
        for (int f = 0; f < ORACLE_NF; f++) {
            const int nb = bins->count(f);
            if (nb < 2) continue;
            for (int b = 0; b < nb; b++) hist[static_cast<size_t>(b)] = Hist{};
            for (size_t r : rows) {
                Hist& c = hist[at(r, f)];
                c.g += (*g)[r]; c.h += (*h)[r]; c.n++;
            }
            double lg = 0, lh = 0;
            int ln = 0;
            for (int b = 0; b + 1 < nb; b++) {
                const Hist& c = hist[static_cast<size_t>(b)];
                lg += c.g; lh += c.h; ln += c.n;
                const int rn = static_cast<int>(rows.size()) - ln;
                if (ln < ORACLE_MIN_LEAF || rn < ORACLE_MIN_LEAF) continue;
                const double rh = totH - lh;
                if (lh < ORACLE_MIN_H || rh < ORACLE_MIN_H) continue;
                const double rg = totG - lg;
                const double gain = lg * lg / (lh + ORACLE_L2) + rg * rg / (rh + ORACLE_L2) - whole;
                if (gain > outGain) { outGain = gain; outF = f; outB = b; }
            }
        }
        return outF >= 0 && outGain > 1e-6;
    }

    int grow(const std::vector<size_t>& rows, int depth) {
        const int me = static_cast<int>(tree.nodes.size());
        tree.nodes.push_back(Node{});
        int f = -1, b = -1;
        double gain = 0;
        if (depth >= ORACLE_DEPTH || rows.size() < static_cast<size_t>(2 * ORACLE_MIN_LEAF) ||
            !bestSplit(rows, f, b, gain)) {
            tree.nodes[static_cast<size_t>(me)].feat = -1;
            tree.nodes[static_cast<size_t>(me)].leaf = static_cast<float>(leafValue(rows));
            return me;
        }
        std::vector<size_t> lr, rr;
        lr.reserve(rows.size()); rr.reserve(rows.size());
        for (size_t r : rows) {
            if (at(r, f) <= static_cast<uint8_t>(b)) lr.push_back(r);
            else rr.push_back(r);
        }
        tree.nodes[static_cast<size_t>(me)].feat = f;
        tree.nodes[static_cast<size_t>(me)].thr = bins->edgeOf(f, b);
        const int l = grow(lr, depth + 1);
        const int r2 = grow(rr, depth + 1);
        tree.nodes[static_cast<size_t>(me)].left = l;
        tree.nodes[static_cast<size_t>(me)].right = r2;
        return me;
    }
};

struct Fit {
    Forest forest;
    std::array<double, ORACLE_NF> gain{};   // сколько каждый признак дал выигрыша
    int trees = 0;
};

/* Обучение с ранней остановкой по проверочной части. Никакой случайности:
   те же данные дают тот же лес, и разбор расхождений не превращается в
   гадание. */
Fit trainForest(const std::vector<Sample>& tr, const std::vector<Sample>& va, Task task = Task::Dir) {
    Fit out;
    if (tr.size() < static_cast<size_t>(ORACLE_MIN_SAMPLES)) return out;

    Bins bins;
    bins.build(tr);
    const size_t n = tr.size();
    std::vector<uint8_t> bin(n * static_cast<size_t>(ORACLE_NF), 0);
    for (size_t i = 0; i < n; i++)
        for (int f = 0; f < ORACLE_NF; f++)
            bin[i * static_cast<size_t>(ORACLE_NF) + static_cast<size_t>(f)] =
                bins.binOf(f, tr[i].f[static_cast<size_t>(f)]);

    const bool sq = task != Task::Dir;
    out.forest.squared = sq;
    if (sq) {
        // Начинаем со среднего хода: дальше деревья правят его по признакам.
        double sum = 0;
        for (const Sample& s : tr) sum += targetOf(s, task);
        out.forest.base = sum / static_cast<double>(n);
    } else {
        double pos = 0;
        for (const Sample& s : tr) pos += s.y;
        const double rate = clampd(pos / static_cast<double>(n), 1e-3, 1 - 1e-3);
        out.forest.base = std::log(rate / (1 - rate));
    }

    std::vector<double> F(n, out.forest.base), g(n, 0), h(n, 0);
    std::vector<double> Fv(va.size(), out.forest.base);
    std::vector<float> yv(va.size(), 0);
    for (size_t i = 0; i < va.size(); i++) yv[i] = targetOf(va[i], task);

    double bestLoss = 1e9;
    int bestAt = 0;
    std::vector<size_t> all(n);
    for (size_t i = 0; i < n; i++) all[i] = i;

    for (int it = 0; it < ORACLE_MAX_TREES; it++) {
        for (size_t i = 0; i < n; i++) {
            const double w = tr[i].w;
            if (sq) {
                g[i] = (F[i] - static_cast<double>(targetOf(tr[i], task))) * w;
                h[i] = w;                       // у квадрата вторая производная постоянна
            } else {
                const double p = sigmoid(F[i]);
                g[i] = (p - static_cast<double>(tr[i].y)) * w;
                h[i] = std::max(1e-6, p * (1 - p)) * w;
            }
        }
        Grower gr;
        gr.xs = &tr; gr.bin = &bin; gr.bins = &bins; gr.g = &g; gr.h = &h;
        gr.squared = sq;
        gr.grow(all, 0);
        if (gr.tree.nodes.size() <= 1 && gr.tree.nodes[0].feat < 0 &&
            std::fabs(static_cast<double>(gr.tree.nodes[0].leaf)) < 1e-9)
            break;                                  // учить больше нечему

        for (const Node& nd : gr.tree.nodes)
            if (nd.feat >= 0) out.gain[static_cast<size_t>(nd.feat)] += 1.0;

        for (size_t i = 0; i < n; i++) F[i] += gr.tree.predict(tr[i].f.data());
        out.forest.trees.push_back(gr.tree);

        if (!va.empty()) {
            std::vector<double> pv(va.size());
            for (size_t i = 0; i < va.size(); i++) {
                Fv[i] += out.forest.trees.back().predict(va[i].f.data());
                pv[i] = sq ? Fv[i] : sigmoid(Fv[i]);
            }
            double loss = 0;
            if (sq) {
                for (size_t i = 0; i < va.size(); i++) {
                    const double d = pv[i] - static_cast<double>(yv[i]);
                    loss += d * d;
                }
                loss /= static_cast<double>(va.size());
            } else {
                loss = loglossOf(pv, yv);
            }
            if (loss < bestLoss - 1e-6) { bestLoss = loss; bestAt = static_cast<int>(out.forest.trees.size()); }
            else if (static_cast<int>(out.forest.trees.size()) >= ORACLE_MIN_TREES &&
                     static_cast<int>(out.forest.trees.size()) - bestAt >= ORACLE_PATIENCE) break;
        }
    }
    // Обрезаем до лучшего шага, но не короче порога: лес из одного дерева —
    // это не «рано остановились», это «не успели начать».
    if (!va.empty() && bestAt > 0 &&
        bestAt < static_cast<int>(out.forest.trees.size())) {
        const size_t keep = std::max<size_t>(static_cast<size_t>(bestAt),
                                             std::min<size_t>(ORACLE_MIN_TREES,
                                                              out.forest.trees.size()));
        out.forest.trees.resize(keep);
    }
    out.trees = static_cast<int>(out.forest.trees.size());

    {
        std::vector<float> col;
        for (int f = 0; f < ORACLE_NF; f++) {
            col.clear();
            col.reserve(tr.size());
            for (const Sample& s : tr) col.push_back(s.f[static_cast<size_t>(f)]);
            std::sort(col.begin(), col.end());
            out.forest.med[static_cast<size_t>(f)] =
                col.empty() ? 0.0 : static_cast<double>(col[col.size() / 2]);
        }
    }

    if (!va.empty() && !sq) {
        std::vector<double> zv(va.size());
        for (size_t i = 0; i < va.size(); i++) zv[i] = out.forest.raw(va[i].f.data());
        fitPlatt(zv, yv, out.forest.calA, out.forest.calB);
    }
    return out;
}

/* ------------------------------------------------------------- рынок --- */

struct Bar {
    long long ts = 0;     // начало часа
    double c = 0;         // закрытие
    double hi = 0, lo = 0;
    double v = 0;         // объём за час
    double oi = 0;        // открытый интерес, доллары
    double fund = 0;      // ставка фандинга за час
};

struct Series {
    std::vector<Bar> bars;   // по возрастанию времени

    /* Последний бар не позже asOf. Именно здесь проходит граница между
       «модель знает» и «модель подглядывает в будущее»: всё, что после,
       для признаков не существует. */
    int at(long long asOf) const {
        if (bars.empty()) return -1;
        int lo = 0, hi = static_cast<int>(bars.size()) - 1, best = -1;
        while (lo <= hi) {
            const int mid = (lo + hi) / 2;
            if (bars[static_cast<size_t>(mid)].ts <= asOf) { best = mid; lo = mid + 1; }
            else hi = mid - 1;
        }
        return best;
    }
};

struct Market {
    std::unordered_map<std::string, Series> perp;
    std::unordered_map<std::string, Series> spot;
    Series btc;
    std::vector<std::pair<long long, double>> breadth;  // час → доля растущих
    long long builtAt = 0;

    double breadthAt(long long asOf) const {
        if (breadth.empty()) return 0.0;
        int lo = 0, hi = static_cast<int>(breadth.size()) - 1, best = -1;
        while (lo <= hi) {
            const int mid = (lo + hi) / 2;
            if (breadth[static_cast<size_t>(mid)].first <= asOf) { best = mid; lo = mid + 1; }
            else hi = mid - 1;
        }
        return best < 0 ? 0.0 : breadth[static_cast<size_t>(best)].second;
    }
};

/* Ряды держим под общим указателем: перестройка меняет указатель одним
   присваиванием, а счёт по монете держит свою копию указателя и работает без
   замка. Копировать сами ряды на каждую монету было бы десятки мегабайт. */
std::mutex g_mktMutex;
std::shared_ptr<const Market> g_mkt;

std::shared_ptr<const Market> market() {
    std::lock_guard<std::mutex> l(g_mktMutex);
    return g_mkt;
}

long long hourFloor(long long sec) { return sec / 3600 * 3600; }

double retOver(const Series& s, int i, int hours) {
    if (i < hours || hours <= 0) return 0;
    const double a = s.bars[static_cast<size_t>(i - hours)].c;
    const double b = s.bars[static_cast<size_t>(i)].c;
    if (a <= 0 || b <= 0) return 0;
    return clampd(b / a - 1.0, -0.9, 3.0);
}

double volOver(const Series& s, int i, int hours) {
    if (i < hours + 1 || hours < 4) return 0;
    double sum = 0, sum2 = 0;
    int n = 0;
    for (int k = i - hours + 1; k <= i; k++) {
        const double a = s.bars[static_cast<size_t>(k - 1)].c;
        const double b = s.bars[static_cast<size_t>(k)].c;
        if (a <= 0 || b <= 0) continue;
        const double r = std::log(b / a);
        if (!std::isfinite(r) || std::fabs(r) > 1.5) continue;
        sum += r; sum2 += r * r; n++;
    }
    if (n < 4) return 0;
    const double m = sum / n;
    const double var = std::max(0.0, sum2 / n - m * m);
    return std::sqrt(var);
}

double atrOver(const Series& s, int i, int hours) {
    if (i < hours || hours < 2) return 0;
    double sum = 0;
    int n = 0;
    for (int k = i - hours + 1; k <= i; k++) {
        const Bar& b = s.bars[static_cast<size_t>(k)];
        if (b.hi <= 0 || b.lo <= 0 || b.c <= 0) continue;
        const double prev = s.bars[static_cast<size_t>(k - 1)].c;
        double tr = b.hi - b.lo;
        if (prev > 0) {
            tr = std::max(tr, std::fabs(b.hi - prev));
            tr = std::max(tr, std::fabs(b.lo - prev));
        }
        sum += tr / b.c;
        n++;
    }
    return n < 2 ? 0 : clampd(sum / n, 0.0, 1.0);
}

double rsiOver(const Series& s, int i, int hours) {
    if (i < hours || hours < 2) return 0;
    double up = 0, dn = 0;
    for (int k = i - hours + 1; k <= i; k++) {
        const double a = s.bars[static_cast<size_t>(k - 1)].c;
        const double b = s.bars[static_cast<size_t>(k)].c;
        if (a <= 0 || b <= 0) continue;
        const double d = b - a;
        if (d >= 0) up += d; else dn -= d;
    }
    if (up + dn <= 0) return 0;
    return 100.0 * up / (up + dn);
}

/* Где цена внутри недельного коридора: 0 — на минимуме, 1 — на максимуме.
   Признак говорит модели то, чего не скажет доходность: рост на 5% у самого
   потолка и тот же рост со дна — разные истории. */
void bandOver(const Series& s, int i, int hours, double& fromHigh, double& fromLow) {
    fromHigh = 0; fromLow = 0;
    if (i < 2) return;
    const int from = std::max(0, i - hours + 1);
    double hi = 0, lo = 0;
    for (int k = from; k <= i; k++) {
        const Bar& b = s.bars[static_cast<size_t>(k)];
        const double h = b.hi > 0 ? b.hi : b.c;
        const double l = b.lo > 0 ? b.lo : b.c;
        if (h <= 0 || l <= 0) continue;
        if (hi == 0 || h > hi) hi = h;
        if (lo == 0 || l < lo) lo = l;
    }
    const double px = s.bars[static_cast<size_t>(i)].c;
    if (px <= 0 || hi <= 0 || lo <= 0) return;
    fromHigh = clampd(px / hi - 1.0, -1.0, 0.0);
    fromLow = clampd(px / lo - 1.0, 0.0, 5.0);
}

double smaRatio(const Series& s, int i, int hours) {
    if (i < hours || hours < 2) return 0;
    double sum = 0;
    int n = 0;
    for (int k = i - hours + 1; k <= i; k++) {
        const double c = s.bars[static_cast<size_t>(k)].c;
        if (c > 0) { sum += c; n++; }
    }
    const double px = s.bars[static_cast<size_t>(i)].c;
    if (n < 2 || px <= 0) return 0;
    const double m = sum / n;
    return m > 0 ? clampd(px / m - 1.0, -0.9, 3.0) : 0;
}

double changeOf(double now, double before) {
    if (before <= 0 || now <= 0) return 0;
    return clampd(now / before - 1.0, -1.0, 5.0);
}

/* MACD: разность быстрой и медленной скользящих, её сигнальная линия и
   разность между ними. Считается по часовым закрытиям и делится на цену —
   иначе у биткоина и у мем-монеты числа несравнимы.
 
   Экспоненциальная средняя разгоняется с первого бара ряда, а не с i: иначе
   у двух событий по одной монете в разные часы был бы разный разогрев и
   разные признаки на одних и тех же данных. */
void macdOf(const Series& s, int i, double& line, double& sig, double& hist) {
    line = sig = hist = 0;
    if (i < 26) return;
    const double kf = 2.0 / 13.0, ks = 2.0 / 27.0, kg = 2.0 / 10.0;
    double fast = 0, slow = 0, signal = 0;
    bool started = false;
    int n = 0;
    for (int k = std::max(0, i - 400); k <= i; k++) {
        const double c = s.bars[static_cast<size_t>(k)].c;
        if (c <= 0) continue;
        if (!started) { fast = slow = c; started = true; }
        else {
            fast += kf * (c - fast);
            slow += ks * (c - slow);
        }
        const double m = fast - slow;
        if (n == 0) signal = m;
        else signal += kg * (m - signal);
        n++;
    }
    const double px = s.bars[static_cast<size_t>(i)].c;
    if (px <= 0 || n < 26) return;
    line = clampd((fast - slow) / px, -0.5, 0.5);
    sig = clampd(signal / px, -0.5, 0.5);
    hist = clampd(line - sig, -0.5, 0.5);
}

double fundingZ(const Series& s, int i, int hours) {
    if (i < hours || hours < 8) return 0;
    double sum = 0, sum2 = 0;
    int n = 0;
    for (int k = i - hours + 1; k <= i; k++) {
        const double f = s.bars[static_cast<size_t>(k)].fund;
        sum += f; sum2 += f * f; n++;
    }
    if (n < 8) return 0;
    const double m = sum / n;
    const double sd = std::sqrt(std::max(1e-18, sum2 / n - m * m));
    if (sd <= 1e-12) return 0;
    return clampd((s.bars[static_cast<size_t>(i)].fund - m) / sd, -5.0, 5.0);
}

/* Как далеко цена уходила от входа за горизонт — вверх и вниз.
 *
 * Это и есть разметка для уровней: стоп имеет смысл ставить за пределом
 * обычного отката, а цель — там, куда цена обычно доходит. У перпов есть
 * настоящие максимум и минимум часа, у спота только цена закрытия, и по
 * закрытиям ход виден мельче, чем он был. */
void excursion(const Series& s, long long from, long long to, double entry,
               double& up, double& dn) {
    up = dn = 0;
    if (entry <= 0) return;
    const int i = s.at(from);
    if (i < 0) return;
    for (size_t k = static_cast<size_t>(i); k < s.bars.size(); k++) {
        const Bar& b = s.bars[k];
        if (b.ts < from) continue;
        if (b.ts > to) break;
        const double hi = b.hi > 0 ? b.hi : b.c;
        const double lo = b.lo > 0 ? b.lo : b.c;
        if (hi > 0) up = std::max(up, hi / entry - 1.0);
        if (lo > 0) dn = std::max(dn, 1.0 - lo / entry);
    }
    up = clampd(up, 0.0, 3.0);
    dn = clampd(dn, 0.0, 1.0);
}

/* ---------------------------------------------------------- признаки --- */

/* Имена короткие и техничные, как на прежнем экране состояния: их видят
   и в боте, и в приложении на шестнадцати языках, а «volatility» и «funding»
   читаются одинаково везде — переводить такое хуже, чем оставить. */
const char* const FEAT_NAME[ORACLE_NF] = {
    "flow",      "volume",    "wallets",  "spread",    "accel",
    "trades",    "ticket",    "top100",   "top dir",   "both",
    "ret 1h",    "ret 6h",    "ret 24h",  "vol 24h",   "vol jump",
    "to high",   "from low",  "RSI",      "trend",     "ATR",
    "funding",   "funding z", "OI 1h",    "OI 24h",    "OI/vlm",
    "vlm 24h",   "liq skew",  "liq/OI",   "leverage",  "liquidity",
    "BTC 24h",   "BTC vol",   "breadth",  "hour",      "hour 2",
    "MACD",      "MACD sig",  "MACD hist",
};

/* Единственное место, где считаются признаки. При обучении сюда приходит
   время события из журнала, при живом счёте — сейчас; ряды одни и те же, и
   потому обученное и применённое совпадают. Разойдись эти два пути — модель
   в бою вела бы себя не так, как на проверке, и понять это было бы нельзя. */
void featuresOf(const Market& m, const OracleInput& in, long long asOf,
                std::array<float, ORACLE_NF>& f) {
    f.fill(0.0f);

    const long long vol = in.buy + in.sell;
    const double usd = static_cast<double>(vol) / 1e9;
    f[0] = static_cast<float>(vol > 0
        ? static_cast<double>(in.buy - in.sell) / static_cast<double>(vol) : 0.0);
    f[1] = static_cast<float>(std::log1p(std::max(0.0, usd)) / std::log1p(1e6));
    f[2] = static_cast<float>(std::log1p(static_cast<double>(in.wallets)) / std::log1p(50.0));
    f[3] = static_cast<float>(1.0 - clampd(in.oneShare, 0.0, 1.0));
    {
        const double a = static_cast<double>(in.net6h);
        const double b = static_cast<double>(in.netPrior) / 3.0;
        const double d = std::fabs(a) + std::fabs(b);
        f[4] = static_cast<float>(d < 1 ? 0.0 : clampd((a - b) / d, -1.0, 1.0));
    }
    {
        const int nt = in.nBuy + in.nSell;
        f[5] = static_cast<float>(nt > 0
            ? static_cast<double>(in.nBuy - in.nSell) / static_cast<double>(nt) : 0.0);
        f[6] = static_cast<float>(nt > 0
            ? std::log1p(usd / nt) / std::log1p(100000.0) : 0.0);
    }
    f[7] = static_cast<float>(clampd(in.topShare, 0.0, 1.0));
    f[8] = static_cast<float>(clampd(in.topDir, -1.0, 1.0));
    f[9] = static_cast<float>(in.bothVenues ? 1.0 : 0.0);

    const Series* s = nullptr;
    if (in.perp) {
        auto it = m.perp.find(in.id);
        if (it != m.perp.end()) s = &it->second;
    } else {
        auto it = m.spot.find(toLower(in.id));
        if (it != m.spot.end()) s = &it->second;
    }
    const int i = s ? s->at(asOf) : -1;
    if (s && i >= 1) {
        f[10] = static_cast<float>(retOver(*s, i, 1));
        f[11] = static_cast<float>(retOver(*s, i, 6));
        f[12] = static_cast<float>(retOver(*s, i, 24));
        const double v24 = volOver(*s, i, 24);
        const double v168 = volOver(*s, i, 168);
        f[13] = static_cast<float>(clampd(v24, 0.0, 1.0));
        f[14] = static_cast<float>(v168 > 1e-9 ? clampd(v24 / v168, 0.0, 10.0) : 0.0);
        double fh = 0, fl = 0;
        bandOver(*s, i, 168, fh, fl);
        f[15] = static_cast<float>(fh);
        f[16] = static_cast<float>(fl);
        const double rsi = rsiOver(*s, i, 14);
        f[17] = static_cast<float>(rsi > 0 ? (rsi - 50.0) / 50.0 : 0.0);
        f[18] = static_cast<float>(smaRatio(*s, i, 24));
        f[19] = static_cast<float>(atrOver(*s, i, 14));
    }

    if (in.perp) {
        f[20] = static_cast<float>(std::tanh(static_cast<double>(in.funding) / 1e7));
        if (s && i >= 0) {
            f[21] = static_cast<float>(fundingZ(*s, i, 168));
            const double oiNow = s->bars[static_cast<size_t>(i)].oi;
            if (i >= 1)
                f[22] = static_cast<float>(changeOf(oiNow, s->bars[static_cast<size_t>(i - 1)].oi));
            if (i >= 24)
                f[23] = static_cast<float>(changeOf(oiNow, s->bars[static_cast<size_t>(i - 24)].oi));
            const double vlm = s->bars[static_cast<size_t>(i)].v;
            f[24] = static_cast<float>(vlm > 0 && oiNow > 0
                ? clampd(oiNow / vlm, 0.0, 50.0) : 0.0);
            if (i >= 24)
                f[25] = static_cast<float>(changeOf(vlm, s->bars[static_cast<size_t>(i - 24)].v));
            const double liq = static_cast<double>(in.liqLong + in.liqShort) / 1e9;
            f[27] = static_cast<float>(oiNow > 0 ? clampd(liq / oiNow, 0.0, 2.0) : 0.0);
        }
        const long long lq = in.liqLong + in.liqShort;
        f[26] = static_cast<float>(lq > 0
            ? static_cast<double>(in.liqShort - in.liqLong) / static_cast<double>(lq) : 0.0);
        f[28] = static_cast<float>(clampd(in.levBp / 5000.0, 0.0, 5.0));
    } else {
        f[29] = static_cast<float>(
            std::log1p(std::max(0.0, in.liqUsd)) / std::log1p(5e6));
    }

    const int bi = m.btc.at(asOf);
    if (bi >= 1) {
        f[30] = static_cast<float>(retOver(m.btc, bi, 24));
        f[31] = static_cast<float>(clampd(volOver(m.btc, bi, 24), 0.0, 1.0));
    }
    f[32] = static_cast<float>(m.breadthAt(asOf));

    const double hour = static_cast<double>((asOf % 86400) / 3600);
    f[33] = static_cast<float>(std::sin(2 * M_PI * hour / 24.0));
    f[34] = static_cast<float>(std::cos(2 * M_PI * hour / 24.0));

    if (s && i >= 26) {
        double line = 0, sig = 0, hist = 0;
        macdOf(*s, i, line, sig, hist);
        f[35] = static_cast<float>(line);
        f[36] = static_cast<float>(sig);
        f[37] = static_cast<float>(hist);
    }

    for (int k = 0; k < ORACLE_NF; k++)
        if (!std::isfinite(f[static_cast<size_t>(k)])) f[static_cast<size_t>(k)] = 0.0f;
}

/* --------------------------------------------- свечи с самой биржи --- */

constexpr long long ORACLE_HISTORY = 95LL * 86400LL;   // чуть больше срока журнала
/* Запросов за один заход. Тик оракула живёт в общем потоке обслуживания
   базы: он ходит раз в минуту и ничего срочного не делает, но занимать его
   на минуты всё равно нельзя. Восемь запросов — это худшие две минуты при
   мёртвой сети и обычные полсекунды при живой. Полный круг по сотне монет
   занимает около часа, и этого хватает: максимум с минимумом за час меняются
   медленно, а цену закрытия бот и так пишет сам каждый час. */
constexpr int ORACLE_CANDLE_BATCH = 8;
constexpr int ORACLE_WEIGHT_CANDLE = 2;

void ensureCandleSchema() {
    std::lock_guard<std::mutex> l(hl::g_hlDbMutex);
    if (!hl::g_hlDb) return;
    const char* schema =
        "CREATE TABLE IF NOT EXISTS hl_candles ("
        "  coin TEXT NOT NULL,"
        "  hour_ts INTEGER NOT NULL,"
        "  o REAL NOT NULL DEFAULT 0,"
        "  h REAL NOT NULL DEFAULT 0,"
        "  l REAL NOT NULL DEFAULT 0,"
        "  c REAL NOT NULL DEFAULT 0,"
        "  v REAL NOT NULL DEFAULT 0,"
        "  PRIMARY KEY (coin, hour_ts));"
        "CREATE INDEX IF NOT EXISTS idx_candles_ts ON hl_candles(hour_ts);";
    char* err = nullptr;
    if (sqlite3_exec(hl::g_hlDb, schema, nullptr, nullptr, &err) != SQLITE_OK) {
        std::cerr << "[оракул] свечи: схема не создана: " << (err ? err : "?") << std::endl;
    }
    if (err) sqlite3_free(err);
}

/* Монеты, которые бот вообще оценивает. Тянуть свечи по всем шести сотням
   рынков Hyperliquid незачем: журнал ведётся по тем, где ходят киты. */
std::vector<std::string> candleCoins() {
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    {
        std::lock_guard<std::mutex> l(dbMutex);
        if (!db) return out;
        sqlite3_stmt* s = nullptr;
        if (prepareOrLog(db, &s,
                "SELECT token, COUNT(*) n FROM ai_events WHERE venue=1 AND ts>=? "
                "GROUP BY token ORDER BY n DESC LIMIT 200")) {
            sqlite3_bind_int64(s, 1, hl::nowSec() - ORACLE_HISTORY);
            while (sqlite3_step(s) == SQLITE_ROW) {
                const std::string c = safeColumnText(s, 0);
                if (!c.empty() && seen.insert(c).second) out.push_back(c);
            }
            sqlite3_finalize(s);
        }
    }
    // Биткоин нужен всегда: это признак режима рынка для всех остальных.
    if (seen.insert("BTC").second) out.push_back("BTC");
    return out;
}

long long lastCandleTs(const std::string& coin) {
    std::lock_guard<std::mutex> l(hl::g_hlDbMutex);
    if (!hl::g_hlDb) return 0;
    sqlite3_stmt* s = nullptr;
    if (!prepareOrLog(hl::g_hlDb, &s, "SELECT MAX(hour_ts) FROM hl_candles WHERE coin=?"))
        return 0;
    sqlite3_bind_text(s, 1, coin.c_str(), -1, SQLITE_TRANSIENT);
    long long v = 0;
    if (sqlite3_step(s) == SQLITE_ROW && sqlite3_column_type(s, 0) != SQLITE_NULL)
        v = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    return v;
}

int saveCandles(const std::string& coin, const json& arr) {
    if (!arr.is_array() || arr.empty()) return 0;
    std::lock_guard<std::mutex> l(hl::g_hlDbMutex);
    if (!hl::g_hlDb) return 0;
    sqlite3_stmt* s = nullptr;
    if (!prepareOrLog(hl::g_hlDb, &s,
            "INSERT OR REPLACE INTO hl_candles(coin,hour_ts,o,h,l,c,v) VALUES(?,?,?,?,?,?,?)"))
        return 0;
    sqlite3_exec(hl::g_hlDb, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr);
    int n = 0;
    for (const auto& e : arr) {
        if (!e.is_object()) continue;
        long long tms = 0;
        if (e.contains("t") && e["t"].is_number()) tms = e["t"].get<long long>();
        if (tms <= 0) continue;
        auto num = [&](const char* k) -> double {
            if (!e.contains(k)) return 0;
            if (e[k].is_number()) return e[k].get<double>();
            if (e[k].is_string()) { try { return std::stod(e[k].get<std::string>()); } catch (...) { return 0; } }
            return 0;
        };
        const double c = num("c");
        if (!(c > 0)) continue;
        sqlite3_reset(s);
        sqlite3_bind_text(s, 1, coin.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 2, hourFloor(tms / 1000));
        sqlite3_bind_double(s, 3, num("o"));
        sqlite3_bind_double(s, 4, num("h"));
        sqlite3_bind_double(s, 5, num("l"));
        sqlite3_bind_double(s, 6, c);
        sqlite3_bind_double(s, 7, num("v"));
        if (sqlite3_step(s) == SQLITE_DONE) n++;
    }
    sqlite3_finalize(s);
    if (sqlite3_exec(hl::g_hlDb, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK)
        sqlite3_exec(hl::g_hlDb, "ROLLBACK", nullptr, nullptr, nullptr);
    return n;
}

/* Часовые свечи. В базе бота есть только цена-марка раз в час и только с
   того дня, как его запустили: настоящих максимума, минимума и часового
   объёма там нет вовсе, а истории — сколько успели. Биржа отдаёт и то и
   другое сразу за три месяца, поэтому новые признаки работают на всём
   журнале, а не только на будущих событиях. */
void fetchCandles() {
    ensureCandleSchema();
    const auto coins = candleCoins();
    if (coins.empty()) return;
    static size_t cursor = 0;
    const long long now = hl::nowSec();
    int done = 0, rows = 0;
    for (size_t k = 0; k < coins.size() && done < ORACLE_CANDLE_BATCH; k++) {
        const std::string& coin = coins[(cursor + k) % coins.size()];
        const long long last = lastCandleTs(coin);
        // Свежая монета — тянем всю историю, знакомая — только новые часы.
        const long long from = last > 0 ? std::max(last, now - ORACLE_HISTORY)
                                        : now - ORACLE_HISTORY;
        if (last > 0 && now - last < 2 * 3600) continue;
        json req;
        req["type"] = "candleSnapshot";
        req["req"] = json{{"coin", coin}, {"interval", "1h"},
                          {"startTime", from * 1000}, {"endTime", now * 1000}};
        const json got = hl::infoPost(req, ORACLE_WEIGHT_CANDLE);
        done++;
        if (!got.is_array()) continue;
        rows += saveCandles(coin, got);
    }
    cursor = (cursor + static_cast<size_t>(done > 0 ? done : 1)) % coins.size();
    if (rows > 0)
        std::cout << "[оракул] свечи: " << rows << " часов по " << done
                  << " монетам" << std::endl;
}

/* ------------------------------------------------- сборка рядов рынка --- */

void loadPerpSeries(Market& m, long long from) {
    std::lock_guard<std::mutex> l(hl::g_hlDbMutex);
    if (!hl::g_hlDb) return;
    sqlite3_stmt* s = nullptr;
    // Основа — часовые снимки бота: ставка, марка, открытый интерес, объём.
    if (prepareOrLog(hl::g_hlDb, &s,
            "SELECT coin,hour_ts,rate_nanos,mark_nanos,oi_nanos,day_vlm_nanos "
            "FROM hl_funding_rate WHERE hour_ts>=? ORDER BY coin,hour_ts")) {
        sqlite3_bind_int64(s, 1, from);
        while (sqlite3_step(s) == SQLITE_ROW) {
            Bar b;
            const std::string coin = safeColumnText(s, 0);
            b.ts = sqlite3_column_int64(s, 1);
            b.fund = static_cast<double>(sqlite3_column_int64(s, 2)) / 1e9;
            b.c = static_cast<double>(sqlite3_column_int64(s, 3)) / 1e9;
            b.oi = static_cast<double>(sqlite3_column_int64(s, 4)) / 1e9;
            b.v = static_cast<double>(sqlite3_column_int64(s, 5)) / 1e9 / 24.0;
            if (coin.empty()) continue;
            m.perp[coin].bars.push_back(b);
        }
        sqlite3_finalize(s);
    }
    // Поверх — свечи биржи: настоящие максимум, минимум и объём за час.
    s = nullptr;
    if (prepareOrLog(hl::g_hlDb, &s,
            "SELECT coin,hour_ts,h,l,c,v FROM hl_candles WHERE hour_ts>=? ORDER BY coin,hour_ts")) {
        sqlite3_bind_int64(s, 1, from);
        std::unordered_map<std::string, std::unordered_map<long long, size_t>> where;
        for (auto& kv : m.perp) {
            auto& idx = where[kv.first];
            for (size_t i = 0; i < kv.second.bars.size(); i++)
                idx[kv.second.bars[i].ts] = i;
        }
        while (sqlite3_step(s) == SQLITE_ROW) {
            const std::string coin = safeColumnText(s, 0);
            if (coin.empty()) continue;
            Bar b;
            b.ts = sqlite3_column_int64(s, 1);
            b.hi = sqlite3_column_double(s, 2);
            b.lo = sqlite3_column_double(s, 3);
            b.c = sqlite3_column_double(s, 4);
            b.v = sqlite3_column_double(s, 5);
            auto wi = where.find(coin);
            if (wi != where.end()) {
                auto at = wi->second.find(b.ts);
                if (at != wi->second.end()) {
                    Bar& dst = m.perp[coin].bars[at->second];
                    dst.hi = b.hi; dst.lo = b.lo; dst.v = b.v;
                    if (b.c > 0) dst.c = b.c;
                    continue;
                }
            }
            // Час, которого у бота не было: свеча его восстанавливает.
            if (b.c > 0) m.perp[coin].bars.push_back(b);
        }
        sqlite3_finalize(s);
    }
    for (auto& kv : m.perp) {
        auto& v = kv.second.bars;
        std::sort(v.begin(), v.end(), [](const Bar& a, const Bar& b) { return a.ts < b.ts; });
        v.erase(std::unique(v.begin(), v.end(),
                            [](const Bar& a, const Bar& b) { return a.ts == b.ts; }), v.end());
    }
}

void loadSpotSeries(Market& m, long long from) {
    std::lock_guard<std::mutex> l(dbMutex);
    if (!db) return;
    sqlite3_stmt* s = nullptr;
    // Только те токены, по которым вообще ведётся журнал: история цен в базе
    // на порядок шире, а память не бесконечна.
    if (!prepareOrLog(db, &s,
            "SELECT h.address,h.ts,h.price_nanos FROM token_price_history h "
            "WHERE h.ts>=? AND h.address IN "
            "  (SELECT DISTINCT token FROM ai_events WHERE venue=0 AND ts>=?) "
            "ORDER BY h.address,h.ts"))
        return;
    sqlite3_bind_int64(s, 1, from);
    sqlite3_bind_int64(s, 2, from);
    while (sqlite3_step(s) == SQLITE_ROW) {
        Bar b;
        const std::string addr = toLower(safeColumnText(s, 0));
        b.ts = hourFloor(sqlite3_column_int64(s, 1));
        b.c = static_cast<double>(sqlite3_column_int64(s, 2)) / 1e9;
        if (addr.empty() || b.c <= 0) continue;
        auto& v = m.spot[addr].bars;
        if (!v.empty() && v.back().ts == b.ts) v.back() = b;
        else v.push_back(b);
    }
    sqlite3_finalize(s);
}

/* Ширина рынка: какая доля монет за сутки в плюсе. Один и тот же приток при
   растущем рынке и при падающем значит разное, а без этого признака модель
   про рынок в целом не знает ничего. */
void buildBreadth(Market& m, long long from, long long to) {
    if (m.perp.empty()) return;
    for (long long t = hourFloor(from) + 24 * 3600; t <= to; t += 3600) {
        int up = 0, n = 0;
        for (const auto& kv : m.perp) {
            const int i = kv.second.at(t);
            if (i < 24) continue;
            const double r = retOver(kv.second, i, 24);
            if (r == 0) continue;
            n++;
            if (r > 0) up++;
        }
        if (n >= 5) m.breadth.emplace_back(t, static_cast<double>(up) / n);
    }
}

void rebuildMarket() {
    const long long now = hl::nowSec();
    const long long from = now - ORACLE_HISTORY;
    Market fresh;
    loadPerpSeries(fresh, from);
    loadSpotSeries(fresh, from);
    auto btc = fresh.perp.find("BTC");
    if (btc != fresh.perp.end()) fresh.btc = btc->second;
    buildBreadth(fresh, from, now);
    fresh.builtAt = now;
    size_t bars = 0;
    for (const auto& kv : fresh.perp) bars += kv.second.bars.size();
    for (const auto& kv : fresh.spot) bars += kv.second.bars.size();
    {
        std::lock_guard<std::mutex> l(g_mktMutex);
        g_mkt = std::make_shared<const Market>(std::move(fresh));
    }
    std::cout << "[оракул] ряды: " << bars << " часов" << std::endl;
}

/* ------------------------------------------------------- хранение --- */

void putU32(std::string& s, uint32_t v) { s.append(reinterpret_cast<const char*>(&v), 4); }
void putF32(std::string& s, float v) { s.append(reinterpret_cast<const char*>(&v), 4); }
void putF64(std::string& s, double v) { s.append(reinterpret_cast<const char*>(&v), 8); }

bool getBytes(const std::string& s, size_t& at, void* dst, size_t n) {
    if (at + n > s.size()) return false;
    std::memcpy(dst, s.data() + at, n);
    at += n;
    return true;
}

std::string packForest(const Forest& f) {
    std::string s;
    s.append("ORC2", 4);
    putU32(s, static_cast<uint32_t>(ORACLE_NF));
    putU32(s, f.squared ? 1u : 0u);
    putF64(s, f.base);
    putF64(s, f.calA);
    putF64(s, f.calB);
    for (double m : f.med) putF64(s, m);
    putU32(s, static_cast<uint32_t>(f.trees.size()));
    for (const Tree& t : f.trees) {
        putU32(s, static_cast<uint32_t>(t.nodes.size()));
        for (const Node& n : t.nodes) {
            putU32(s, static_cast<uint32_t>(n.feat));
            putF32(s, n.thr);
            putU32(s, static_cast<uint32_t>(n.left));
            putU32(s, static_cast<uint32_t>(n.right));
            putF32(s, n.leaf);
        }
    }
    return s;
}

bool unpackForestAt(const std::string& s, size_t& at, Forest& f) {
    char magic[4] = {0, 0, 0, 0};
    if (!getBytes(s, at, magic, 4) || std::memcmp(magic, "ORC2", 4) != 0) return false;
    uint32_t nf = 0;
    if (!getBytes(s, at, &nf, 4) || nf != static_cast<uint32_t>(ORACLE_NF)) return false;
    uint32_t sq = 0;
    if (!getBytes(s, at, &sq, 4)) return false;
    f.squared = sq != 0;
    if (!getBytes(s, at, &f.base, 8)) return false;
    if (!getBytes(s, at, &f.calA, 8)) return false;
    if (!getBytes(s, at, &f.calB, 8)) return false;
    for (int i = 0; i < ORACLE_NF; i++)
        if (!getBytes(s, at, &f.med[static_cast<size_t>(i)], 8)) return false;
    uint32_t nt = 0;
    if (!getBytes(s, at, &nt, 4) || nt > 100000) return false;
    f.trees.clear();
    f.trees.reserve(nt);
    for (uint32_t i = 0; i < nt; i++) {
        uint32_t nn = 0;
        if (!getBytes(s, at, &nn, 4) || nn == 0 || nn > 4096) return false;
        Tree t;
        t.nodes.resize(nn);
        for (uint32_t k = 0; k < nn; k++) {
            uint32_t fe = 0, le = 0, ri = 0;
            float thr = 0, leaf = 0;
            if (!getBytes(s, at, &fe, 4) || !getBytes(s, at, &thr, 4) ||
                !getBytes(s, at, &le, 4) || !getBytes(s, at, &ri, 4) ||
                !getBytes(s, at, &leaf, 4)) return false;
            Node& n = t.nodes[k];
            n.feat = static_cast<int>(fe);
            n.thr = thr;
            n.left = static_cast<int>(le);
            n.right = static_cast<int>(ri);
            n.leaf = leaf;
            if (n.feat >= ORACLE_NF) return false;
            if (n.feat >= 0 && (n.left < 0 || n.right < 0 ||
                                n.left >= static_cast<int>(nn) || n.right >= static_cast<int>(nn)))
                return false;
        }
        f.trees.push_back(std::move(t));
    }
    return true;
}

bool unpackForest(const std::string& s, Forest& f) {
    size_t at = 0;
    return unpackForestAt(s, at, f);
}

/* Модель — три леса: направление, ход вверх, ход вниз. Первые два байта
   говорят, сколько лесов внутри: старая запись с одним читается как прежде,
   и после обновления бот не теряет обученную модель. */
struct Model {
    Forest dir;
    Forest up;
    Forest dn;
    bool levels = false;      // обучены ли леса уровней
};

std::string packModel(const Model& m) {
    std::string s;
    s.append("ORCM", 4);
    putU32(s, m.levels ? 3u : 1u);
    s += packForest(m.dir);
    if (m.levels) {
        s += packForest(m.up);
        s += packForest(m.dn);
    }
    return s;
}

bool unpackModel(const std::string& s, Model& m) {
    if (s.size() >= 4 && std::memcmp(s.data(), "ORC2", 4) == 0) {
        m.levels = false;            // запись прежнего образца: только направление
        return unpackForest(s, m.dir);
    }
    size_t at = 0;
    char magic[4] = {0, 0, 0, 0};
    if (!getBytes(s, at, magic, 4) || std::memcmp(magic, "ORCM", 4) != 0) return false;
    uint32_t n = 0;
    if (!getBytes(s, at, &n, 4) || (n != 1 && n != 3)) return false;
    if (!unpackForestAt(s, at, m.dir)) return false;
    m.levels = n == 3;
    if (!m.levels) return true;
    return unpackForestAt(s, at, m.up) && unpackForestAt(s, at, m.dn);
}

void ensureModelSchema() {
    std::lock_guard<std::mutex> l(dbMutex);
    if (!db) return;
    const char* schema =
        "CREATE TABLE IF NOT EXISTS ai_models ("
        "  venue INTEGER NOT NULL,"
        "  horizon INTEGER NOT NULL,"
        "  created_at INTEGER NOT NULL,"
        "  samples INTEGER NOT NULL DEFAULT 0,"
        "  test_n INTEGER NOT NULL DEFAULT 0,"
        "  trees INTEGER NOT NULL DEFAULT 0,"
        "  auc REAL NOT NULL DEFAULT 0,"
        "  logloss REAL NOT NULL DEFAULT 0,"
        "  acc REAL NOT NULL DEFAULT 0,"
        "  brier REAL NOT NULL DEFAULT 0,"
        "  base_logloss REAL NOT NULL DEFAULT 0,"
        "  base_rate REAL NOT NULL DEFAULT 0,"
        "  wf_auc REAL NOT NULL DEFAULT 0,"
        "  levels INTEGER NOT NULL DEFAULT 0,"
        "  up_err REAL NOT NULL DEFAULT 0,"
        "  dn_err REAL NOT NULL DEFAULT 0,"
        "  gain BLOB,"
        "  model BLOB,"
        "  PRIMARY KEY(venue, horizon));"
        /* Последняя попытка обучения — принятая или нет. Без неё экран мог
           сказать только «модель не обучена», и человек с 2468 готовыми
           исходами не понимал, чего ещё ждать. */
        "CREATE TABLE IF NOT EXISTS ai_model_try ("
        "  venue INTEGER NOT NULL,"
        "  horizon INTEGER NOT NULL,"
        "  at INTEGER NOT NULL,"
        "  samples INTEGER NOT NULL DEFAULT 0,"
        "  auc REAL NOT NULL DEFAULT 0,"
        "  logloss REAL NOT NULL DEFAULT 0,"
        "  base_logloss REAL NOT NULL DEFAULT 0,"
        "  wf_auc REAL NOT NULL DEFAULT 0,"
        "  accepted INTEGER NOT NULL DEFAULT 0,"
        "  PRIMARY KEY(venue, horizon));";
    char* err = nullptr;
    if (sqlite3_exec(db, schema, nullptr, nullptr, &err) != SQLITE_OK)
        std::cerr << "[оракул] схема моделей: " << (err ? err : "?") << std::endl;
    if (err) sqlite3_free(err);
    // Старая таблица без столбцов уровней: доливаем на месте.
    for (const char* mig : {"ALTER TABLE ai_models ADD COLUMN levels INTEGER NOT NULL DEFAULT 0",
                            "ALTER TABLE ai_models ADD COLUMN up_err REAL NOT NULL DEFAULT 0",
                            "ALTER TABLE ai_models ADD COLUMN dn_err REAL NOT NULL DEFAULT 0"}) {
        char* e = nullptr;
        sqlite3_exec(db, mig, nullptr, nullptr, &e);
        if (e) sqlite3_free(e);
    }
}

struct Live {
    bool have = false;
    Model model;
    OracleStats st;
};
std::mutex g_liveMutex;
/* Площадка × горизонт. Индекс горизонта — порядок в ORACLE_HZ. */
constexpr long long ORACLE_HZ[] = {ORACLE_H6, ORACLE_H24};
constexpr int ORACLE_NH = 2;
Live g_live[2][ORACLE_NH];

int hIndex(long long horizon) {
    for (int i = 0; i < ORACLE_NH; i++)
        if (ORACLE_HZ[i] == horizon) return i;
    return ORACLE_NH - 1;                      // по умолчанию сутки
}

void applyStats(OracleStats& st, const std::array<double, ORACLE_NF>& gain) {
    std::vector<std::pair<std::string, double>> v;
    double tot = 0;
    for (double g : gain) tot += g;
    if (tot <= 0) tot = 1;
    for (int i = 0; i < ORACLE_NF; i++)
        v.emplace_back(FEAT_NAME[i], gain[static_cast<size_t>(i)] / tot);
    std::sort(v.begin(), v.end(),
              [](const std::pair<std::string, double>& a, const std::pair<std::string, double>& b) {
                  return a.second > b.second;
              });
    v.resize(std::min<size_t>(v.size(), 5));
    st.top = std::move(v);
}

void saveModel(bool perp, const Model& f, const OracleStats& st,
               const std::array<double, ORACLE_NF>& gain) {
    const long long horizon = st.horizon > 0 ? st.horizon : ORACLE_H24;
    ensureModelSchema();
    const std::string blob = packModel(f);
    std::string gblob;
    for (double g : gain) putF64(gblob, g);
    std::lock_guard<std::mutex> l(dbMutex);
    if (!db) return;
    sqlite3_stmt* s = nullptr;
    if (!prepareOrLog(db, &s,
            "INSERT OR REPLACE INTO ai_models(venue,horizon,created_at,samples,test_n,trees,"
            "auc,logloss,acc,brier,base_logloss,base_rate,wf_auc,levels,up_err,dn_err,"
            "gain,model) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"))
        return;
    sqlite3_bind_int(s, 1, perp ? 1 : 0);
    sqlite3_bind_int64(s, 2, horizon);
    sqlite3_bind_int64(s, 3, st.at);
    sqlite3_bind_int64(s, 4, st.samples);
    sqlite3_bind_int64(s, 5, st.test);
    sqlite3_bind_int(s, 6, st.trees);
    sqlite3_bind_double(s, 7, st.auc);
    sqlite3_bind_double(s, 8, st.logloss);
    sqlite3_bind_double(s, 9, st.acc);
    sqlite3_bind_double(s, 10, st.brier);
    sqlite3_bind_double(s, 11, st.baseLogloss);
    sqlite3_bind_double(s, 12, st.baseRate);
    sqlite3_bind_double(s, 13, st.wfAuc);
    sqlite3_bind_int(s, 14, st.levels ? 1 : 0);
    sqlite3_bind_double(s, 15, st.upErr);
    sqlite3_bind_double(s, 16, st.dnErr);
    sqlite3_bind_blob(s, 17, gblob.data(), static_cast<int>(gblob.size()), SQLITE_TRANSIENT);
    sqlite3_bind_blob(s, 18, blob.data(), static_cast<int>(blob.size()), SQLITE_TRANSIENT);
    if (sqlite3_step(s) != SQLITE_DONE)
        std::cerr << "[оракул] модель не сохранена" << std::endl;
    sqlite3_finalize(s);
}

void saveTry(bool perp, long long horizon, long long samples, double auc, double loss,
             double base, double wf, bool accepted) {
    ensureModelSchema();
    std::lock_guard<std::mutex> l(dbMutex);
    if (!db) return;
    sqlite3_stmt* s = nullptr;
    if (!prepareOrLog(db, &s,
            "INSERT OR REPLACE INTO ai_model_try(venue,horizon,at,samples,auc,logloss,"
            "base_logloss,wf_auc,accepted) VALUES(?,?,?,?,?,?,?,?,?)"))
        return;
    sqlite3_bind_int(s, 1, perp ? 1 : 0);
    sqlite3_bind_int64(s, 2, horizon);
    sqlite3_bind_int64(s, 3, hl::nowSec());
    sqlite3_bind_int64(s, 4, samples);
    sqlite3_bind_double(s, 5, auc);
    sqlite3_bind_double(s, 6, loss);
    sqlite3_bind_double(s, 7, base);
    sqlite3_bind_double(s, 8, wf);
    sqlite3_bind_int(s, 9, accepted ? 1 : 0);
    sqlite3_step(s);
    sqlite3_finalize(s);
}

void loadModels() {
    ensureModelSchema();
    for (int v = 0; v < 2; v++) for (int hi = 0; hi < ORACLE_NH; hi++) {
        const long long horizon = ORACLE_HZ[hi];
        std::string blob, gblob;
        OracleStats st;
        {
            std::lock_guard<std::mutex> l(dbMutex);
            if (!db) return;
            sqlite3_stmt* s = nullptr;
            if (!prepareOrLog(db, &s,
                    "SELECT created_at,samples,test_n,trees,auc,logloss,acc,brier,"
                    "base_logloss,base_rate,wf_auc,gain,model,levels,up_err,dn_err "
                    "FROM ai_models "
                    "WHERE venue=? AND horizon=?"))
                return;
            sqlite3_bind_int(s, 1, v);
            sqlite3_bind_int64(s, 2, horizon);
            if (sqlite3_step(s) == SQLITE_ROW) {
                st.at = sqlite3_column_int64(s, 0);
                st.samples = sqlite3_column_int64(s, 1);
                st.test = sqlite3_column_int64(s, 2);
                st.trees = sqlite3_column_int(s, 3);
                st.auc = sqlite3_column_double(s, 4);
                st.logloss = sqlite3_column_double(s, 5);
                st.acc = sqlite3_column_double(s, 6);
                st.brier = sqlite3_column_double(s, 7);
                st.baseLogloss = sqlite3_column_double(s, 8);
                st.baseRate = sqlite3_column_double(s, 9);
                st.wfAuc = sqlite3_column_double(s, 10);
                const void* gp = sqlite3_column_blob(s, 11);
                const int gn = sqlite3_column_bytes(s, 11);
                if (gp && gn > 0) gblob.assign(static_cast<const char*>(gp), static_cast<size_t>(gn));
                const void* p = sqlite3_column_blob(s, 12);
                const int n = sqlite3_column_bytes(s, 12);
                if (p && n > 0) blob.assign(static_cast<const char*>(p), static_cast<size_t>(n));
                st.upErr = sqlite3_column_double(s, 14);
                st.dnErr = sqlite3_column_double(s, 15);
            }
            sqlite3_finalize(s);
        }
        if (blob.empty()) continue;
        Model f;
        if (!unpackModel(blob, f)) {
            std::cerr << "[оракул] модель " << (v ? "перпов" : "спота")
                      << " не читается, забыта" << std::endl;
            continue;
        }
        std::array<double, ORACLE_NF> gain{};
        if (gblob.size() == static_cast<size_t>(ORACLE_NF) * 8) {
            size_t at = 0;
            for (int i = 0; i < ORACLE_NF; i++) getBytes(gblob, at, &gain[static_cast<size_t>(i)], 8);
        }
        st.trained = true;
        st.levels = f.levels;
        st.horizon = horizon;
        applyStats(st, gain);
        std::lock_guard<std::mutex> l(g_liveMutex);
        g_live[v][hi].have = true;
        g_live[v][hi].model = std::move(f);
        g_live[v][hi].st = std::move(st);
    }
}

/* ------------------------------------------------------- обучение --- */

/* Журнал событий с исходами. Дубли одной монеты за день схлопываются: иначе
   один разогнавшийся день даёт двадцать почти одинаковых примеров и модель
   учит его наизусть. */
std::vector<Sample> loadSamples(bool perp, const Market& m, long long horizon) {
    /* Исходы обоих горизонтов бот собирает давно: на шести часах своя пара
       столбцов, на сутках своя. Разметка хода считается на том же окне. */
    const bool six = horizon == ORACLE_H6;
    const double minMove = oracleMinMove(horizon);
    std::vector<Sample> out;
    std::lock_guard<std::mutex> lock(dbMutex);
    if (!db) return out;
    sqlite3_stmt* s = nullptr;
    const std::string px = six ? "price_6h" : "price_24h";
    const std::string filled = six ? "filled_6h" : "filled_at";
    const std::string sql =
        "SELECT e.ts,e.token,e.buy_nanos,e.sell_nanos,e.n_buy,e.n_sell,e.wallets,"
        "e.one_share_bp,e.net_6h,e.net_prior,e.funding_nanos,e.liq_nanos,e.lev_bp,"
        "e.top_share_bp,e.top_dir_bp,e.liq_long_nanos,e.liq_short_nanos,e.both_venues,"
        "e.price_then,e." + px + " "
        "FROM ai_events e WHERE e." + filled + ">0 AND e.price_then>0 AND e." + px + ">0 "
        "AND e.window_days=24 AND e.venue=? "
        // Один пример на монету в день: иначе разогнавшийся день учится
        // наизусть двадцатью почти одинаковыми строками.
        "AND NOT EXISTS ("
        "  SELECT 1 FROM ai_events e2 WHERE e2.token=e.token AND e2.venue=e.venue "
        "  AND e2.window_days=24 AND e2." + filled + ">0 AND e2.price_then>0 "
        "  AND e2." + px + ">0 AND e2.ts/86400=e.ts/86400 AND e2.id<e.id) "
        "ORDER BY e.ts";
    if (!prepareOrLog(db, &s, sql.c_str()))
        return out;
    sqlite3_bind_int(s, 1, perp ? 1 : 0);
    while (sqlite3_step(s) == SQLITE_ROW) {
        OracleInput in;
        in.perp = perp;
        const long long ts = sqlite3_column_int64(s, 0);
        in.id = safeColumnText(s, 1);
        in.buy = sqlite3_column_int64(s, 2);
        in.sell = sqlite3_column_int64(s, 3);
        in.nBuy = sqlite3_column_int(s, 4);
        in.nSell = sqlite3_column_int(s, 5);
        in.wallets = sqlite3_column_int(s, 6);
        in.oneShare = sqlite3_column_int(s, 7) / 10000.0;
        in.net6h = sqlite3_column_int64(s, 8);
        in.netPrior = sqlite3_column_int64(s, 9);
        in.funding = sqlite3_column_int64(s, 10);
        in.liqUsd = static_cast<double>(sqlite3_column_int64(s, 11)) / 1e9;
        in.levBp = sqlite3_column_int(s, 12);
        in.topShare = sqlite3_column_int(s, 13) / 10000.0;
        in.topDir = sqlite3_column_int(s, 14) / 10000.0;
        in.liqLong = sqlite3_column_int64(s, 15);
        in.liqShort = sqlite3_column_int64(s, 16);
        in.bothVenues = sqlite3_column_int(s, 17);
        const long long then = sqlite3_column_int64(s, 18);
        const long long later = sqlite3_column_int64(s, 19);
        if (then <= 0 || later <= 0) continue;
        const double ret = static_cast<double>(later - then) / static_cast<double>(then);
        if (std::fabs(ret) < minMove) continue;
        Sample sm;
        sm.ts = ts;
        featuresOf(m, in, ts, sm.f);
        sm.y = ret > 0 ? 1.0f : 0.0f;
        {
            // Ход цены в обе стороны — разметка для уровней. Считается по тем
            // же рядам, что и признаки, и только вперёд от события.
            const Series* ser = nullptr;
            if (perp) {
                auto it = m.perp.find(in.id);
                if (it != m.perp.end()) ser = &it->second;
            } else {
                auto it = m.spot.find(toLower(in.id));
                if (it != m.spot.end()) ser = &it->second;
            }
            double up = 0, dn = 0;
            if (ser) excursion(*ser, ts, ts + horizon, usdOf(then), up, dn);
            sm.up = static_cast<float>(up);
            sm.dn = static_cast<float>(dn);
        }
        // Большое движение весит больше маленького: ошибка на нём дороже.
        sm.w = static_cast<float>(clampd(std::fabs(ret) / minMove, 1.0, 3.0));
        out.push_back(std::move(sm));
    }
    sqlite3_finalize(s);
    return out;
}

struct Scored {
    double auc = 0, logloss = 0, acc = 0, brier = 0, baseLoss = 0, rate = 0;
};

Scored scoreOn(const Forest& f, const std::vector<Sample>& xs) {
    Scored r;
    if (xs.empty()) return r;
    std::vector<double> p(xs.size());
    std::vector<float> y(xs.size());
    double pos = 0;
    for (size_t i = 0; i < xs.size(); i++) {
        p[i] = f.p(xs[i].f.data());
        y[i] = xs[i].y;
        pos += xs[i].y;
    }
    r.rate = pos / static_cast<double>(xs.size());
    r.auc = aucOf(p, y);
    r.logloss = loglossOf(p, y);
    r.acc = accOf(p, y);
    r.brier = brierOf(p, y);
    std::vector<double> flat(xs.size(), clampd(r.rate, 1e-6, 1 - 1e-6));
    r.baseLoss = loglossOf(flat, y);
    return r;
}

/* Скользящая проверка: четыре раза учимся на прошлом и смотрим на следующем
   куске. Одна отложенная часть может оказаться удачной случайно; четыре
   подряд — уже похоже на правду. */
double walkForward(const std::vector<Sample>& xs) {
    if (xs.size() < static_cast<size_t>(ORACLE_MIN_SAMPLES) * 2) return 0;
    double sum = 0;
    int n = 0;
    for (int k = 1; k <= 4; k++) {
        const size_t trEnd = xs.size() * static_cast<size_t>(3 + k) / 8;
        const size_t teEnd = xs.size() * static_cast<size_t>(4 + k) / 8;
        if (trEnd < static_cast<size_t>(ORACLE_MIN_SAMPLES) || teEnd <= trEnd) continue;
        const size_t vaFrom = trEnd * 85 / 100;
        std::vector<Sample> tr(xs.begin(), xs.begin() + static_cast<long>(vaFrom));
        std::vector<Sample> va(xs.begin() + static_cast<long>(vaFrom),
                               xs.begin() + static_cast<long>(trEnd));
        std::vector<Sample> te(xs.begin() + static_cast<long>(trEnd),
                               xs.begin() + static_cast<long>(teEnd));
        if (tr.size() < static_cast<size_t>(ORACLE_MIN_SAMPLES) || te.size() < 40) continue;
        Fit fit = trainForest(tr, va);
        if (fit.trees == 0) continue;
        sum += scoreOn(fit.forest, te).auc;
        n++;
    }
    return n > 0 ? sum / n : 0;
}

void trainVenue(bool perp, const Market& m, long long horizon) {
    std::vector<Sample> xs = loadSamples(perp, m, horizon);
    const std::string who = std::string(perp ? "перпы" : "спот") + " "
                          + (horizon == ORACLE_H6 ? "6ч" : "24ч");
    if (xs.size() < static_cast<size_t>(ORACLE_MIN_SAMPLES) + 100) {
        std::cout << "[оракул] " << who << ": исходов " << xs.size()
                  << ", нужно " << ORACLE_MIN_SAMPLES + 100 << std::endl;
        return;
    }
    std::sort(xs.begin(), xs.end(),
              [](const Sample& a, const Sample& b) { return a.ts < b.ts; });

    // Делим по времени, а не случайно: в бою модель всегда смотрит вперёд.
    const size_t nTr = xs.size() * 70 / 100;
    const size_t nVa = xs.size() * 85 / 100;
    std::vector<Sample> tr(xs.begin(), xs.begin() + static_cast<long>(nTr));
    std::vector<Sample> va(xs.begin() + static_cast<long>(nTr),
                           xs.begin() + static_cast<long>(nVa));
    std::vector<Sample> te(xs.begin() + static_cast<long>(nVa), xs.end());

    Fit fit = trainForest(tr, va);
    if (fit.trees == 0) {
        std::cout << "[оракул] " << who << ": деревьев не выросло" << std::endl;
        return;
    }
    const Scored sc = scoreOn(fit.forest, te);
    const double wf = walkForward(xs);

    /* Уровни: два леса про то, как далеко цена уходит вверх и вниз. Они
       принимаются отдельно от направления и по своему признаку — средняя
       ошибка должна быть меньше, чем у предсказания «всегда средний ход».
       Иначе стоп с целью считает прежняя формула от волатильности. */
    Model model;
    model.dir = fit.forest;
    double upErr = 0, dnErr = 0;
    {
        Fit fUp = trainForest(tr, va, Task::Up);
        Fit fDn = trainForest(tr, va, Task::Dn);
        if (fUp.trees > 0 && fDn.trees > 0) {
            double eUp = 0, eDn = 0, bUp = 0, bDn = 0, mUp = 0, mDn = 0;
            for (const Sample& x : tr) { mUp += x.up; mDn += x.dn; }
            mUp /= static_cast<double>(tr.size());
            mDn /= static_cast<double>(tr.size());
            for (const Sample& x : te) {
                eUp += std::fabs(fUp.forest.value(x.f.data()) - x.up);
                eDn += std::fabs(fDn.forest.value(x.f.data()) - x.dn);
                bUp += std::fabs(mUp - x.up);
                bDn += std::fabs(mDn - x.dn);
            }
            const size_t nte = te.empty() ? 1 : te.size();
            eUp /= nte; eDn /= nte; bUp /= nte; bDn /= nte;
            if (eUp < bUp && eDn < bDn) {
                model.up = fUp.forest;
                model.dn = fDn.forest;
                model.levels = true;
                upErr = eUp * 100.0;
                dnErr = eDn * 100.0;
                std::cout << "[оракул] " << who << ": уровни от модели, ошибка "
                          << std::llround(eUp * 1000) / 10.0 << "% / "
                          << std::llround(eDn * 1000) / 10.0 << "% против "
                          << std::llround(bUp * 1000) / 10.0 << "% / "
                          << std::llround(bDn * 1000) / 10.0 << "%" << std::endl;
            } else {
                std::cout << "[оракул] " << who
                          << ": уровни не приняты, остаётся формула" << std::endl;
            }
        }
    }

    std::cout << "[оракул] " << who << ": " << xs.size() << " исходов, "
              << fit.trees << " деревьев, тест " << te.size()
              << " · AUC " << std::llround(sc.auc * 1000) / 1000.0
              << " · потери " << std::llround(sc.logloss * 1000) / 1000.0
              << " против " << std::llround(sc.baseLoss * 1000) / 1000.0
              << " · точность " << std::llround(sc.acc * 100) << "%"
              << " · скользящий AUC " << std::llround(wf * 1000) / 1000.0 << std::endl;

    // Модель принимается, только если она лучше постоянного прогноза и на
    // тесте, и на скользящей проверке. Иначе на экране была бы «модель», а
    // под ней — монетка.
    const bool accepted = !(sc.auc < 0.55 || sc.logloss >= sc.baseLoss || wf < 0.52);
    saveTry(perp, horizon, static_cast<long long>(xs.size()), sc.auc, sc.logloss,
            sc.baseLoss, wf, accepted);
    if (!accepted) {
        std::cout << "[оракул] " << who << ": не принята, остаёмся на прежнем" << std::endl;
        return;
    }

    OracleStats st;
    st.trained = true;
    st.horizon = horizon;
    st.at = hl::nowSec();
    st.samples = static_cast<long long>(xs.size());
    st.test = static_cast<long long>(te.size());
    st.trees = fit.trees;
    st.auc = sc.auc;
    st.logloss = sc.logloss;
    st.acc = sc.acc;
    st.brier = sc.brier;
    st.baseLogloss = sc.baseLoss;
    st.baseRate = sc.rate;
    st.wfAuc = wf;
    st.levels = model.levels;
    st.upErr = upErr;
    st.dnErr = dnErr;
    applyStats(st, fit.gain);
    saveModel(perp, model, st, fit.gain);
    {
        const int hi = hIndex(horizon);
        std::lock_guard<std::mutex> l(g_liveMutex);
        g_live[perp ? 1 : 0][hi].have = true;
        g_live[perp ? 1 : 0][hi].model = model;
        g_live[perp ? 1 : 0][hi].st = st;
    }
}

}  // namespace

/* -------------------------------------------------------- наружу --- */

/* Переучивание с чистого листа.
 *
 * Включается переменной окружения WHALE_ORACLE_RESET и срабатывает один раз
 * за запуск. Два уровня, и разница между ними велика:
 *
 *   models — стираются обученные модели, записи о попытках, текущие сигналы
 *            и журнал выданных. Модель переучится на первом же тике, послужной
 *            список начнётся с нуля. Это то, что нужно после смены правил.
 *
 *   all    — вдобавок стирается журнал исходов (ai_events). Это три месяца
 *            собранных результатов, и без них обучать будет нечего недели две.
 *            Разметка уровней и так считается заново из истории цен, так что
 *            выигрыша от этого нет — только пауза.
 */
void oracleReset() {
    /* Файл-метка рядом с ботом — второй способ, и на боевой машине
     * единственный работающий. `sudo WHALE_ORACLE_RESET=all systemctl restart`
     * не делает ничего: systemd берёт окружение службы из юнита, а не из той
     * оболочки, где набрали команду, и переменная до бота не доходит.
     * Поэтому: `echo all > .oracle-reset` и обычный перезапуск. Метка
     * стирается сразу после сброса, так что второй раз он не повторится. */
    std::string m;
    const char* mode = std::getenv("WHALE_ORACLE_RESET");
    if (mode && *mode) m = mode;
    const char* MARK = ".oracle-reset";
    bool fromFile = false;
    if (m.empty()) {
        std::ifstream f(MARK);
        if (f) {
            std::getline(f, m);
            fromFile = true;
            // Пустой файл — значит «modes по умолчанию», а не «ничего».
            while (!m.empty() && (m.back() == '\r' || m.back() == ' ')) m.pop_back();
            if (m.empty()) m = "models";
        }
    }
    if (fromFile) std::remove(MARK);
    if (m.empty()) return;
    if (m == "0" || m == "no") return;
    const bool wipeAll = m == "all";

    std::lock_guard<std::mutex> l(dbMutex);
    if (!db) return;
    // Каждая таблица отдельно: часть из них заводит ai.cpp, и на первом
    // запуске их ещё нет. Одной командой первая же ошибка отменила бы всё.
    auto wipe = [&](const char* table) {
        const std::string sql = std::string("DELETE FROM ") + table;
        char* err = nullptr;
        if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
            const std::string msg = err ? err : "";
            if (msg.find("no such table") == std::string::npos)
                std::cerr << "[оракул] сброс " << table << ": " << msg << std::endl;
        }
        if (err) sqlite3_free(err);
    };
    for (const char* t : {"ai_models", "ai_model_try", "ai_signals", "ai_signal_log"})
        wipe(t);
    std::cout << "[оракул] сброшены модели, попытки и журнал сигналов" << std::endl;
    if (wipeAll) {
        wipe("ai_events");
        std::cout << "[оракул] журнал исходов стёрт: обучать будет нечего, "
                     "пока он не наберётся заново" << std::endl;
    }
}

void oracleTick() {
    static long long lastCandles = 0;
    static long long lastTrain = 0;
    static long long lastMarket = 0;
    static bool loaded = false;
    const long long now = hl::nowSec();

    if (!loaded) {
        // Сброс до чтения моделей: иначе прочитаем то, что сейчас сотрём.
        ensureModelSchema();
        oracleReset();
        loadModels();
        loaded = true;
    }

    // Свечи докачиваем понемногу: за тик не больше двух десятков монет,
    // чтобы не съесть весь лимит запросов к бирже.
    if (now - lastCandles >= 300) {
        lastCandles = now;
        fetchCandles();
    }
    // Ряды перестраиваем раз в час: внутри часа новых баров всё равно нет.
    if (now - lastMarket >= 3600) {
        lastMarket = now;
        rebuildMarket();
    }
    // Переобучение раз в шесть часов. Чаще незачем: за час журнал прибавляет
    // единицы примеров, а обучение стоит секунды процессора.
    if (now - lastTrain >= 6 * 3600) {
        lastTrain = now;
        auto m = market();
        if (!m || m->builtAt == 0) return;
        for (long long h : ORACLE_HZ) {
            trainVenue(false, *m, h);
            trainVenue(true, *m, h);
        }
    }
}

OracleVerdict oracleScore(const OracleInput& in, long long asOf, long long horizon) {
    OracleVerdict v;
    const int slot = in.perp ? 1 : 0;
    const int hi = hIndex(horizon);
    auto m = market();
    if (!m || m->builtAt == 0) return v;
    std::array<float, ORACLE_NF> feat{};
    featuresOf(*m, in, asOf > 0 ? asOf : hl::nowSec(), feat);
    // Лес не копируем: он на сотню килобайт, а зовут эту функцию на каждую
    // монету в списке.
    std::lock_guard<std::mutex> l(g_liveMutex);
    if (!g_live[slot][hi].have) return v;
    v.pUp = clampd(g_live[slot][hi].model.dir.p(feat.data()), 0.001, 0.999);
    v.known = true;
    return v;
}

std::vector<OracleReason> oracleWhy(const OracleInput& in, long long asOf,
                                    long long horizon, int n) {
    std::vector<OracleReason> out;
    const int slot = in.perp ? 1 : 0;
    const int hi = hIndex(horizon);
    auto m = market();
    if (!m || m->builtAt == 0) return out;
    std::array<float, ORACLE_NF> feat{};
    featuresOf(*m, in, asOf > 0 ? asOf : hl::nowSec(), feat);

    std::lock_guard<std::mutex> l(g_liveMutex);
    if (!g_live[slot][hi].have) return out;
    const Forest& f = g_live[slot][hi].model.dir;
    const double p0 = f.p(feat.data());
    std::vector<std::pair<double, int>> shift;
    for (int i = 0; i < ORACLE_NF; i++) {
        const float keep = feat[static_cast<size_t>(i)];
        const float med = static_cast<float>(f.med[static_cast<size_t>(i)]);
        if (std::fabs(static_cast<double>(keep - med)) < 1e-9) continue;
        feat[static_cast<size_t>(i)] = med;
        const double p1 = f.p(feat.data());
        feat[static_cast<size_t>(i)] = keep;
        if (std::fabs(p0 - p1) > 1e-6) shift.emplace_back(p0 - p1, i);
    }
    std::sort(shift.begin(), shift.end(),
              [](const std::pair<double, int>& a, const std::pair<double, int>& b) {
                  return std::fabs(a.first) > std::fabs(b.first);
              });
    for (int k = 0; k < n && k < static_cast<int>(shift.size()); k++)
        out.push_back(OracleReason{FEAT_NAME[shift[static_cast<size_t>(k)].second],
                                   shift[static_cast<size_t>(k)].first});
    return out;
}

OracleLevels oracleLevels(const OracleInput& in, long long asOf, long long horizon) {
    OracleLevels v;
    const int slot = in.perp ? 1 : 0;
    const int hi = hIndex(horizon);
    auto m = market();
    if (!m || m->builtAt == 0) return v;
    std::array<float, ORACLE_NF> feat{};
    featuresOf(*m, in, asOf > 0 ? asOf : hl::nowSec(), feat);
    std::lock_guard<std::mutex> l(g_liveMutex);
    if (!g_live[slot][hi].have || !g_live[slot][hi].model.levels) return v;
    v.up = clampd(g_live[slot][hi].model.up.value(feat.data()), 0.0, 3.0);
    v.dn = clampd(g_live[slot][hi].model.dn.value(feat.data()), 0.0, 1.0);
    v.known = v.up > 0 && v.dn > 0;
    return v;
}

OracleStats oracleStats(bool perp, long long horizon) {
    std::lock_guard<std::mutex> l(g_liveMutex);
    return g_live[perp ? 1 : 0][hIndex(horizon)].st;
}

bool oracleReady(bool perp, long long horizon) {
    std::lock_guard<std::mutex> l(g_liveMutex);
    return g_live[perp ? 1 : 0][hIndex(horizon)].have;
}

/* Все горизонты разом: один обход признаков на каждый, дальше зовущий
   сравнивает планы и берёт лучший. Считать признаки по два раза незачем —
   они от горизонта не зависят. */
std::vector<OracleView> oracleViews(const OracleInput& in, long long asOf) {
    std::vector<OracleView> out;
    const int slot = in.perp ? 1 : 0;
    auto m = market();
    if (!m || m->builtAt == 0) return out;
    std::array<float, ORACLE_NF> feat{};
    featuresOf(*m, in, asOf > 0 ? asOf : hl::nowSec(), feat);
    std::lock_guard<std::mutex> l(g_liveMutex);
    for (int hi = 0; hi < ORACLE_NH; hi++) {
        const Live& live = g_live[slot][hi];
        if (!live.have) continue;
        OracleView v;
        v.horizon = ORACLE_HZ[hi];
        v.known = true;
        v.pUp = clampd(live.model.dir.p(feat.data()), 0.001, 0.999);
        if (live.model.levels) {
            v.up = clampd(live.model.up.value(feat.data()), 0.0, 3.0);
            v.dn = clampd(live.model.dn.value(feat.data()), 0.0, 1.0);
            v.levels = v.up > 0 && v.dn > 0;
        }
        out.push_back(v);
    }
    return out;
}

const char* oracleFeatureName(int i) {
    if (i < 0 || i >= ORACLE_NF) return "?";
    return FEAT_NAME[i];
}
