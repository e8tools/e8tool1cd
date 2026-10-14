/*
    Tool1CD library provides access to 1CD database files.
    Copyright © 2009-2017 awa
    Copyright © 2017-2018 E8 Tools contributors

    This file is part of Tool1CD Library.

    Tool1CD Library is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Tool1CD Library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with Tool1CD Library.  If not, see <http://www.gnu.org/licenses/>.
*/
#include "ConsoleOutput.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace Console {

#ifdef _WIN32

namespace {

HANDLE console_handle(std::FILE *stream)
{
	return GetStdHandle(stream == stderr ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
}

// Запись в настоящую консоль: широкие символы, кодовая страница не участвует.
bool write_to_console(HANDLE handle, const std::string &utf8_text)
{
	if (utf8_text.empty()) {
		return true;
	}
	int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8_text.data(),
					  static_cast<int>(utf8_text.size()), nullptr, 0);
	if (wlen <= 0) {
		return false;
	}
	std::wstring wide(static_cast<size_t>(wlen), L'\0');
	if (MultiByteToWideChar(CP_UTF8, 0, utf8_text.data(),
				static_cast<int>(utf8_text.size()), &wide[0], wlen) <= 0) {
		return false;
	}
	const wchar_t *p = wide.data();
	size_t left = wide.size();
	while (left > 0) {
		DWORD written = 0;
		if (!WriteConsoleW(handle, p, static_cast<DWORD>(left), &written, nullptr) || written == 0) {
			return false;
		}
		p += written;
		left -= written;
	}
	return true;
}

} // namespace

void write(std::FILE *stream, const std::string &utf8_text)
{
	HANDLE handle = console_handle(stream);
	DWORD mode = 0;
	// GetConsoleMode не проходит, если вывод перенаправлен в файл или канал, —
	// тогда остаётся обычная запись UTF-8 байтами.
	if (handle != INVALID_HANDLE_VALUE && handle != nullptr && GetConsoleMode(handle, &mode)
			&& write_to_console(handle, utf8_text)) {
		return;
	}
	std::fwrite(utf8_text.data(), 1, utf8_text.size(), stream);
	std::fflush(stream);
}

#else

void write(std::FILE *stream, const std::string &utf8_text)
{
	std::fwrite(utf8_text.data(), 1, utf8_text.size(), stream);
	std::fflush(stream);
}

#endif

void write_line(std::FILE *stream, const std::string &utf8_text)
{
	write(stream, utf8_text);
	write(stream, "\n");
}

}
