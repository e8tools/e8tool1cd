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
#include "TStream.hpp"
#include "DebugTrace.h"

#include <cstdlib>
#include <cstdio>
#include <string>

// Диагностика выхода за границы потока. Включается переменной окружения
// TOOL1CD_DEBUG_SEEK=1. Печатает, какой файл читается, куда и докуда.
static bool tool1cd_seek_debug()
{
	static const bool on = (std::getenv("TOOL1CD_DEBUG_SEEK") != nullptr);
	return on;
}

static const char* tool1cd_origin_name(const TSeekOrigin Origin)
{
	if (Origin == soBeginning) return "begin";
	if (Origin == soEnd)       return "end";
	return "current";
}

static void tool1cd_trace_dump();

static void tool1cd_report_bad_seek(const TStream* self, int64_t offset,
                                    TSeekOrigin Origin, int64_t size, int64_t pos)
{
	if (!tool1cd_seek_debug()) return;
	std::fprintf(stderr,
		"[SEEK] Wrong offset: файл=%s режим=%s offset=%lld pos=%lld size=%lld\n",
		self ? self->describe().c_str() : "?",
		tool1cd_origin_name(Origin),
		(long long)offset, (long long)pos, (long long)size);
	tool1cd_trace_dump();
}

// Сколько раз читали и писали: видно, до какого места дошла обработка.
// Кольцевой журнал операций Seek: при сбое видно, что читалось перед ним.
struct SeekTraceEntry {
	const char* what;
	long long   offset;
	long long   pos;
	long long   size;
};

static const int SEEK_TRACE_MAX = 64;
static SeekTraceEntry& tool1cd_trace_ring(int idx) { static SeekTraceEntry r[SEEK_TRACE_MAX]; return r[idx]; }
static int& tool1cd_trace_head() { static int h = 0; return h; }

static void tool1cd_trace_add(const char* what, int64_t offset, int64_t pos, int64_t size)
{
	if (!tool1cd_seek_debug()) return;
	int& h = tool1cd_trace_head();
	SeekTraceEntry& e = tool1cd_trace_ring(h % SEEK_TRACE_MAX);
	e.what = what; e.offset = (long long)offset; e.pos = (long long)pos; e.size = (long long)size;
	++h;
}

static void tool1cd_trace_dump()
{
	if (!tool1cd_seek_debug()) return;
	int h = tool1cd_trace_head();
	int n = (h < SEEK_TRACE_MAX) ? h : SEEK_TRACE_MAX;
	std::fprintf(stderr, "[SEEK] предыстория последних %d операций (старые сверху):\n", n);
	for (int i = 0; i < n; ++i) {
		const SeekTraceEntry& e = tool1cd_trace_ring((h - n + i + SEEK_TRACE_MAX * 2) % SEEK_TRACE_MAX);
		std::fprintf(stderr, "    %-8s offset=%-12lld pos=%-12lld size=%lld\n",
			e.what ? e.what : "?", e.offset, e.pos, e.size);
	}
}

static long long& tool1cd_read_counter() { static long long n = 0; return n; }
static long long& tool1cd_write_counter() { static long long n = 0; return n; }
#include "Exception.hpp"
#include <cstring>

using namespace std;

