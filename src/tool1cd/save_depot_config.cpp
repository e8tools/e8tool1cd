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
#include <vector>
#include "SystemClasses/System.IOUtils.hpp"
#include <boost/filesystem.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <memory>

#include "UZLib.h"
#include "Class_1CD.h"
#include "Base64.h"
#include "Common.h"
#include "TempStream.h"
#include "ConfigStorage.h"
#include "Constants.h"
#include "CRC32.h"
#include "PackDirectory.h"
#include "TableIterator.h"
#include "SystemClasses/String.hpp"

#include <boost/uuid/detail/sha1.hpp>
#include <cstdint>
#include <fstream>
#include <map>

using namespace std;
using namespace System;

/* возвращает текст вида
{
{216,0}
}*/
string serialize_version(int configVerMajor, int configVerMinor)
{
	auto t = std::make_shared<Tree>("", node_type::nd_list, nullptr);
	auto tc = new Tree("", node_type::nd_list, t.get());
	tc = new Tree("", node_type::nd_list, tc);
	tc->add_child(to_string(configVerMajor), node_type::nd_number);
	tc->add_child(to_string(configVerMinor), node_type::nd_number);
	return outtext(t.get());
}

bool try_store_blob_data(const TableRecord &record,
						 const Field *data_field,
						 bool inflate_stream,
						 const Field *hash_field,
						 PackDirectory &pack_directory,
						 TStream *&out);


namespace {

// get_digest есть в двух видах: Boost до 1.8x принимает unsigned int[5],
// с 1.8x — unsigned char[20]. Выбираем по типу аргумента, который реально
// принимает вызов, через SFINAE-проверку выражения вызова.
template <typename Hash>
auto sha1_digest_impl(Hash &h, unsigned int (&d)[5], int)
	-> decltype(h.get_digest(d), void())
{
	h.get_digest(d);
}

template <typename Hash>
auto sha1_digest_impl(Hash &h, unsigned int (&d)[5], long)
	-> decltype(h.get_digest(*reinterpret_cast<unsigned char (*)[20]>(d)), void())
{
	unsigned char b[20];
	h.get_digest(b);
	for (int i = 0; i < 5; i++) {
		d[i] = (static_cast<unsigned int>(b[i * 4 + 0]) << 24) |
		       (static_cast<unsigned int>(b[i * 4 + 1]) << 16) |
		       (static_cast<unsigned int>(b[i * 4 + 2]) << 8) |
		        static_cast<unsigned int>(b[i * 4 + 3]);
	}
}

// Дайджест в виде 20 байт: у обеих версий Boost порядок полей big-endian.
template <typename Hash>
void sha1_digest_bytes(Hash &h, unsigned char (&out)[20])
{
	unsigned int d[5] = {0, 0, 0, 0, 0};
	sha1_digest_impl(h, d, 0);
	for (int i = 0; i < 5; i++) {
		out[i * 4 + 0] = static_cast<unsigned char>((d[i] >> 24) & 0xff);
		out[i * 4 + 1] = static_cast<unsigned char>((d[i] >> 16) & 0xff);
		out[i * 4 + 2] = static_cast<unsigned char>((d[i] >> 8) & 0xff);
		out[i * 4 + 3] = static_cast<unsigned char>(d[i] & 0xff);
	}
}

} // namespace


static string sha1_base64(TStream *body)
{
	unsigned char buffer[8192];
	boost::uuids::detail::sha1 hash;

	int64_t old_pos = body->GetPosition();
	body->Seek(0, soFromBeginning);
	while (true) {
		int64_t read = body->Read(buffer, sizeof(buffer));
		if (read <= 0) {
			break;
		}
		hash.process_bytes(buffer, static_cast<size_t>(read));
	}
	body->Seek(old_pos, soFromBeginning);

	// У Boost до 1.8x get_digest берёт unsigned int[5], начиная с 1.8x —
	// unsigned char[20]. Берём тот вид, который принимает текущая версия.
	unsigned char bytes[20] = {0};
	sha1_digest_bytes(hash, bytes);

	// SHA1 -> base64 в том же виде, что пишет платформа (28 символов с '=' на конце).
	TTempStream raw;
	raw.Write(bytes, sizeof(bytes));
	raw.Seek(0, soFromBeginning);

	// base64_encode пишет результат широкими символами (UTF-16), а в configinfo
	// суммы хранятся обычным ASCII — поэтому берём из каждого символа младший байт.
	TTempStream encoded;
	base64_encode(&raw, &encoded, 0);
	encoded.Seek(0, soFromBeginning);
	std::vector<char> wide(static_cast<size_t>(encoded.GetSize()) + 1, 0);
	encoded.Read(wide.data(), encoded.GetSize());

	string result;
	for (size_t i = 0; i + 1 < static_cast<size_t>(encoded.GetSize()); i += 2) {
		result.push_back(wide[i]);
	}
	while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
		result.pop_back();
	}
	return result;
}


