#include <iostream>
#include <string>
#include <vector>
#include <set>
#include <thread>
#include <chrono>
#include <curl/curl.h>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <map>
#include <algorithm>
#include <sqlite3.h>

#include "json.hpp"

using json = nlohmann::json;

// ==================== КОНФИГУРАЦИЯ ====================

struct Whale {
    std::string address;
    std::string name;
};

const std::vector<Whale> WHALES = {
    {"0xb81be888587704aa77c581e01299abe3667a7897", "Кит №1 Siren 🐋"},
};

const std::string BOT_NAME = "🐋 Whale";
const std::string TELEGRAM_BOT_TOKEN = "8845630927:AAEb0Xhm7DUn7ihwPPhZVaaLN3C7hRM2FS0";
const std::string OWNER_CHAT_ID = "546348566";

std::vector<std::string> SUBSCRIBERS = {"546348566"};
double THRESHOLD_USD = 100.0;

const std::string BSC_RPC_URL = "https://bsc-dataseed.bnbchain.org";
const std::string TRANSFER_TOPIC = "0xddf252ad1be2c89b69c2b068fc378daa952ba7f163c4a11628f55a4df523b3ef";
const std::string USDT_ADDRESS = "0x55d398326f99059ff775485246999027b3197955";

const std::string PANCAKE_ROUTER_V2 = "0x10ed43c718714eb63d5aa57b78b54704e256024e";
const std::string PANCAKE_ROUTER_V1 = "0x05ff2b0db69458a0750badebc4f9e13add608c7f";
const std::string PANCAKE_FACTORY = "0xca143ce32fe78f1f7012d318b25d93c61a5a5c5e";

const std::map<std::string, std::string> KNOWN_TOKENS = {
    {"0x55d398326f99059ff775485246999027b3197955", "USDT"},
    {"0x8ac76a51cc950d9822d68b83fe1ad97b32cd580d", "USDC"},
    {"0xe9e7cea3dedca5984780bafc599bd69add087d56", "BUSD"},
    {"0x1af3f329e8be154074d8769d1ffa4ee058b1dbc3", "DAI"},
    {"0x7130d2a12b9bcbfae4f2634d864a1ee1ce3ead9c", "BTCB"},    {"0x2170ed0880ac9a755fd29b2688956bd959f933f8", "ETH"},
    {"0xbb4cdb9cbd36b01bd1cbaebf2de08d9173bc095c", "WBNB"},
    {"0x0e09fabb73bd3ade0a17ecc321fd13a19e81ce82", "CAKE"},
    {"0xcf6bb5389c92bdda8a3747ddb454cb7a64626c63", "XVS"},
    {"0x1d2f0da169ceb9fc7b3144628db156f3f6c60dbe", "XRP"},
    {"0x3ee2200efb3400fabb9aacf31297cbdd1d435d47", "ADA"},
    {"0x7083609fce4d1d8dc0c979aab8c869ea2c873402", "DOT"},
    {"0xba2ae424d960c26247dd6c32edc70b295c744c43", "DOGE"},
    {"0xf8a0bf9cf54bb92f17374d9e9a321e6a111a51bd", "LINK"},
    {"0x4338665cbb7b2485a8855a139b75d5e34ab0db94", "LTC"},
    {"0x8ff795a6f4d97e7887c79bea79aba5cc76444adf", "BCH"},
    {"0x0d8ce2a99bb6e3b7db580ed848240e4a0f9ae153", "FIL"},
    {"0xfb6115445bff7b52feb98650c87f44907e58f802", "AAVE"},
    {"0x5f0da599bb2cccfcf67dfd5d6247d2600d7070f7", "MKR"},
    {"0x9ac98382f0bc08e75067ca9c53a262b9b75f1fb8", "SNX"},
    {"0x16939ef78684453bfdfb47825f8a5f714f12623a", "CRV"},
    {"0x52ce071bd9b1c4b00a0b92d298c512478cad67e8", "COMP"},
    {"0x947950bcc74888a40ffa2593c5798f11fc9124c4", "SUSHI"},
    {"0x88f1a5ae2a3bf98aeaf342d26b30a79438c9142e", "YFI"},
    {"0x111111111117dc0aa78b770fa6a738034120c302", "1INCH"},
    {"0xc748673057861a797275cd8a068abb95a902e8de", "BabyDoge"},
    {"0x2859e4544c4bb03966803b044a93563bd2d0dd4d", "SHIB"},
    {"0x67b725d7e342d7b611fa85e859df9697d9378b2e", "SAND"},
    {"0x3203c9e46ca618c8c1ce5dc67e7e9d75f5da2377", "MANA"},
    {"0x715d400f88c167884bbcc41c5fea407ed4d2f8a0", "AXS"},
    {"0x7ddee176f665cd201f93eede625770e2fd911990", "GALA"},
    {"0x3f382dbd960e3a9bbceae22651e88158d2791550", "CHZ"},
    {"0x2ff3d0f6990a40261c66e1ff2017acbc282eb6d0", "ENJ"},
    {"0x8f0528ce5ef7b51152a59745befdd91d97091d2f", "ALPACA"},
    {"0x56b6fb708fc5732dec1afc8d8556423a2edccbd6", "EOS"},
    {"0x3d6545b08693dae087e957cb1180ee38b9e3c25e", "ETC"},
    {"0xb3c11196a4f3b1da7c23d9fb0a319f7e79e83e0e", "TRX"},
    {"0x997a58129890bbda032231a52ed1ddc845fc18e1", "SIREN"}
};

