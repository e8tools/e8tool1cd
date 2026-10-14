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
#ifndef CONSOLE_OUTPUT_H_INCLUDED
#define CONSOLE_OUTPUT_H_INCLUDED

#include <cstdio>
#include <string>

// Вывод текста в консоль. Строки в программе хранятся в UTF-8, а консоль
// Windows работает в своей кодовой странице (обычно cp866), поэтому прямая
// запись байтов UTF-8 через cout/cerr даёт нечитаемые символы. На Windows
// текст уходит в консоль через WriteConsoleW — широкими символами, минуя
// кодовую страницу; при перенаправленном выводе (в файл или канал) пишутся
// байты UTF-8, как и раньше.
namespace Console {

// Записать строку UTF-8 в поток.
void write(std::FILE *stream, const std::string &utf8_text);

// Записать строку UTF-8 с переводом строки.
void write_line(std::FILE *stream, const std::string &utf8_text);

}

#endif // CONSOLE_OUTPUT_H_INCLUDED
