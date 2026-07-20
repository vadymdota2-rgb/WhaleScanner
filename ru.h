#pragma once
#include <string>

enum class Lang { EN, RU };

Lang langFromCode(const std::string& code);
std::string tr(Lang lang, const std::string& key);

std::string pluralRu(long long n, const std::string& one, const std::string& few, const std::string& many);