// ==================== SQLITE ====================
sqlite3* db = nullptr;

void initDatabase() {
    int rc = sqlite3_open("whale_history.db", &db);
    if (rc) { std::cerr << "Cannot open database: " << sqlite3_errmsg(db) << std::endl; return; }
    const char* sql = "CREATE TABLE IF NOT EXISTS transactions (id INTEGER PRIMARY KEY AUTOINCREMENT, tx_hash TEXT UNIQUE, whale_name TEXT, action TEXT, token TEXT, usd_value REAL, timestamp DATETIME DEFAULT CURRENT_TIMESTAMP);";
    char* errMsg = nullptr;
    rc = sqlite3_exec(db, sql, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) { std::cerr << "SQL error: " << errMsg << std::endl; sqlite3_free(errMsg); }
}

void saveTransaction(const std::string& txHash, const std::string& whaleName, const std::string& action, const std::string& token, double usdValue) {
    if (!db) return;
    std::string sql = "INSERT OR IGNORE INTO transactions (tx_hash, whale_name, action, token, usd_value) VALUES ('" + txHash + "', '" + whaleName + "', '" + action + "', '" + token + "', " + std::to_string(usdValue) + ");";    char* errMsg = nullptr;
    sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &errMsg);
    if (errMsg) sqlite3_free(errMsg);
}

// ==================== УТИЛИТЫ ====================
size_t writeCallback(void* contents, size_t size, size_t nmemb, std::string* s) {
    s->append((char*)contents, size * nmemb);
    return size * nmemb;
}

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

double hexToDouble(const std::string& hex) {
    if (hex.empty() || hex == "0x" || hex == "0x0") return 0.0;
    std::string cleanHex = hex;
    if (cleanHex.substr(0, 2) == "0x") cleanHex = cleanHex.substr(2);
    if (cleanHex.empty()) return 0.0;
    try { unsigned long long val = std::stoull(cleanHex, nullptr, 16); return static_cast<double>(val); }
    catch (...) { return 0.0; }
}

std::string extractAddress(const std::string& hex) {
    if (hex.length() < 42) return "";
    return "0x" + toLower(hex.substr(hex.length() - 40));
}

