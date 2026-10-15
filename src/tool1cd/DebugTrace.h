/*
    Диагностика для разбора ошибок чтения хранилища 1С (Wrong offset и др.).

    Включается переменной окружения TOOL1CD_DEBUG_SEEK=1.
    Файл журнала — TOOL1CD_DEBUG_LOG (по умолчанию вывод в stderr).
*/
#ifndef DEBUGTRACE_H
#define DEBUGTRACE_H

#include <string>
#include <cstdint>

namespace DebugTrace {

// Включена ли диагностика (TOOL1CD_DEBUG_SEEK=1).
bool enabled();

// Записать строку в журнал.
void log(const std::string &line);

// Открытие/закрытие файла.
void file_open(const std::string &path, const std::string &mode, bool ok, int64_t size);
void file_close(const std::string &path);

// Операция над потоком.
void op(const char *what, const std::string &file, int64_t offset, int64_t pos, int64_t size);

// Ошибка: печатает параметры, предысторию и стек вызовов.
void fail_seek(const std::string &file, const char *origin,
               int64_t offset, int64_t pos, int64_t size);

// Чтение блока .1CD.
void block_read(int64_t block_num, int64_t offset, int64_t page_size, int64_t file_size);

// Сигнатура страницы: что реально лежит в начале блока.
void block_signature(int64_t block_num, const unsigned char *sig, int len);

// Стек вызовов (адреса) — для addr2line.
void backtrace_now(const char *why);

} // namespace DebugTrace

#endif
