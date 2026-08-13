#pragma once
#include <string>

enum class Lang { EN, RU, ES, PT, FR, TR, AR, PL, DE, UK, HI, ID, VI, KO, ZH, JA };

Lang langFromCode(const std::string& code);
std::string tr(Lang lang, const std::string& key);

void checkTranslations();

const char* trEs(const std::string& key);
const char* trPt(const std::string& key);
const char* trFr(const std::string& key);
const char* trTr(const std::string& key);
const char* trAr(const std::string& key);
const char* trPl(const std::string& key);
const char* trDe(const std::string& key);
const char* trUk(const std::string& key);
const char* trHi(const std::string& key);
const char* trId(const std::string& key);
const char* trVi(const std::string& key);
const char* trKo(const std::string& key);
const char* trZh(const std::string& key);
const char* trJa(const std::string& key);

std::string pluralRu(long long n, const std::string& one, const std::string& few, const std::string& many);
