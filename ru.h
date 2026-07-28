#pragma once
#include <string>

enum class Lang { EN, RU, ES };

Lang langFromCode(const std::string& code);
std::string tr(Lang lang, const std::string& key);

// Языки сверх базовых EN/RU живут в отдельных файлах и подключаются вот так.
// Каждый новый язык - один новый файл и ноль правок в существующих; ключ, до
// которого у переводчика не дошли руки, молча откатывается на английский, а не
// превращается в пустоту на экране.
const char* trEs(const std::string& key);   // nullptr, если ключа нет

std::string pluralRu(long long n, const std::string& one, const std::string& few, const std::string& many);