// Описание одной записи файла configinfo расширения: имя и контрольная сумма
struct configinfo_entry {
	string name;
	string hash;
};


// Тело объекта целиком в память: нужно, чтобы посчитать сумму по данным.
static TTempStream *read_body(TStream *body)
{
	TTempStream *copy = new TTempStream;
	int64_t old_pos = body->GetPosition();
	body->Seek(0, soFromBeginning);
	copy->CopyFrom(body, body->GetSize());
	copy->Seek(0, soFromBeginning);
	body->Seek(old_pos, soFromBeginning);
	return copy;
}


// Идентификатор расширения лежит в самом хранилище — в служебном теле вида
// {0,2,<GUID расширения>,0,<GUID первого объекта>,0} (86 байт: BOM и текст).
// Хэша этого тела нет ни в одной таблице, поэтому через HISTORY его не достать:
// файл лежит в data/objects/<2 hex>/<остальные 38 hex> и находится перебором.
// Снимок версии нужен лишь как запасной источник, когда тела нет.
static string extension_guid_from_bodies(const boost::filesystem::path &root_path)
{
	boost::filesystem::path objects = root_path.parent_path() / "data" / "objects";
	if (!directory_exists(objects)) {
		return string();
	}

	boost::filesystem::directory_iterator sub(objects), sub_end;
	for (; sub != sub_end; sub++) {
		if (!boost::filesystem::is_directory(sub->path())) {
			continue;
		}
		boost::filesystem::directory_iterator file(sub->path()), file_end;
		for (; file != file_end; file++) {
			if (!boost::filesystem::is_regular_file(file->path())) {
				continue;
			}
			// Нас интересуют только маленькие текстовые тела
			boost::system::error_code err;
			auto size = boost::filesystem::file_size(file->path(), err);
			if (err || size < 10 || size > 512) {
				continue;
			}

			std::ifstream in(file->path().string(), std::ios::binary);
			if (!in) {
				continue;
			}
			string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
			if (text.compare(0, 3, "\xef\xbb\xbf") == 0) {
				text = text.substr(3);
			}
			if (text.compare(0, 5, "{0,2,") != 0) {
				continue;
			}
			size_t first = text.find(',', 5);
			if (first == string::npos) {
				continue;
			}
			string guid = text.substr(5, first - 5);
			if (guid.size() == 36) {
				return guid;
			}
		}
	}
	return string();
}


// Есть ли рядом с файлом .1CD снимок версии: в нём платформа хранит идентификатор
// расширения, которого нет ни в одной таблице хранилища.
static string extension_guid_from_snapshot(const boost::filesystem::path &root_path, int ver)
{
	boost::filesystem::path cache_dir = root_path.parent_path() / "cache";
	if (!directory_exists(cache_dir)) {
		return string();
	}

	// Номер версии добивается нулями слева до пяти знаков: ddb00354.snp
	string ver_part = string("00000").append(to_string(ver));
	string name_snap = string("ddb").append(ver_part.substr(ver_part.size() - 5, 5)).append(".snp");
	boost::filesystem::path file_snap = cache_dir / name_snap;

	if (!boost::filesystem::exists(file_snap)) {
		return string();
	}

	// Заголовочная запись снапшота выглядит так (текстовая сериализация):
	// {6,<GUID снимка>,<GUID хранилища>,<GUID расширения>,...}
	// Все, кроме сноски, нам не нужны, поэтому ищем только третий GUID.
	std::ifstream in(file_snap.string(), std::ios::binary);
	if (!in) {
		return string();
	}
	string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

	size_t pos = content.find("{6,");
	if (pos == string::npos) {
		return string();
	}

	// Пропускаем первые два GUID, третий — идентификатор расширения.
	size_t cur = pos + 3;
	for (int skip = 0; skip < 2; skip++) {
		cur = content.find(',', cur);
		if (cur == string::npos) {
			return string();
		}
		cur++;
	}
	size_t end = content.find(',', cur);
	if (end == string::npos) {
		return string();
	}
	return content.substr(cur, end - cur);
}


