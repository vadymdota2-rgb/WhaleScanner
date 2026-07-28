#pragma once
#include <string>

enum class Lang { EN, RU, ES, PT, FR, TR, AR };

Lang langFromCode(const std::string& code);
std::string tr(Lang lang, const std::string& key);

// Языки сверх базовых EN/RU живут в отдельных файлах и подключаются вот так.
// Каждый новый язык - один новый файл и ноль правок в существующих; ключ, до
// которого у переводчика не дошли руки, молча откатывается на английский, а не
// превращается в пустоту на экране.
const char* trEs(const std::string& key);   // nullptr, если ключа нет
const char* trPt(const std::string& key);   // то же для португальского
const char* trFr(const std::string& key);   // то же для французского
const char* trTr(const std::string& key);   // то же для турецкого
// Арабский: письмо справа налево. Сами строки обычные, но порядок арабского
// текста и латинских вставок (суммы, тикеры) расставляет уже Unicode при
// отрисовке - проверять надо на экране, а не в коде.
const char* trAr(const std::string& key);

std::string pluralRu(long long n, const std::string& one, const std::string& few, const std::string& many);
