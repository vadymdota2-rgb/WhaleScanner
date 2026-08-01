#pragma once
#include <string>

enum class Lang { EN, RU, ES, PT, FR, TR, AR };

Lang langFromCode(const std::string& code);
std::string tr(Lang lang, const std::string& key);

const char* trEs(const std::string& key);
const char* trPt(const std::string& key);
const char* trFr(const std::string& key);
const char* trTr(const std::string& key);
const char* trAr(const std::string& key);

std::string pluralRu(long long n, const std::string& one, const std::string& few, const std::string& many);
