#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Сбор несистемных разделяемых библиотек рядом с бинарником (Linux, tool1cd).

Замыкание обходится рекурсивно по `ldd`: начиная с самого файла, каждая
несистемная библиотека копируется в <dest> и опрашивается снова. Библиотеки
glibc и динамический линкер не копируются — они есть в любой системе.

Режимы:
    --exe <файл> --dest <каталог>            собрать библиотеки в каталог
    --patch --exe <файл> --dest <каталог>    прописать rpath: $ORIGIN у каждой
                                            библиотеки, у бинарника — <--rpath>
    --check --dir <каталог>                 проверить готовый каталог

Важно про rpath (иначе архив не работает на машине без Qt6):
    * RUNPATH (DT_RUNPATH, его ставит patchelf по умолчанию) НЕ наследуется
      зависимостями: бинарник находит libQt6Core.so.6 в lib/, а та уже не ищет
      там libicuuc.so.72 — получаем «cannot open shared object file».
    * Поэтому линковка — RPATH (DT_RPATH), ключ --force-rpath: RPATH
      наследуется по всей цепочке зависимостей.
    * И дополнительно каждая скопированная библиотека получает свой $ORIGIN:
      тогда каждая сама находит соседей.
    * Проверка --check идёт БЕЗ LD_LIBRARY_PATH — так, как будет на машине
      пользователя; с LD_LIBRARY_PATH неполный архив проходил бы проверку.
