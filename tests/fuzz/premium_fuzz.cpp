// Фаззинг правил оплаты (libFuzzer, clang): tests/run.sh гоняет его
// FUZZ_SECONDS секунд (по умолчанию 20), CI — минуту.
//
//   clang++ -std=c++20 -g -O1 -I. -fsanitize=fuzzer,address,undefined \
//       tests/fuzz/premium_fuzz.cpp -o premium_fuzz
//   ./premium_fuzz -max_total_time=60 tests/fuzz/corpus
//
// Вход — байты из чужих рук: тело ответа обозревателя, нагрузка счёта,
// адрес, памятка. На любом входе — ни падения, ни исключения, ни ошибки
// памяти, и каждое правило держит свой инвариант. Нарушение — abort с
// причиной; libFuzzer сохраняет вход, и он воспроизводится.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include "../../premium_logic.h"

namespace {

void require(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "нарушено: %s\n", what);
        std::abort();
    }
}

bool isRaw(const std::string& a) {
    const size_t c = a.find(':');
    if (c == std::string::npos || a.size() != c + 65) return false;
    for (size_t i = c + 1; i < a.size(); ++i)
        if (!((a[i] >= '0' && a[i] <= '9') || (a[i] >= 'A' && a[i] <= 'F'))) return false;
    return true;
}

constexpr std::string_view WALLET_RAW = "0:C088D62FCB62942308C47200F56713D5C3E08FC1D93859B050C2956C410E63B0";

void strings(std::string_view s) {
    const std::string str(s);
    long long v = 0;
    if (premium::strictInt(s, v)) {
        require(v >= 0 && !s.empty() && s.size() <= 18, "strictInt: только цифры, до 18");
        for (const char c : s) require(c >= '0' && c <= '9', "strictInt: не цифра");
    }
    const std::string raw = premium::tonRawAddress(s);
    if (!raw.empty()) {
        require(isRaw(raw), "tonRawAddress: вид 0:HEX64");
        require(premium::tonRawAddress(raw) == raw, "tonRawAddress: сырой вид — неподвижная точка");
    }
    const std::string memo = premium::memoKey(s);
    require(memo.size() <= 64, "memoKey: длина");
    for (const char c : memo) require(c != ' ' && c != '\n' && c != '\r' && c != '\t' && !(c >= 'a' && c <= 'z'),
                                      "memoKey: пробелы и строчные");
    require(premium::memoKey(memo) == memo, "memoKey: повтор ничего не меняет");
    const std::string log = premium::logSafe(s, 300);
    require(log.size() <= 300 + 3, "logSafe: длина");
    for (const char c : log) require(static_cast<unsigned char>(c) >= 0x20 && c != 0x7F, "logSafe: управляющий знак");
    std::string chat;
    long long until = 0;
    if (premium::introBinding(s, chat, until)) require(until > 0 && !chat.empty(), "introBinding: срок и чат");
    for (const long long now : {0LL, 1800000000LL}) {
        const auto verdict = premium::checkStars(s, "XTR", 150, "42", now);
        if (verdict == premium::Checkout::Ok) {
            const auto* plan = premium::starPlan(s);
            require(plan != nullptr && plan->stars == 150, "checkStars: цена тарифа");
            require(premium::introBinding(s, chat, until) && chat == "42" && now <= until + premium::INTRO_GRACE_SEC,
                    "checkStars: вводная — своему и в окно");
        }
    }
}

void transfers(const nlohmann::json& j) {
    const auto check = [](const nlohmann::json& t) {
        const auto pay = premium::usdtTransfer(t, WALLET_RAW);
        if (!pay) return;
        require(pay->units >= premium::USDT_MIN_UNITS, "usdtTransfer: от доллара");
        require(!pay->hash.empty() && pay->hash.size() <= 128 && pay->at > 0, "usdtTransfer: хеш и время");
        require(!pay->memo.empty() && premium::memoKey(pay->memo) == pay->memo, "usdtTransfer: памятка");
        require(premium::tonRawAddress(premium::strField(t, "jetton_master")) == premium::USDT_MASTER,
                "usdtTransfer: только USD₮");
        require(premium::tonRawAddress(premium::strField(t, "destination")) == WALLET_RAW,
                "usdtTransfer: только на наш кошелёк");
        require(!t.at("transaction_aborted").get<bool>(), "usdtTransfer: не сорвался");
        (void)premium::paidEnough(pay->units, pay->units);
        require(premium::usdText(pay->units).find('.') != std::string::npos, "usdText: формат");
    };
    check(j);
    if (j.is_object() && j.contains("jetton_transfers") && j["jetton_transfers"].is_array())
        for (const auto& t : j["jetton_transfers"]) check(t);
    if (j.is_array())
        for (const auto& t : j) check(t);
    const long long a = premium::intField(j, "total_amount");
    require(a >= -1, "intField: -1 или целое ≥ 0");
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::string_view s(reinterpret_cast<const char*>(data), size);  // NOLINT: граница с libFuzzer
    try {
        strings(s);
        const nlohmann::json j = nlohmann::json::parse(s, nullptr, false);
        if (!j.is_discarded()) transfers(j);
        for (int days : {-1, 0, 1, 30, 365, premium::MAX_GRANT_DAYS, premium::MAX_GRANT_DAYS + 1}) {
            long long n = 0;
            (void)premium::strictInt(s.substr(0, 18), n);
            const auto t = premium::extendTerm(1, n, n, 1800000000, days);
            if (t) require(t->expire > 1800000000 && t->expire >= t->start, "extendTerm: срок в будущем");
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "исключение: %s\n", e.what());
        std::abort();
    }
    return 0;
}
