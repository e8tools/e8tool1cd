#include "DebugTrace.h"

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <vector>
#include <ctime>

#ifdef __linux__
#include <execinfo.h>
#include <unistd.h>
#endif

namespace DebugTrace {

static std::FILE* journal()
{
	static std::FILE* f = nullptr;
	static bool tried = false;
	if (!tried) {
		tried = true;
		const char* path = std::getenv("TOOL1CD_DEBUG_LOG");
		if (path && *path) {
			f = std::fopen(path, "w");
		}
	}
	return f;
}

bool enabled()
{
	static const bool on = (std::getenv("TOOL1CD_DEBUG_SEEK") != nullptr);
	return on;
}

void log(const std::string &line)
{
	if (!enabled()) return;
	std::FILE* f = journal();
	if (f) {
		std::fprintf(f, "%s\n", line.c_str());
		std::fflush(f);
	} else {
		std::fprintf(stderr, "%s\n", line.c_str());
	}
}

void file_open(const std::string &path, const std::string &mode, bool ok, int64_t size)
{
	if (!enabled()) return;
	char buf[512];
	std::snprintf(buf, sizeof(buf), "[OPEN ] %-60s режим=%-8s ok=%d размер=%lld",
		path.c_str(), mode.c_str(), (int)ok, (long long)size);
	log(buf);
}

void file_close(const std::string &path)
{
	if (!enabled()) return;
	log("[CLOSE] " + path);
}

// --- кольцевая предыстория операций ---
struct OpEntry { std::string what; std::string file; int64_t offset, pos, size; };
static const int OP_MAX = 128;
static OpEntry ops[OP_MAX];

static int& op_head() { static int h = 0; return h; }

void op(const char *what, const std::string &file, int64_t offset, int64_t pos, int64_t size)
{
	if (!enabled()) return;
	int& h = op_head();
	OpEntry& e = ops[h % OP_MAX];
	e.what = what ? what : "?";
	e.file = file;
	e.offset = offset; e.pos = pos; e.size = size;
	++h;
}

static void dump_history()
{
	int h = op_head();
	int n = (h < OP_MAX) ? h : OP_MAX;
	log("  --- предыстория последних операций (старые сверху) ---");
	for (int i = 0; i < n; ++i) {
		const OpEntry& e = ops[(h - n + i + OP_MAX * 2) % OP_MAX];
		char buf[512];
		std::snprintf(buf, sizeof(buf), "      %-7s %-50s offset=%-12lld pos=%-12lld size=%lld",
			e.what.c_str(), e.file.c_str(),
			(long long)e.offset, (long long)e.pos, (long long)e.size);
		log(buf);
	}
}

void fail_seek(const std::string &file, const char *origin,
               int64_t offset, int64_t pos, int64_t size)
{
	if (!enabled()) return;
	char buf[512];
	std::snprintf(buf, sizeof(buf),
		"[FAIL ] Wrong offset: файл=%s режим=%s offset=%lld pos=%lld size=%lld",
		file.c_str(), origin ? origin : "?", (long long)offset,
		(long long)pos, (long long)size);
	log(buf);
	dump_history();
	backtrace_now("Wrong offset");
}

void block_read(int64_t block_num, int64_t offset, int64_t page_size, int64_t file_size)
{
	if (!enabled()) return;
	char buf[512];
	std::snprintf(buf, sizeof(buf),
		"[BLOCK] блок=%lld смещение=%lld страница=%lld размер_файла=%lld%s",
		(long long)block_num, (long long)offset, (long long)page_size, (long long)file_size,
		(offset + page_size > file_size) ? "  <-- ЗА КОНЦОМ ФАЙЛА" : "");
	log(buf);
}

void block_signature(int64_t block_num, const unsigned char *sig, int len)
{
	if (!enabled()) return;
	char hex[128] = {0};
	int n = (len > 16) ? 16 : len;
	for (int i = 0; i < n; ++i) {
		std::snprintf(hex + i*3, 4, "%02x ", sig[i]);
	}
	char buf[256];
	std::snprintf(buf, sizeof(buf), "[SIG  ] блок=%lld первые байты: %s",
		(long long)block_num, hex);
	log(buf);
}

void backtrace_now(const char *why)
{
	if (!enabled()) return;
#ifdef __linux__
	void* frames[32];
	int n = ::backtrace(frames, 32);
	char** syms = ::backtrace_symbols(frames, n);
	char buf[512];
	std::snprintf(buf, sizeof(buf), "  --- стек вызовов (%d кадров), причина: %s ---", n, why ? why : "?");
	log(buf);
	for (int i = 0; i < n; ++i) {
		log("      " + std::string(syms ? syms[i] : "??"));
	}
	if (syms) std::free(syms);
#else
	char buf[256];
	std::snprintf(buf, sizeof(buf), "  --- стек вызовов недоступен на этой платформе (%s) ---", why ? why : "?");
	log(buf);
#endif
}

} // namespace DebugTrace