"""
import argparse
import os
import shutil
import subprocess
import sys

# Библиотеки, которые присутствуют в любой системе: glibc и линкер.
SYSTEM = {
    "linux-vdso.so.1",
    "linux-gate.so.1",
    "ld-linux.so.2",
    "ld-linux-x86-64.so.2",
    "libc.so.6",
    "libm.so.6",
    "libpthread.so.0",
    "libdl.so.2",
    "librt.so.1",
    "libresolv.so.2",
    "libutil.so.1",
    "libnsl.so.1",
    "libanl.so.1",
}

DEFAULT_RPATH = "$ORIGIN/lib:$ORIGIN/../lib/tool1cd"


def is_elf(path):
    try:
        with open(path, "rb") as f:
            return f.read(4) == b"\x7fELF"
    except OSError:
        return False


def ldd_deps(path):
    """Список (имя, путь|None) — прямые зависимости файла по данным ldd."""
    try:
        proc = subprocess.run(["ldd", path], capture_output=True, text=True)
    except OSError:
        return []
    if proc.returncode != 0:
        return []

    deps = []
    for raw in proc.stdout.splitlines():
        line = raw.strip()
        if not line:
            continue
        # «statically linked», «not a dynamic executable» — зависимостей нет.
        if "=>" not in line and "/" not in line:
            continue
        if "=>" in line:
            name, rest = line.split("=>", 1)
            name, rest = name.strip(), rest.strip()
            if rest.startswith("not found"):
                deps.append((name, None))
            else:
                deps.append((name, rest.split(" (")[0].strip()))
        else:
            # Строка вида "/lib64/ld-linux-x86-64.so.2 (0x...)" — линкер.
            name = line.split(" (")[0].strip()
            deps.append((os.path.basename(name), name))
    return deps


def is_system(name):
    return os.path.basename(name) in SYSTEM


def collect(exe, dest):
    """Рекурсивно скопировать несистемные зависимости в dest."""
    if not is_elf(exe):
        print("bundle-linux-libs: %s не ELF-файл" % exe, file=sys.stderr)
        return [], []

    os.makedirs(dest, exist_ok=True)
    queue = [os.path.abspath(exe)]
    seen = set()
    copied = []
    missing = []

    while queue:
        current = queue.pop()
        key = os.path.basename(current)
        if key in seen:
            continue
        seen.add(key)

        for name, path in ldd_deps(current):
            if is_system(name):
                continue
            if path is None:
                missing.append((os.path.basename(current), name))
                continue
            target = os.path.join(dest, os.path.basename(path))
            if not os.path.exists(target):
                shutil.copy2(os.path.realpath(path), target)
                copied.append(os.path.basename(target))
            queue.append(target)

    return sorted(copied), missing


def patchelf(*args):
    proc = subprocess.run(["patchelf"] + list(args), capture_output=True, text=True)
    if proc.returncode != 0:
        print("bundle-linux-libs: patchelf %s: %s" % (" ".join(args), proc.stderr.strip()),
              file=sys.stderr)
        return False
    return True


def patch(exe, dest, rpath):
    """Прописать rpath: у бинарника — rpath, у каждой библиотеки — $ORIGIN."""
    ok = patchelf("--force-rpath", "--set-rpath", rpath, exe)
    libs = 0
    for name in sorted(os.listdir(dest)):
        path = os.path.join(dest, name)
        if not is_elf(path):
            continue
        ok = patchelf("--force-rpath", "--set-rpath", "$ORIGIN", path) and ok
        libs += 1
    print("bundle-linux-libs: rpath прописан: бинарник [%s], библиотек %d" % (rpath, libs))
    return ok


def rpath_of(path):
    try:
        proc = subprocess.run(["patchelf", "--print-rpath", path], capture_output=True, text=True)
    except OSError:
        return None
    if proc.returncode != 0:
        return None
    return proc.stdout.strip()


def check(root, quiet=False):
    """Проверить каталог так, как он будет работать на машине пользователя:
    без LD_LIBRARY_PATH, только за счёт rpath."""
    libs = os.path.join(root, "lib")
    if not os.path.isdir(libs):
        print("bundle-linux-libs: нет каталога %s" % libs, file=sys.stderr)
        return False

    files = []
    for dirpath, _dirnames, filenames in os.walk(root):
        for name in filenames:
            path = os.path.join(dirpath, name)
            if is_elf(path):
                files.append(path)

    inside = os.path.realpath(root)
    bad = []
    for path in files:
        rp = rpath_of(path)
        if rp is not None and "$ORIGIN" not in rp:
            bad.append((path, "rpath", "нет $ORIGIN: %r" % rp))
        try:
            proc = subprocess.run(["ldd", path], capture_output=True, text=True)
        except OSError:
            continue
        for raw in proc.stdout.splitlines():
            line = raw.strip()
            if "=>" not in line:
                continue
            name, rest = line.split("=>", 1)
            name, rest = name.strip(), rest.strip()
            if is_system(name):
                continue
            if rest.startswith("not found"):
                bad.append((path, name, "не найдена (нет rpath?)"))
                continue
            resolved = os.path.realpath(rest.split(" (")[0].strip())
            if not (resolved == inside or resolved.startswith(inside + os.sep)):
                bad.append((path, name, "взялась вне каталога: %s" % resolved))

    if not quiet:
        print("bundle-linux-libs: проверено ELF-файлов %d, внутри каталога %d библиотек"
              % (len(files), len(os.listdir(libs))))
    if bad:
        for path, name, where in bad:
            print("  %s: %s -> %s" % (os.path.relpath(path, root), name, where), file=sys.stderr)
        return False
    return True


def main():
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("--exe")
    ap.add_argument("--dest")
    ap.add_argument("--patch", action="store_true")
    ap.add_argument("--rpath", default=DEFAULT_RPATH)
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--dir")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    if args.check:
        if not args.dir:
            ap.error("--check требует --dir")
        ok = check(args.dir, quiet=args.quiet)
        print("bundle-linux-libs: %s" % ("зависимость полная" if ok else "НЕПОЛНАЯ ЗАВИСИМОСТЬ"))
        return 0 if ok else 1

    if not args.exe or not args.dest:
        ap.error("нужны --exe и --dest")

    if args.patch:
        return 0 if patch(args.exe, args.dest, args.rpath) else 1

    copied, missing = collect(args.exe, args.dest)
    print("bundle-linux-libs: скопировано %d библиотек:" % len(copied))
    for name in copied:
        print("  %s" % name)

    if missing:
        for who, name in missing:
            print("bundle-linux-libs: не найдена зависимость %s (нужна %s)" % (name, who),
                  file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
