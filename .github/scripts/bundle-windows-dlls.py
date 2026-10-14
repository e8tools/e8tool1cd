#!/usr/bin/env python3
"""Собирает в каталог выпуска все несистемные DLL, нужные v8unpack.exe.

Обход рекурсивный: берутся не только прямые зависимости exe, но и зависимости
самих DLL. Это важно, потому что Qt6Core.dll из MSYS2 тянет за собой ICU,
zstd, pcre2, b2 и double-conversion. Пока список DLL перечислялся руками,
часть из них не попадала в архив, и на чистой Windows появлялось
"система не обнаружила libicuuc78.dll".

DLL считается «нашей», если файл с таким именем есть в каталоге MSYS2 bin
(источник поставки). Всё остальное — системное (Windows 10+ отдаёт api-ms-win-*
и классические системные библиотеки сам). Если чего-то не нашлось ни в bin,
ни в списке системных — это ошибка упаковки, и скрипт завершается с кодом 1,
чтобы неполный архив не уехал в релиз.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys

# Библиотеки, которые Windows предоставляет сама. Список намеренно широкий:
# ошибка в сторону системной библиотеки видна сразу, а пропущенная своя —
# выстреливает у пользователя.
SYSTEM_DLLS = {
    "advapi32.dll", "authz.dll", "bcrypt.dll", "comdlg32.dll", "crypt32.dll",
    "dbghelp.dll", "dnsapi.dll", "dwmapi.dll", "dxgi.dll", "gdi32.dll",
    "iphlpapi.dll", "kernel32.dll", "kernelbase.dll", "mpr.dll", "msvcrt.dll",
    "netapi32.dll", "ntdll.dll", "ole32.dll", "oleaut32.dll", "opengl32.dll",
    "powrprof.dll", "psapi.dll", "rpcrt4.dll", "secur32.dll", "setupapi.dll",
    "shell32.dll", "shlwapi.dll", "ucrtbase.dll", "user32.dll", "userenv.dll",
    "usp10.dll", "uxtheme.dll", "version.dll", "winmm.dll", "winspool.drv",
    "ws2_32.dll", "wtsapi32.dll",
}

SYSTEM_PREFIXES = ("api-ms-win-", "ext-ms-win-")


def is_system(name):
    low = name.lower()
    return low in SYSTEM_DLLS or low.startswith(SYSTEM_PREFIXES)


def imported_dlls(path):
    """Имена DLL из таблицы импорта PE-файла."""
    out = subprocess.run(["objdump", "-p", path], capture_output=True, text=True).stdout
    return re.findall(r"DLL Name:\s*(\S+)", out)


def main():
    # Консоль Windows по умолчанию пишет в cp1252, и русский текст в выводе
    # роняет скрипт с UnicodeEncodeError. Явно переводим потоки в UTF-8.
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass

    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True, help="главный исполняемый файл")
    ap.add_argument("--prefix", required=True, help="каталог bin MSYS2 — источник DLL")
    ap.add_argument("--dest", required=True, help="каталог выпуска")
    a = ap.parse_args()
    os.makedirs(a.dest, exist_ok=True)

    index = {}
    for f in os.listdir(a.prefix):
        if f.lower().endswith(".dll"):
            index[f.lower()] = os.path.join(a.prefix, f)

    copied, queue, seen, unresolved = [], [a.exe], set(), []
    while queue:
        cur = queue.pop()
        for name in imported_dlls(cur):
            low = name.lower()
            if low in seen:
                continue
            seen.add(low)
            src = index.get(low)
            if src:
                dst = os.path.join(a.dest, os.path.basename(src))
                shutil.copy2(src, dst)
                copied.append(os.path.basename(src))
                queue.append(dst)
            elif not is_system(name):
                unresolved.append(name)

    print("=== собрано несистемных библиотек: %d ===" % len(copied))
    for n in sorted(set(copied)):
        print("  +", n)
    print("=== системных (в поставку не идут): %d ===" % len(seen - {c.lower() for c in copied}))

    if unresolved:
        print("ОШИБКА: не найдены ни в %s, ни среди системных:" % a.prefix)
        for n in sorted(set(unresolved)):
            print("  ?", n)
        return 1
    print("замыкание зависимостей полное")
    return 0


if __name__ == "__main__":
    sys.exit(main())