bool contains_ic(const vector<string> &vector, const string &string_to_find)
{
	for (auto &value : vector) {
		if (EqualIC(value, string_to_find)) {
			return true;
		}
	}
	return false;
}

//---------------------------------------------------------------------------
// Сохранение конфигурации в файл из хранилища конфигураций
// ver - номер версии сохраняемой конфигурации
// ver > 0 - используется переданный номер версии
// ver <= 0 - номер версии от последней конфигурации. 0 - последняя конфигурация, -1 - предпоследняя и т.д., т.е. Номер версии определяется как номер последней + ver
bool T_1CD::save_depot_config(const string &_filename, int32_t ver, bool use_snapshot)
{
	Field* fldd_rootobjid;

	Field* fldh_datahash;
	BinaryGuid rootobj;

	Field* flde_datahash;
	vector<TableRecord*> reces;
	vector<string> extnames;

	depot_ver depotVer;
	uint32_t configVerMajor, configVerMinor;
	PackDirectory pack_directory;
	V8Catalog* cat;
	V8Catalog* cath;
	bool oldformat;
	// Хранилище расширения: контейнер собирается иначе, корень — файл configinfo
	// со списком объектов и контрольных сумм, а не root/version/versions.
	const bool extension = extension_depot();
	Tree* tv; // корень дерева файла versions
	Tree* tvc; // тек. элемент дерева файла versions
	Tree* tr; // корень дерева файла root
	Tree* trc; // тек. элемент дерева файла root
	Tree* tcountv; // узел, содержащий счетчик в файле versions
	Tree* tcountr; // узел, содержащий счетчик в файле root

	boost::uuids::random_generator uuid_gen;

	union
	{
		char b[2];
		unsigned short s;
	} cv;

	try {

		assert_i_am_a_repository();

	} catch (DetailedException &exc) {
		exc.show();
		return false;
	}

	// Получаем версию хранилища
	fldd_rootobjid = table_depot->get_field("ROOTOBJID");

	TableIterator depot_iterator(table_depot);
	if (depot_iterator.eof()) {
		throw DetailedException("Не удалось прочитать запись в таблице DEPOT.");
	}

	depotVer = get_depot_version(depot_iterator.current());
	rootobj = depot_iterator.current().get<BinaryGuid>(fldd_rootobjid);

	// "Нормализуем" версию конфигурации
	ver = get_ver_depot_config(ver);

	// Ищем строку с номером версии конфигурации

	Field *fldv_vernum = table_versions->get_field("VERNUM");

	TableIterator versions_iterator(table_versions);
	while (!versions_iterator.eof()) {
		int32_t vernum = ToIntDef(versions_iterator.current().get_string(fldv_vernum), 0);
		if (vernum == ver) {
			break;
		}
		versions_iterator.next();
	}

	if (versions_iterator.eof()) {
		throw DetailedException("В хранилище не найдена версия конфигурации")
				.add_detail("Требуемая версия", ver);
	}

	boost::filesystem::path filepath = boost::filesystem::path(_filename);
	boost::filesystem::path root_path(filename); // путь к 1cd


	if (use_snapshot && try_save_snapshot(versions_iterator.current(), ver, rootobj, root_path, filepath)) {
		return true;
	}

	auto vrec = versions_iterator.current();

	// Определяем версию структуры конфигурации (для файла version)
	if(depotVer >= depot_ver::Ver5)
	{
		const char *frec = vrec.get_raw("CVERSION");
		cv.b[0] = frec[1];
		cv.b[1] = frec[0];
		configVerMajor = cv.s;
		frec += 2;
		cv.b[0] = frec[1];
		cv.b[1] = frec[0];
		configVerMinor = cv.s;
	}
	else
	{
		configVerMinor = 0;
		if(version == db_ver::ver8_0_3_0 || version == db_ver::ver8_0_5_0) configVerMajor = 6;
		else if(version == db_ver::ver8_1_0_0) configVerMajor = 106;
		else configVerMajor = 216;
	}

	// Инициализируем таблицы HISTORY и EXTERNALS

	Field *fldh_objid      = table_history->get_field("OBJID");
	Field *fldh_vernum     = table_history->get_field("VERNUM");
	Field *fldh_objverid   = table_history->get_field("OBJVERID");
	Field *fldh_removed    = table_history->get_field("REMOVED");
	Field *fldh_datapacked = table_history->get_field("DATAPACKED");
	Field *fldh_objdata    = table_history->get_field("OBJDATA");

	Field *flde_objid      = table_externals->get_field("OBJID");
	Field *flde_vernum     = table_externals->get_field("VERNUM");
	Field *flde_extname    = table_externals->get_field("EXTNAME");
	Field *flde_extverid   = table_externals->get_field("EXTVERID");
	Field *flde_datapacked = table_externals->get_field("DATAPACKED");
	Field *flde_extdata    = table_externals->get_field("EXTDATA");

	if (depotVer >= depot_ver::Ver6)
	{
		fldh_datahash = table_history->get_field("DATAHASH");
		flde_datahash = table_externals->get_field("DATAHASH");

		boost::filesystem::path root_dir = root_path.parent_path();
		pack_directory.init(root_dir);
		if (!pack_directory.is_available()) {
		}
	}
	else
	{
		fldh_datahash = nullptr;
		flde_datahash = nullptr;
	}

	// Тела объектов хранилищ формата 6+ лежат в каталоге data рядом с .1CD.
	// Если его нет, объект взять негде — сообщаем об этом прямо.
	auto object_read_error = [&pack_directory](const char *table, const string &object, const string &ver) {
		DetailedException ex("Ошибка чтения объекта конфигурации");
		ex.add_detail("Таблица", table)
				.add_detail("Объект", object)
				.add_detail("Версия", ver);
		if (!pack_directory.is_available()) {
			ex.add_detail("Причина", "нет каталога данных хранилища (data/pack, data/objects)")
					.add_detail("Что делать", "выгружать из полной копии хранилища: файл .1CD вместе с каталогом data");
		}
		return ex;
	};

	Index *index_history = table_history->get_index("PK");

	TableRecord *rech1 = nullptr;
	TableRecord *rech2 = nullptr;
	reces.resize(0);

	auto HistoryIndex_numrec = index_history->get_numrecords();
	IndexedTableIterator externals_iterator(table_externals, "PK");

	if (boost::filesystem::exists(filepath)) {
		boost::filesystem::remove(filepath);
	}
	cat = new V8Catalog(filepath.string(), false);

	// root, versions

	std::map<string,string> vermap; // контейнер для сортировки versions
	std::map<string,string> rootmap; // контейнер для сортировки root
	std::map<string,TStream*> extmap; // контейнер для сортировки файлов в корне
	std::map<string,TStream*> metamap; // контейнер для сортировки файлов в metadata
	// Тела объектов для configinfo расширения: имя -> данные (до выгрузки в контейнер)
	std::map<string,TStream*> bodymap;
	// Тела версий объектов: «объект.номер версии» -> данные
	std::map<string,TTempStream*> version_bodies;

	tv = new Tree("",  node_type::nd_list, nullptr); // корень дерева файла versions
	tvc = new Tree("", node_type::nd_list, tv); // тек. элемент дерева файла versions
	tr = new Tree("",  node_type::nd_list, nullptr); // корень дерева файла root
	trc = new Tree("", node_type::nd_list, tr); // тек. элемент дерева файла root

	tvc->add_child("1", node_type::nd_number);
	tcountv = tvc->add_child("0", node_type::nd_number); // узел, содержащий счетчик в файле versions

	vermap[""] = GUIDasMS(uuid_gen().data);

	if (!extension) {
		TStream *in = new TMemoryStream;
		in->Write(TEncoding::UTF8->GetPreamble(), TEncoding::UTF8->GetPreamble().size());
		in->WriteString(serialize_version(configVerMajor, configVerMinor));
		TStream *out = new TMemoryStream;
		in->Seek(0, soFromBeginning);
		ZDeflateStream(in, out);
		delete in;
		extmap["version"] = out;
	}
	if (!extension) {
		vermap["version"] = GUIDasMS(uuid_gen().data);
	}

	if(configVerMajor < 100)
	{
		trc->add_child("1", node_type::nd_number);
		trc->add_child(rootobj.as_MS(), node_type::nd_guid);
		tcountr = trc->add_child("0", node_type::nd_number); // узел, содержащий счетчик в файле root
		oldformat = true;
	}
	else
	{
		trc->add_child("2", node_type::nd_number);
		trc->add_child(rootobj.as_MS(), node_type::nd_guid);
		tcountr = nullptr;
		oldformat = false;
	}

	int lastver = -1;
	bool lastremoved = true;

	BinaryGuid curobj;
	for (uint32_t iHistory_Index = 0; iHistory_Index <= HistoryIndex_numrec; iHistory_Index++)
	{
		if(iHistory_Index < HistoryIndex_numrec)
		{
			uint32_t num_rec = index_history->get_numrec(iHistory_Index);
			rech2 = table_history->get_record(num_rec);
		}

		if (rech2->get<BinaryGuid>(fldh_objid) != curobj || iHistory_Index == HistoryIndex_numrec) {
			// это новый объект или конец таблицы
			if(!lastremoved)
			{
				TStream *out;

				string sObjId = rech1->get<BinaryGuid>(fldh_objid).as_MS();
				if (!try_store_blob_data(*rech1, fldh_objdata, oldformat, fldh_datahash, pack_directory, out)) {
					throw object_read_error("HISTORY", sObjId, rech1->get_string(fldh_vernum));
				}
				{
					if(oldformat)
					{
						rootmap[sObjId] = rech1->get<BinaryGuid>(fldh_objverid).as_MS();
						metamap[sObjId] = out;
					}
					else
					{
						vermap[sObjId] = rech1->get<BinaryGuid>(fldh_objverid).as_MS();
						// Для расширения тела нужны целиком — по ним считаются суммы configinfo.
						// Для обычной конфигурации они уходят в контейнер как есть.
						if (extension) {
							bodymap[sObjId] = read_body(out);
							// Та же запись HISTORY хранит и тело конкретной версии объекта.
							string vkey = sObjId + "." + rech1->get_string(fldh_vernum);
							version_bodies[vkey] = read_body(out);
						}
						extmap[sObjId] = out;
					}

					// Вот тут идем по EXTERNALS
					while (!externals_iterator.eof()) {
						const auto &rece = externals_iterator.current();
						BinaryGuid current_external_guid = rece.get<BinaryGuid>(flde_objid);
						if (current_external_guid > curobj) {
							break;
						}

						if (current_external_guid == curobj) {
							int32_t vernum = ToIntDef(rece.get_string(flde_vernum), numeric_limits<int32_t>::max());
							string ext_name = rece.get_string(flde_extname);
							if (vernum <= ver && rece.get<bool>(flde_datapacked)) {
								size_t j;
								bool found = false;
								for (j = 0; j < reces.size(); j++) {
									if (EqualIC(ext_name, reces[j]->get_string(flde_extname))) {
										reces[j] = new TableRecord(rece);
										found = true;
										break;
									}
								}
								if (!found) {
									reces.push_back(new TableRecord(rece));
								}
							}
							if (vernum == lastver) {
								extnames.push_back(ext_name);
							}
						}
						externals_iterator.next();
					}
					for (auto rec : reces) {
						string ext_name = rec->get_string(flde_extname);
						if (!contains_ic(extnames, ext_name)) {
							continue;
						}

						TStream *out;
						if (!try_store_blob_data(*rec, flde_extdata, false, flde_datahash, pack_directory, out)) {
							throw object_read_error("EXTERNALS", ext_name, rec->get_string(flde_vernum));
						}
						vermap[ext_name] = rec->get<BinaryGuid>(flde_extverid).as_MS();
						extmap[ext_name] = out;

					}
					reces.clear();
					extnames.clear();
				}
			}

			curobj = rech2->get<BinaryGuid>(fldh_objid);
			lastremoved = true;
		}

		if(iHistory_Index < HistoryIndex_numrec)
		{
			int32_t vernum = ToIntDef(rech2->get_string(fldh_vernum), numeric_limits<int32_t>::max());
			if(vernum <= ver)
			{
				if (rech2->get<bool>(fldh_removed)) {
					lastremoved = true;
				}
				else
				{
					bool datapacked = false;
					if (!rech2->is_null_value(fldh_datapacked)) {
						if (rech2->get<bool>(fldh_datapacked)) {
							datapacked = true;
						}
					}
					if(datapacked)
					{
						rech1 = rech2;
						lastremoved = false;
						lastver = vernum;
					}
				}
			}
		}
	}

	// TODO: Полечить дичайшие утечки памяти
	for(size_t j = 0; j < reces.size(); j++) {
		delete reces[j];
	}


	// Завершаем формирование списков версий

	if (extension) {
		// В configinfo каждая версия объекта — отдельная строка «объект.номер»
		// со своей суммой, а не только последняя версия. Собираем список версий
		// по HISTORY: для каждого объекта нумеруем его версии по порядку (0,1,2...).
		std::map<string, vector<string>> object_versions; // объект -> список версий
		{
			Index *v_index = table_history->get_index("PK");
			auto v_count = v_index->get_numrecords();
			for (uint32_t vi = 0; vi < v_count; vi++) {
				TableRecord *vrec = table_history->get_record(v_index->get_numrec(vi));
				int32_t vn = ToIntDef(vrec->get_string(fldh_vernum), numeric_limits<int32_t>::max());
				if (vn > ver) {
					continue;
				}
				bool removed = vrec->is_null_value(fldh_removed) ? false : vrec->get<bool>(fldh_removed);
				if (removed) {
					continue;
				}
				string obj_id = vrec->get<BinaryGuid>(fldh_objid).as_MS();
				bool packed = !vrec->is_null_value(fldh_datapacked) && vrec->get<bool>(fldh_datapacked);
				if (!packed) {
					continue;
				}
				object_versions[obj_id].push_back(vrec->get_string(fldh_vernum));
			}
		}


		// У платформы в configinfo на объект приходится запись без версии плюс
		// строки его версий. Единственная версия уже описана записью объекта, так
		// что отдельная строка «GUID.N» появляется только при нескольких версиях.
		// N — порядковый индекс по возрастанию номера версии в HISTORY.
		std::map<string, string> objects;
		for (auto &item : object_versions) {
			const string &obj_id = item.first;
			vector<string> vers = item.second;

			// Сумма по актуальному телу объекта (ключ без версии)
			auto base = bodymap.find(obj_id);
			if (base != bodymap.end() && base->second != nullptr) {
				objects[obj_id] = sha1_base64(base->second);
			}

			// ПРОБА: строки версий не пишем вообще
			if (true) {
				continue;
			}

			sort(vers.begin(), vers.end(), [](const string &l, const string &r) {
				return atoi(l.c_str()) < atoi(r.c_str());
			});

			int index = 0;
			for (const string &vn : vers) {
				string key_src = obj_id + "." + vn;
				auto body_it = version_bodies.find(key_src);
				if (body_it == version_bodies.end() || body_it->second == nullptr) {
					index++;
					continue;
				}
				objects[obj_id + "." + to_string(index)] = sha1_base64(body_it->second);
				index++;
			}
		}


		// Список объектов берём из rootmap (для старого формата) или vermap:
		// в обоих случаях это пары «имя объекта / хэш версии», а контрольные суммы
		// считаются по уже выгруженным телам.
		TTempStream info;
		// Идентификатор расширения берём из снимка версии: в его заголовочной
		// записи {6,<GUID снимка>,<GUID хранилища>,<GUID расширения>,...} третье
		// поле — это и есть GUID расширения. В таблицах .1CD он не хранится,
		// а поиск по телам даёт чужой GUID (идентификатор объекта, не расширения).
		string guid = extension_guid_from_snapshot(root_path, ver);
		if (guid.empty()) {
			// Резерв: снимка может не быть — хранилище без каталога cache.
			guid = extension_guid_from_bodies(root_path);
		}
		if (guid.empty()) {
			throw DetailedException("Не найден идентификатор расширения")
				.add_detail("Что искали", "GUID расширения в снимке версии или в телах хранилища")
				.add_detail("Где искали", (root_path.parent_path() / "cache").string() + "/ddb**" + to_string(ver) + ".snp и data/objects")
				.add_detail("Причина", "в таблицах хранилища идентификатор расширения не хранится")
				.add_detail("Что делать", "выгружать из полной копии хранилища вместе с каталогом cache");
		}

		// Шапка configinfo для расширения: {0,{216,0,{1,0}}},{2,<GUID>,},
		// Версия формата 80327 — как в COMPATIBILITYMODE хранилища расширений.
		//
		// Поле после GUID остаётся ПУСТЫМ: платформа проверяет его как подпись
		// конфигурации и на любом непустом значении отвергает файл (в том числе
		// на корректном по размеру). Пустое поле принимается, расширение
		// открывается и дерево метаданных разбирается.
		string text;
		text.push_back('\xef'); text.push_back('\xbb'); text.push_back('\xbf');
		text.append("{0,\n{216,0,\n{1,0}\n}\n},\n{2,");
		text.append(guid);
		text.append(",},\n{");

		// Счётчик — количество записей, которые идут следом
		text.append(to_string(objects.size()));
		text.append(",");
		// Текст файла: пары «имя,сумма» через запятую.
		for (auto &item : objects) {
			text.append("\"").append(item.first).append("\",");
			text.append(item.second);
			text.append(",");
		}
		if (!objects.empty()) {
			text.pop_back();
		}
		text.append("}\n}\n");

		for (auto &item : bodymap) {
			delete item.second;
		}
		bodymap.clear();
		for (auto &item : version_bodies) {
			delete item.second;
		}
		version_bodies.clear();

		// Текст пишем байтами UTF-8: WriteString кодирует в UTF-16, и платформа
		// такое содержимое не разбирает.
		{
			// отладка: сохранить несжатый текст рядом, чтобы видеть байты
			std::ofstream dbg("/tmp/configinfo_debug.txt", std::ios::binary);
			dbg.write(text.data(), static_cast<std::streamsize>(text.size()));
		}
		TTempStream *in = new TTempStream;
		in->Write(text.data(), static_cast<int64_t>(text.size()));
		in->Seek(0, soFromBeginning);
		TTempStream *out = new TTempStream;
		ZDeflateStream(in, out);
		delete in;
		extmap["configinfo"] = out;

		for (auto& psmap : extmap) {
			cat->createFile(psmap.first)->write_and_close(psmap.second);
			delete psmap.second;
		}
		delete cat;
		return true;
	}

	vermap["versions"] = GUIDasMS(uuid_gen().data);

	if(oldformat)
	{
		tcountv->set_value(to_string(vermap.size()), node_type::nd_number);
		tcountr->set_value(to_string(rootmap.size()), node_type::nd_number);
	}
	else
	{
		vermap["root"] = GUIDasMS(uuid_gen().data);
		tcountv->set_value(to_string(vermap.size()), node_type::nd_number);
	}

	// Запись root
	for( auto& pmap: rootmap )
	{
		trc->add_child(pmap.first, node_type::nd_string);
		trc->add_child(pmap.second, node_type::nd_guid);
	}
	string tree_text = outtext(tr);
	delete tr;
	{
		TStream *in = new TMemoryStream;
		in->Write(TEncoding::UTF8->GetPreamble(), TEncoding::UTF8->GetPreamble().size());
		in->WriteString(tree_text);
		in->Seek(0, soFromBeginning);
		TStream *out = new TTempStream;
		if (oldformat) {
			out->CopyFrom(in, 0);
			metamap["root"] = out;
		} else {
			ZDeflateStream(in, out);
			extmap["root"] = out;
		}
		delete in;
	}
	// Запись versions

	for( auto& pmap: vermap )
	{
		tvc->add_child(pmap.first, node_type::nd_string);
		tvc->add_child(pmap.second, node_type::nd_guid);
	}

	string tv_text = outtext(tv);
	delete tv;
	{
		TStream *in = new TMemoryStream;
		in->Write(TEncoding::UTF8->GetPreamble(), TEncoding::UTF8->GetPreamble().size());
		{
			TStreamWriter sw(in, TEncoding::UTF8, 1024);
			sw.Write(tv_text);
		}
		TStream *out = new TTempStream;
		in->Seek(0, soFromBeginning);
		ZDeflateStream(in, out);
		extmap["versions"] = out;
	}

	if(oldformat)
	{
		cath = cat->CreateCatalog("metadata", true);
		for( auto& psmap: metamap )
		{
			cath->createFile(psmap.first)->write_and_close(psmap.second);
			delete psmap.second;
		}
	}
	for( auto& psmap: extmap )
	{
		cat->createFile(psmap.first)->write_and_close(psmap.second);
		delete psmap.second;
	}

	delete cat;
	return true;
}