// ==================== HTTP ====================
std::string httpRequest(const std::string& url, const std::string& postData = "") {
    CURL* curl = curl_easy_init();
    if (!curl) return "";
    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    if (!postData.empty()) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postData.c_str());
        struct curl_slist* headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    return response;
}
// ==================== TELEGRAM ====================
void sendToUser(const std::string& chatId, const std::string& text) {
    std::string url = "https://api.telegram.org/bot" + TELEGRAM_BOT_TOKEN + "/sendMessage";
    json payload;
    payload["chat_id"] = chatId;
    payload["text"] = text;
    payload["parse_mode"] = "HTML";
    payload["disable_web_page_preview"] = true;
    httpRequest(url, payload.dump());
}

void sendToAll(const std::string& text) {
    for (const auto& chatId : SUBSCRIBERS) {
        sendToUser(chatId, text);
    }
}

void addSubscriber(const std::string& chatId) {
    for (const auto& id : SUBSCRIBERS) {
        if (id == chatId) return;
    }
    SUBSCRIBERS.push_back(chatId);
    std::cout << "[SUBSCRIBER] Добавлен: " << chatId << std::endl;
    sendToUser(chatId, "✅ <b>Добро пожаловать в Whale Siren!</b>\n\nТеперь ты будешь получать уведомления о крупных сделках китов 🐋");
}

json getTelegramUpdates(long offset) {
    std::string url = "https://api.telegram.org/bot" + TELEGRAM_BOT_TOKEN + "/getUpdates?offset=" + std::to_string(offset) + "&timeout=1";
    std::string response = httpRequest(url);
    if (response.empty()) return json();
    try { return json::parse(response); }
    catch (...) { return json(); }
}

void handleTelegramCommands() {
    static long lastUpdateId = 0;
    json updates = getTelegramUpdates(lastUpdateId);
    if (updates.is_null() || !updates.contains("result")) return;
    
    for (const auto& update : updates["result"]) {
        if (!update.contains("message")) continue;
        if (!update["message"].contains("text")) continue;
        
        long updateId = update["update_id"].get<long>();
        lastUpdateId = updateId + 1;
        
        std::string text = update["message"]["text"].get<std::string>();
        std::string chatId = std::to_string(update["message"]["chat"]["id"].get<long>());
        
        addSubscriber(chatId);        bool isOwner = (chatId == OWNER_CHAT_ID);
        
        if (text == "/start") {
            sendToUser(chatId, "🐋 <b>Whale Siren</b>\n\nПривет! Я слежу за крупными сделками китов в сети BSC.\n\nТы подписан на уведомления!");
        }
        else if (text.find("/threshold") == 0) {
            if (!isOwner) {
                sendToUser(chatId, "⚠️ Менять поріг може тільки власник!");
                continue;
            }
            size_t spacePos = text.find(' ');
            if (spacePos == std::string::npos) {
                std::stringstream ss; ss << std::fixed << std::setprecision(0) << THRESHOLD_USD;
                sendToUser(chatId, "📊 Поточний поріг: $" + ss.str());
                continue;
            }
            try {
                double newThreshold = std::stod(text.substr(spacePos + 1));
                if (newThreshold < 100 || newThreshold > 1000000) {
                    sendToUser(chatId, "⚠️ Порог від $100 до $1,000,000");
                    continue;
                }
                THRESHOLD_USD = newThreshold;
                std::stringstream ss; ss << std::fixed << std::setprecision(0) << newThreshold;
                sendToAll("✅ Порог змінено: $" + ss.str());
            } catch (...) {
                sendToUser(chatId, "❌ Помилка! Вкажи число.");
            }
        }
        else if (text == "/status") {
            std::string msg = "📊 <b>Статус</b>\n\n🐋 Китів: " + std::to_string(WHALES.size()) + "\n💰 Порог: $" + std::to_string((int)THRESHOLD_USD) + "\n👥 Підписників: " + std::to_string(SUBSCRIBERS.size());
            sendToUser(chatId, msg);
        }
        else if (text == "/help") {
            std::string msg = "🤖 <b>Команди</b>\n\n/start - підписка\n/status - статус\n/threshold - поріг";
            if (isOwner) msg += "\n/threshold X - змінити";
            sendToUser(chatId, msg);
        }
    }
}