namespace System {

namespace Classes {

TStream::TStream()
	: m_position(0), m_size(0)
{
}


int64_t TStream::GetSize() const
{
	return m_size;
}

void TStream::SetSize(int64_t NewSize)
{
	// TODO: реализовать TStream::SetSize
	m_size = NewSize;
}

int64_t TStream::GetPosition() const
{
	return m_position;
}

void TStream::SetPosition(int64_t NewPosition)
{
	m_position = NewPosition;
}

int64_t TStream::Seek(const int64_t offset, const TSeekOrigin Origin)
{
	if (Origin == soFromBeginning) {

		if (offset > GetSize()) {
			DebugTrace::fail_seek(describe(), tool1cd_origin_name(Origin), offset, m_position, GetSize());
			throw Exception("Wrong offset");
		}
		m_position = offset;

	} else if (Origin == soFromEnd) {

		m_position = GetSize() - offset;

		if (GetSize() < offset) {
			DebugTrace::fail_seek(describe(), tool1cd_origin_name(Origin), offset, m_position, GetSize());
			throw Exception("Wrong offset");
		}

	} else {

		if (m_position + offset < 0 || m_position + offset > GetSize()) {
			DebugTrace::fail_seek(describe(), tool1cd_origin_name(Origin), offset, m_position, GetSize());
			throw Exception("Wrong offset");
		}
		m_position += offset;
	}
	DebugTrace::op("seek", describe(), offset, m_position, GetSize());
	return m_position;
}

int64_t TStream::ReadBuffer(void *Buffer, int64_t Count)
{
	auto data_read = Read(Buffer, Count);
	return data_read;
}

int64_t TStream::Read(std::vector<uint8_t> &Buffer, int64_t Count)
{
	auto CountToRead = (Count <= 0) ? GetSize() : Count;
	if (Buffer.size() < CountToRead) {
		Buffer.resize(CountToRead);
	}
	return Read(Buffer.data(), CountToRead);
}

int64_t TStream::CopyFrom(TStream *Source, const int64_t Count)
{
	if (Count == 0) {
		Source->Seek(0, soFromBeginning);
		auto data_size = Source->GetSize();
		if (data_size != 0) {
			return CopyFrom(Source, data_size);
		}
		return 0;
	}
	std::vector<uint8_t> _data;
	auto resultCount = Source->Read(_data, Count);
	return Write(_data, resultCount);
}

int64_t TStream::WriteBuffer(const void *Buffer, const int64_t Count)
{
	auto result = Write(Buffer, Count);
	if (result != Count) {
		// Throw EWriteError
	}
	return result;
}

int64_t TStream::Write(const std::vector<uint8_t> &Buffer, const int64_t Count)
{
	if (Count == 0) {
		auto data_size = Buffer.size();
		if (data_size != 0) {
			return Write(Buffer.data(), data_size);
		}
		return 0;
	}
	return Write(Buffer.data(), Count);
}

void TStream::Close()
{
}

void TStream::WriteString(const std::string &string)
{
	Write(string.c_str(), string.size());
}

TStream::~TStream()
{
}


THandleStream::~THandleStream()
{
}

TWrapperStream::TWrapperStream()
{
}

TWrapperStream::TWrapperStream(const std::shared_ptr<std::iostream> &stream)
	: _stream(stream)
{
	init_size();
}

TWrapperStream::TWrapperStream(std::shared_ptr<std::iostream> &&stream)
	: _stream(stream)
{
	init_size();
}

void TWrapperStream::reset(std::iostream *stream)
{
	_stream.reset(stream);
	init_size();
	m_position = 0;
}

void TWrapperStream::reopen(const std::shared_ptr<std::iostream> &stream)
{
	_stream = stream;
	init_size();
}

void TWrapperStream::init_size()
{
	_stream->seekg(0, std::ios_base::end);
	m_size = _stream->tellg();
	_stream->seekg(0, std::ios_base::beg);
	if (m_size < 0) {
		m_size = 0;
	}
}


int64_t TWrapperStream::Read(void *Buffer, int64_t Count)
{
	_stream->clear();
	_stream->seekg(GetPosition(), std::ios_base::beg);
	_stream->read((char*)Buffer, Count);
	if (_stream->bad()) {
		std::string err(std::strerror(errno));
		std::cerr << err << std::endl;
		throw Exception(err);
	}
	auto data_read = _stream->gcount();
	m_position += data_read;
	return data_read;
}

int64_t TWrapperStream::Write(const void *Buffer, int64_t Count)
{
	_stream->seekp(GetPosition(), std::ios_base::beg);
	_stream->write((char*)Buffer, Count);
	_stream->flush();
	if (_stream->bad()) {
		string err(std::strerror(errno));
		std::cerr << err << std::endl;
		throw Exception(err);
	}

	m_position += Count;
	if (m_position > m_size) {
		m_size = m_position;
	}

	return Count;
}

TWrapperStream::~TWrapperStream()
{
	_stream.reset();
}

} // Classes

} // System

