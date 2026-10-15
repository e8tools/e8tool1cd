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

#include <regex>

#include "PackDirectory.h"
#include "DetailedException.h"
#include "Common.h"
#include "DetailedException.h"

boost::filesystem::path object_path(const boost::filesystem::path &rootpath, const std::string &datahash)
{
	// aabbccddeeff -> ./data/objects/aa/bbccddeeff
	// Хеш в имени файла хранится без первых двух символов (они образуют имя
	// каталога). Строку обрезаем до DATAHASH_FIELD_LENGTH*2: она может прийти
	// с лишними байтами, и тогда в путь попали бы лишние символы.
	std::string hex = datahash;
	if (hex.size() > DATAHASH_FIELD_LENGTH * 2) {
		hex.resize(DATAHASH_FIELD_LENGTH * 2);
	}
	if (hex.size() < 2) {
		return rootpath / hex;
	}
	return rootpath / hex.substr(0, 2) / hex.substr(2);
}


PackDirectory::PackDirectory() {

}

PackDirectory::PackDirectory(boost::filesystem::path& init_path) {
	init(init_path);
}

PackDirectory::~PackDirectory() {
}

void PackDirectory::init(boost::filesystem::path& init_path) {
	packdates.clear();
	objects_path.clear();
	available = false;

	boost::filesystem::path subpath = init_path / "data" / "pack";
	if(!directory_exists(subpath)) {
		// Каталога с данными хранилища может не быть (например, если есть только
		// файл .1CD). Это не ошибка: данные читаются из blob-полей таблиц.
		return;
	}
	std::regex pack_mask("pack-.*\\.ind");
	boost::filesystem::directory_iterator dit(subpath), dend;
	for (; dit != dend; dit++) {
		boost::filesystem::path current_path = dit->path();
		if (!std::regex_match(current_path.filename().string(), pack_mask)) {
			continue;
		}
		std::shared_ptr<Packdata> pd = std::make_shared<Packdata>(current_path);
		packdates.push_back(pd);
	}
	objects_path = init_path / "data" / "objects";
	available = true;
}

void binary_datahash(const std::string &datahash, void *buf)
{
	auto bytebuf = (uint8_t*)buf;
	for (int i = 0; i < DATAHASH_FIELD_LENGTH; ++i) {
		bytebuf[i] = from_hex_digit(datahash[i*2]) << 4 | from_hex_digit(datahash[i*2 + 1]);
	}
}

// Диагностика: путь файла тела объекта по хешу (data/objects/aa/bbbb...).
std::string PackDirectory::describe_object(const std::string &datahash) const
{
	if (objects_path.empty()) {
		return std::string("<каталог objects не задан>");
	}
	return object_path(objects_path, datahash).string();
}

bool PackDirectory::object_exists(const std::string &datahash) const
{
	if (objects_path.empty()) {
		return false;
	}
	return boost::filesystem::exists(object_path(objects_path, datahash));
}

bool PackDirectory::hash_in_index(const std::string &datahash) const
{
	if ((int)datahash.size() < DATAHASH_FIELD_LENGTH * 2) {
		return false;
	}
	char binary_hash[DATAHASH_FIELD_LENGTH];
	binary_datahash(datahash, binary_hash);
	for (const auto &pd : packdates) {
		if (pd->has_hash(binary_hash)) {
			return true;
		}
	}
	return false;
}

size_t PackDirectory::count_objects() const
{
	if (objects_path.empty() || !boost::filesystem::exists(objects_path)) {
		return 0;
	}
	size_t n = 0;
	for (boost::filesystem::recursive_directory_iterator it(objects_path), end; it != end; ++it) {
		if (boost::filesystem::is_regular_file(it->path())) {
			++n;
		}
	}
	return n;
}

TStream* PackDirectory::get_data(const std::string &datahash, bool &found)
{
	found = false;

	if (!available) {
		return nullptr;
	}

	char binary_hash[DATAHASH_FIELD_LENGTH];
	binary_datahash(datahash, binary_hash);

	for(auto& packdata: packdates) {
		TStream *buffer = packdata->get_data(binary_hash, found);
		if(found) {
			return buffer;
		}
	}

	auto data_path = object_path(objects_path, datahash);
	if (!boost::filesystem::exists(data_path)) {
		return nullptr;
	}

	try
	{
		// Поток НЕ закрываем: TFileStream::Read() при каждом чтении вызывает
		// Reopen(), поэтому закрытый поток переоткрывал бы файл тысячи раз.
		// На Windows такие повторы упираются в отказ ("Файл не открыт") и
		// выгрузка обрывается. Поток закрывается сам в своём деструкторе.
		TStream *buffer = new TFileStream(data_path, fmOpenRead | fmShareDenyNone);
		found = true;
		return buffer;
	} catch (...) {
	}
	return nullptr;
}