// ==================== BSC RPC ====================
json rpcCall(const std::string& method, const json& params) {
    json request;
    request["jsonrpc"] = "2.0";
    request["method"] = method;
    request["params"] = params;
    request["id"] = 1;
    std::string response = httpRequest(BSC_RPC_URL, request.dump());
    if (response.empty()) return json();    try { return json::parse(response)["result"]; }
    catch (...) { return json(); }
}

json getLatestBlock() { return rpcCall("eth_getBlockByNumber", {"latest", true}); }
json getTransactionReceipt(const std::string& txHash) { return rpcCall("eth_getTransactionReceipt", {txHash}); }

// ==================== ЦЕНЫ ====================
std::map<std::string, std::pair<double, time_t>> priceCache;

double getTokenPriceUsd(const std::string& tokenAddress) {
    std::string lowerAddr = toLower(tokenAddress);
    if (priceCache.count(lowerAddr)) {
        auto& cached = priceCache[lowerAddr];
        if (time(nullptr) - cached.second < 60) return cached.first;
    }
    if (lowerAddr == "0xbb4cdb9cbd36b01bd1cbaebf2de08d9173bc095c") {
        std::string response = httpRequest("https://api.binance.com/api/v3/ticker/price?symbol=BNBUSDT");
        try {
            json j = json::parse(response);
            double price = std::stod(j["price"].get<std::string>());
            priceCache[lowerAddr] = {price, time(nullptr)};
            return price;
        } catch (...) { return 0.0; }
    }
    std::string url = "https://api.dexscreener.com/latest/dex/tokens/" + tokenAddress;
    std::string response = httpRequest(url);
    try {
        json j = json::parse(response);
        if (j.contains("pairs") && j["pairs"].is_array() && j["pairs"].size() > 0) {
            double maxLiquidity = 0, bestPrice = 0;
            for (const auto& pair : j["pairs"]) {
                if (pair.contains("liquidity") && pair["liquidity"].contains("usd")) {
                    double liq = pair["liquidity"]["usd"].get<double>();
                    if (liq > maxLiquidity && pair.contains("priceUsd")) {
                        maxLiquidity = liq;
                        bestPrice = std::stod(pair["priceUsd"].get<std::string>());
                    }
                }
            }
            if (bestPrice > 0) { priceCache[lowerAddr] = {bestPrice, time(nullptr)}; return bestPrice; }
        }
    } catch (...) {}
    return 0.0;
}

// ==================== АНАЛИЗ ====================
struct Transfer { std::string tokenAddress; std::string from; std::string to; double amount; double usdValue; };

std::vector<Transfer> analyzeTransaction(const std::string& txHash) {    std::vector<Transfer> transfers;
    json receipt = getTransactionReceipt(txHash);
    if (receipt.is_null() || !receipt.contains("logs")) return transfers;
    for (const auto& log : receipt["logs"]) {
        if (!log.contains("topics") || log["topics"].size() < 3) continue;
        if (log["topics"][0].get<std::string>() != TRANSFER_TOPIC) continue;
        Transfer t;
        t.tokenAddress = toLower(log["address"].get<std::string>());
        t.from = extractAddress(log["topics"][1].get<std::string>());
        t.to = extractAddress(log["topics"][2].get<std::string>());
        t.amount = hexToDouble(log["data"].get<std::string>()) / std::pow(10.0, 18);
        t.usdValue = t.amount * getTokenPriceUsd(t.tokenAddress);
        transfers.push_back(t);
    }
    return transfers;
}

