#include "beneficiary_stats.h"

#include <sstream>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <mutex>

#include "utils.h"  // toLower

using json = nlohmann::json;

// Диагностический модуль: копит частоту адресов, на которые чаще всего
// натыкается анализатор в ветке "DEX interaction" (swap виден, но flow
// кошелька не подтверждён) - чтобы находить кандидатов для таблицы роутеров.
// Полностью самодостаточен: main.cpp вызывает только recordBeneficiarySignal
// в точке классификации и handleBeneficiaryCommand в диспетчере команд.
// Для удаления фичи достаточно убрать эти два вызова, #include и сами файлы.
namespace {

// Ограничение размера — защита от неограниченного роста памяти на случай
// патологически большого числа уникальных адресов (например, CREATE2-контракты).
constexpr size_t BENEFICIARY_TALLY_CAP = 20000;

std::mutex g_benefTallyMutex;
std::unordered_map<std::string, uint64_t> g_benefToTally;   // tx.to (вызываемый контракт) -> счётчик
std::unordered_map<std::string, uint64_t> g_benefAddrTally; // адрес-бенефициар -> счётчик

std::string buildTopAddrList(const std::unordered_map<std::string, uint64_t>& m, size_t topN, bool labelRouters) {
    std::vector<std::pair<std::string, uint64_t>> v(m.begin(), m.end());
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    std::stringstream ss;
    size_t n = std::min(topN, v.size());
    for (size_t i = 0; i < n; ++i) {
        ss << (i + 1) << ". <code>" << v[i].first << "</code> — " << v[i].second;
        if (labelRouters) {
            std::string lbl = lookupRouterLabel(v[i].first);
            ss << (lbl.empty() ? " ❓" : (" (" + lbl + ")"));
        }
        ss << "\n";
    }
    if (v.empty()) ss << "(нет данных — подождите трафика)\n";
    return ss.str();
}

} // namespace

void recordBeneficiarySignal(const nlohmann::json& tx, const TxResult& res) {
    if (res.flowBeneficiaries.empty()) return;
    std::string to = (tx.is_object() && tx.contains("to") && tx["to"].is_string())
                          ? toLower(tx["to"].get<std::string>()) : "";
    std::lock_guard<std::mutex> lk(g_benefTallyMutex);
    if (!to.empty() && (g_benefToTally.count(to) || g_benefToTally.size() < BENEFICIARY_TALLY_CAP))
        g_benefToTally[to]++;

    // Формат res.flowBeneficiaries: "token:addr:amount;token:addr:amount;..."
    const std::string& s = res.flowBeneficiaries;
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t semi = s.find(';', pos);
        std::string entry = s.substr(pos, semi == std::string::npos ? std::string::npos : semi - pos);
        size_t c1 = entry.find(':');
        size_t c2 = (c1 == std::string::npos) ? std::string::npos : entry.find(':', c1 + 1);
        if (c1 != std::string::npos && c2 != std::string::npos) {
            std::string addr = toLower(entry.substr(c1 + 1, c2 - c1 - 1));
            if (!addr.empty() && (g_benefAddrTally.count(addr) || g_benefAddrTally.size() < BENEFICIARY_TALLY_CAP))
                g_benefAddrTally[addr]++;
        }
        if (semi == std::string::npos) break;
        pos = semi + 1;
    }
}

bool handleBeneficiaryCommand(const std::string& chatId, const std::string& text) {
    if (text != "/beneficiaries") return false;
    if (chatId != OWNER_CHAT_ID) {
        sendMsg(chatId, "Access denied.");
        return true;
    }

    size_t toCount, addrCount;
    std::string toList, addrList;
    {
        std::lock_guard<std::mutex> lk(g_benefTallyMutex);
        toCount = g_benefToTally.size();
        addrCount = g_benefAddrTally.size();
        toList = buildTopAddrList(g_benefToTally, 15, true);
        addrList = buildTopAddrList(g_benefAddrTally, 15, false);
    }

    std::stringstream ss;
    ss << "\U0001F52C <b>DEX interaction \u2014 куда обращаются</b>\n\n"
       << "<b>Топ адресов вызова (tx.to)</b>, уникальных: " << toCount << "\n"
       << toList
       << "\n<b>Топ адресов-бенефициаров</b>, уникальных: " << addrCount << "\n"
       << addrList
       << "\n❓ = ещё не в таблице роутеров";
    sendMsg(chatId, ss.str());
    return true;
}