// Проверка, что поток содержит файл конфигурации (контейнер 1С), а не
// служебные данные хранилища
static bool is_v8_catalog(TStream *stream)
{
	try {
		stream->Seek(0, soFromBeginning);
		V8Catalog catalog(stream, true, true);
		return catalog.is_catalog();
	}
	catch (...) {
		return false;
	}
}



bool T_1CD::try_save_snapshot(const TableRecord &version_record,
							  int ver,
							  const BinaryGuid &rootobj,
							  const boost::filesystem::path &root_path,
							  const boost::filesystem::path &target_file_path) const
{
	if (version_record.get<BinaryGuid>("SNAPSHOTMAKER").is_empty()) {
		return false;
	}
	string ver_part  = string("00000").append(to_string(ver));
	string name_snap = string("ddb").append(ver_part.substr(ver_part.size() - 5, 5)).append(".snp");

	boost::filesystem::path file_snap = root_path.parent_path() / "cache" / name_snap;


	if (!boost::filesystem::exists(file_snap)){
		return false;
	}

	std::shared_ptr<TStream> in;
	try {
		in.reset(new TFileStream(file_snap, fmOpenRead | fmShareDenyNone));
	}
	catch (...) {
		return false;
	}

	snapshot_version snap_ver = snapshot_version::Ver1;
	BinaryGuid snapshot_maker = version_record.get<BinaryGuid>("SNAPSHOTMAKER");

	if (rootobj == snapshot_maker || snapshot_maker == SNAPSHOT_VER1) {
		snap_ver = snapshot_version::Ver1;
	}
	else if (snapshot_maker == SNAPSHOT_VER2) {
		snap_ver = snapshot_version::Ver2;
	};

	// Содержимое снэпшота разбираем в память: целевой файл создаётся только
	// после проверок, чтобы в каталоге выгрузки не осталось испорченного файла
	TTempStream content;

	switch (snap_ver) {
		case snapshot_version::Ver1:
		{
			try {
				ZInflateStream(in.get(), &content);
			}
			catch(...) {
				return false;
			}
			break;
		}
		case snapshot_version::Ver2:
		{
			content.CopyFrom(in.get(), 0);
			break;
		}
		default:
		{
			return false;
		}
	};

	uint32_t snapshot_crc = version_record.get<uint32_t>("SNAPSHOTCRC");
	uint32_t calc_crc = _crc32(&content);
	if (calc_crc != snapshot_crc) {
		return false;
	}

	// Снэпшот может содержать не файл конфигурации, а служебную сериализацию
	// версии хранилища (так устроены, в частности, хранилища расширений).
	// Такой файл нельзя отдавать как результат выгрузки: конфигурацию
	// собираем по таблицам хранилища.
	if (!is_v8_catalog(&content)) {
		return false;
	}

	std::shared_ptr<TStream> out;
	try
	{
		out.reset(new TFileStream(target_file_path, fmCreate | fmShareDenyWrite));
	}
	catch(...) {
		throw DetailedException("Не удалось создать файл конфигурации")
				.add_detail("Имя файла", target_file_path.string());
	}

	content.Seek(0, soFromBeginning);
	out->CopyFrom(&content, 0);

	return true;
}


bool try_store_blob_data(const TableRecord &record,
						 const Field *data_field,
						 bool inflate_stream,
						 const Field *hash_field,
						 PackDirectory &pack_directory,
						 TStream *&out)
{
	if (record.try_store_blob_data(data_field, out, inflate_stream)) {
		return true;
	}

	if (!hash_field) {
		return false;
	}

	bool found = false;
	out = pack_directory.get_data(record.get_string(hash_field), found);
	return found;
}