// ==================== ГЛАВНАЯ ЛОГИКА ====================
void processTransaction(const std::string& txHash, const std::string& whaleAddr, const std::string& whaleName) {
    std::vector<Transfer> transfers = analyzeTransaction(txHash);
    if (transfers.empty()) return;
    
    std::string otherToken;
    std::string action = "";
    double totalUsd = 0.0;
    std::string counterparty;
    bool isSwap = false;
    
    for (const auto& t : transfers) {
        bool fromWhale = (t.from == whaleAddr);
        bool toWhale = (t.to == whaleAddr);
        if (!fromWhale && !toWhale) continue;
        if (fromWhale && toWhale) continue;
        
        totalUsd += t.usdValue;
        std::string otherAddr = fromWhale ? t.to : t.from;
        std::string lowerOther = toLower(otherAddr);
        
        if (lowerOther == PANCAKE_ROUTER_V2 || lowerOther == PANCAKE_ROUTER_V1 || lowerOther == PANCAKE_FACTORY) {
            isSwap = true;
        }
        
        if (t.tokenAddress == USDT_ADDRESS || toLower(t.tokenAddress) == USDT_ADDRESS) {
            action = toWhale ? "🔴 ПРОДАЖ" : "🟢 ПОКУПКА";
        } else {
            otherToken = t.tokenAddress;
        }
        counterparty = otherAddr;
    }
        if (totalUsd < THRESHOLD_USD) return;
    
    std::string tokenName = otherToken.empty() ? "USDT" : (KNOWN_TOKENS.count(otherToken) ? KNOWN_TOKENS.at(otherToken) : "Unknown");
    
    // ✅ ИСПРАВЛЕНО: добавлен totalUsd в поток!
    std::stringstream ss;
    ss << std::fixed << std::setprecision(2) << totalUsd;
    
    std::string msg = "<b>" + BOT_NAME + "</b>\n\n👤 <b>" + whaleName + "</b>\n";
    
    if (!isSwap) {
        msg += "📤 <b>ПЕРЕВОД</b>\n\n💰 Сумма: <b>$" + ss.str() + "</b>\n";
        if (tokenName != "Unknown") msg += "🪙 Токен: <b>" + tokenName + "</b>\n";
        msg += "📍 Кому: <code>" + counterparty.substr(0, 10) + "..." + counterparty.substr(38) + "</code>\n";
    } else {
        msg += action + "\n\n";
        if (action == "🔴 ПРОДАЖ") {
            msg += "Продав: <b>" + tokenName + "</b>\nОтримав: <b>$" + ss.str() + "</b>\n";
        } else {
            msg += "Купив: <b>" + tokenName + "</b>\nВитратив: <b>$" + ss.str() + "</b>\n";
        }
    }
    msg += "🔗 <a href=\"https://bscscan.com/tx/" + txHash + "\">Транзакция</a>";
    
    sendToAll(msg);
    saveTransaction(txHash, whaleName, action, tokenName, totalUsd);
    std::cout << "[" << whaleName << "] " << (isSwap ? action : "ПЕРЕВОД") << " " << tokenName << " - $" << totalUsd << std::endl;
}

// ==================== MAIN ====================
int main() {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    initDatabase();
    std::cout << BOT_NAME << " STARTED" << std::endl;
    std::cout << "Monitoring " << WHALES.size() << " whale(s)" << std::endl;
    std::cout << "Threshold: $" << THRESHOLD_USD << std::endl;
    std::cout << "==========================" << std::endl;
    
    std::set<std::string> seenHashes;
    while (true) {
        try {
            handleTelegramCommands();
            json block = getLatestBlock();
            if (block.is_null() || !block.contains("transactions")) {
                std::this_thread::sleep_for(std::chrono::seconds(2));
                continue;
            }
            for (const auto& tx : block["transactions"]) {
                std::string txHash = tx["hash"].get<std::string>();
                if (seenHashes.count(txHash)) continue;                seenHashes.insert(txHash);
                std::string from = toLower(tx["from"].get<std::string>());
                std::string to = "";
                if (tx.contains("to") && !tx["to"].is_null()) to = toLower(tx["to"].get<std::string>());
                for (const auto& whale : WHALES) {
                    if (from == whale.address || to == whale.address) {
                        processTransaction(txHash, whale.address, whale.name);
                    }
                }
            }
            if (seenHashes.size() > 10000) seenHashes.clear();
        } catch (const std::exception& e) {
            std::cerr << "Error: " << e.what() << std::endl;
        }
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    if (db) sqlite3_close(db);
    curl_global_cleanup();
    return 0;
}
