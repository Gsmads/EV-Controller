#!/usr/bin/env python3
"""Сторож границ платформы: две проверки из этапа 0.2 docs/SLICE_0_PLAN.md.

  headers    задача 0.2.4 — платформенные заголовки не включаются нигде, кроме
             firmware/hal_atmega328p.cpp. Основание: ADR-0001, CONVENTIONS §1.1.

  functions  задача 0.2.5 — функции Arduino не вызываются нигде, ВКЛЮЧАЯ HAL.
             Основание: ADR-0002, CONVENTIONS §1.2 (список из 13 имён).

Коды возврата разделены намеренно:

    0   проверено, нарушений нет
    1   найдено нарушение
    2   проверить не удалось (нет каталога, нечитаемый файл, чужой корень)

Отказ инструмента не должен выглядеть ни как успех, ни как нарушение: в первом
случае правило молча выключается, во втором — CI краснеет не по делу.

РАЗБОР ИСХОДНИКА повторяет фазы трансляции C, а не ищет по сырому тексту:

  1. снимается BOM, переводы строк приводятся к \\n;
  2. фаза 2 — склейка строк, оканчивающихся обратной косой чертой
     (`#include \\` + перевод строки + `<Arduino.h>` — валидная директива);
  3. содержимое комментариев И СТРОКОВЫХ ЛИТЕРАЛОВ заменяется пробелами,
     нумерация строк сохраняется.

Шаг 3 нужен в обе стороны. Без него строка `"/*"` в коде гасит обе проверки
до ближайшего `*/` или до конца файла — сторож печатает успех, не посмотрев
ни на что. И наоборот: слово `delay` в тексте диагностического сообщения или
поле структуры `uint16_t delay;` засчитывались бы за вызов.

Исключения ниже — временные, введены ADR-0014 по решению владельца. Каждое
привязано к issue, которая его снимает. Список может только сокращаться:
для заголовков перечислены конкретные имена, для функций — точное число
допустимых вхождений.

ЧЕГО ЭТОТ СТОРОЖ НЕ ПОКРЫВАЕТ — читать до того, как на него положиться:

  * Косвенное включение. firmware/app_debug.cpp включает MICRO_UART.h и PRINT.h
    (строки 7-8), а те тянут Arduino.h. То есть файл слоя app_* получает весь
    Arduino Core, и проверка headers этого не видит: она смотрит только прямые
    директивы #include в самом файле. Закрыть это может лишь разбор реального
    дерева включений (gcc -H), а он требует собираемости всех модулей — см.
    issue #33.
  * Использование платформенных имён без включения их заголовка. В
    MICRO_UART.cpp есть ISR(SERIAL_UDRE), UDR0, UCSR0B, UBRR0H — ни avr/io.h,
    ни avr/interrupt.h там не включены. В app_debug.cpp и EV_Controller.ino
    есть PSTR, в PRINT.cpp — pgm_read_byte_near; avr/pgmspace.h не включён
    нигде. Ни то, ни другое сторож не ловит.
  * firmware/EV_Controller.ino. Arduino IDE подставляет Arduino.h в скетч сама,
    строки #include там нет и не будет.
  * Макросы A0..A7 в firmware/cfg_board.h (строки 41-46). Они определены только
    в Arduino.h, из-за чего svc_pedals.cpp не собирается вне Arduino IDE, —
    но это не #include и не вызов функции, и под обе проверки не подпадает.
  * Условная компиляция. Директива внутри #if 0 или #ifdef считается
    нарушением: препроцессор сторож не разворачивает.
  * Вызов или включение через макрос: `#define HDR <Arduino.h>` + `#include HDR`,
    склейка имени оператором ## . Поиск идёт по тексту после фаз 1-3, макросы
    не подставляются.
  * Вызов, разорванный переводом строки без обратной косой: `millis` на одной
    строке, `()` на следующей. Проверка functions требует скобку за именем и
    смотрит строку целиком, но не через её конец.
  * Платформенный заголовок, закрытый одноимённым файлом проекта. Имя, которому
    соответствует существующий файл под firmware/, считается своим (так
    firmware/PRINT.h не путается с Print.h из ядра Arduino). Обратная сторона:
    firmware/Arduino.h-заглушка вывела бы включения из-под проверки.
  * Каталог tools/. Там хостовый Python и JavaScript, где serial.Serial — это
    pyserial, а не Arduino. Проверка functions его не смотрит вовсе.
  * Точные строки исключений по функциям. Проверяется пара «файл + имя» и число
    вхождений; при превышении печатаются ВСЕ вхождения, чтобы человек увидел,
    какое из них новое, но привязки к строкам у исключения нет.
  * Имена avr-libc сверх списка CONVENTIONS §1.2: _delay_ms, _delay_us и
    подобные в списке запрещённых идентификаторов не значатся. Практически они
    закрыты правилом заголовков (объявлены в util/delay.h), но прямой проверки
    по имени нет — расширение списка меняет обещание §1.2 и требует решения
    владельца.

Самопроверка сторожа: tools/ci/test_check_layer_boundaries.py

Запуск:
    tools/ci/check_layer_boundaries.py headers
    tools/ci/check_layer_boundaries.py functions
    tools/ci/check_layer_boundaries.py all
"""

import os
import posixpath
import re
import sys

# ---------------------------------------------------------------------------
# Правила
# ---------------------------------------------------------------------------

# Единственный файл, которому платформенные заголовки разрешены (ADR-0001).
PLATFORM_FILE = "firmware/hal_atmega328p.cpp"

# Запрещённые идентификаторы, CONVENTIONS §1.2 (docs/CONVENTIONS.md:19-21).
# Разделены по способу употребления: функции вызываются, объекты адресуются
# точкой. Требование «за именем идёт скобка или точка» отсекает поле структуры
# uint16_t delay и константу перечисления SPI, оставляя настоящие обращения.
ARDUINO_CALLS = [
    "digitalWrite", "digitalRead", "pinMode", "analogRead", "analogWrite",
    "millis", "micros", "delayMicroseconds", "delay", "shiftOut",
]
ARDUINO_OBJECTS = ["Serial", "Wire", "SPI"]
ARDUINO_FUNCTIONS = ARDUINO_CALLS + ARDUINO_OBJECTS

# Платформенные заголовки. Пространства имён avr-libc целиком: кроме avr/*
# сюда входят util/* (util/delay.h, util/atomic.h) и compat/*.
PLATFORM_NAMESPACES = ("avr", "util", "compat")

# Заголовки без каталога. Сверяются без учёта регистра, поэтому проектный
# firmware/PRINT.h снимается отдельной проверкой «файл существует под firmware/».
PLATFORM_HEADERS = {
    "arduino.h", "wprogram.h",
    "wiring.h", "wiring_private.h", "wiring_digital.h", "wiring_analog.h",
    "wiring_shift.h", "wiring_pulse.h",
    "hardwareserial.h", "print.h", "printable.h", "stream.h",
    "wstring.h", "wcharacter.h", "pins_arduino.h", "binary.h", "usbapi.h",
    "softwareserial.h", "spi.h", "wire.h", "eeprom.h",
}

# Регистр не учитывается: файловые системы macOS и Windows не различают .H и .h.
SOURCE_SUFFIXES = (
    ".c", ".cc", ".cxx", ".cpp", ".c++",
    ".h", ".hh", ".hxx", ".hpp", ".h++",
    ".inc", ".ipp", ".tpp", ".ino", ".s",
)

# ---------------------------------------------------------------------------
# Исключения — ADR-0014. Временные. Снимаются вместе с указанными issue.
# ---------------------------------------------------------------------------

# файл -> (какие именно заголовки ему прощаются, причина).
# Именами, а не файлом целиком: иначе в MICRO_UART.h можно было бы дописать
# avr/wdt.h, и сторож бы промолчал.
HEADER_EXCEPTIONS = {
    # Унаследовано от Grbl вместе с кодом UART. Снимается задачей #31.
    "firmware/MICRO_UART.h": ({"Arduino.h"}, "ADR-0014; снимается с #31"),
    # Унаследованная отладочная печать, тот же источник.
    "firmware/PRINT.h": ({"Arduino.h"}, "ADR-0014; снимается с #31"),
}

# (файл, идентификатор) -> сколько вхождений допускается. Больше — отказ.
FUNCTION_EXCEPTIONS = {
    (PLATFORM_FILE, "pinMode"): 3,           # hal_gpio_mode, строки 41-43
    (PLATFORM_FILE, "digitalWrite"): 1,      # hal_gpio_write, строка 49
    (PLATFORM_FILE, "digitalRead"): 1,       # hal_gpio_read, строка 54
    (PLATFORM_FILE, "millis"): 1,            # hal_system_millis, строка 289
    (PLATFORM_FILE, "delayMicroseconds"): 1, # hal_system_delay_us, строка 294
}
FUNCTION_EXCEPTION_REASON = "ADR-0014; снимается с #32"

# ---------------------------------------------------------------------------
# Разбор исходника
# ---------------------------------------------------------------------------

RC_CLEAN, RC_VIOLATION, RC_BROKEN = 0, 1, 2


class GuardError(Exception):
    """Сторож не смог выполнить проверку. Не то же самое, что нарушение."""


def repo_root():
    """Корень репозитория с проверкой признаков.

    realpath, а не abspath: запуск через символическую ссылку иначе даёт корень
    рядом со ссылкой, дерево получается пустым, и прогон выходит зелёным.
    """
    here = os.path.realpath(__file__)
    root = os.path.dirname(os.path.dirname(os.path.dirname(here)))
    for marker in ("firmware", "tools/ci"):
        if not os.path.isdir(os.path.join(root, marker)):
            raise GuardError(
                "корень репозитория определён как %s, но в нём нет %s.\n"
                "Сторож запущен не из дерева проекта." % (root, marker))
    return root


def sources(*roots):
    """Исходники под указанными каталогами, путями от корня репозитория.

    followlinks=True: подкаталог-символическая ссылка внутри firmware/ иначе
    не просматривается вовсе. Повторные заходы отсекаются по realpath.
    """
    root = repo_root()
    found, seen = [], set()
    for sub in roots:
        base = os.path.join(root, sub)
        if not os.path.isdir(base):
            raise GuardError("каталог %s не найден — проверять нечего" % sub)
        for dirpath, dirnames, filenames in os.walk(base, followlinks=True):
            real = os.path.realpath(dirpath)
            if real in seen:
                dirnames[:] = []
                continue
            seen.add(real)
            for name in sorted(filenames):
                if name.lower().endswith(SOURCE_SUFFIXES):
                    full = os.path.join(dirpath, name)
                    found.append(os.path.relpath(full, root).replace(os.sep, "/"))
    if not found:
        raise GuardError("в %s не найдено ни одного исходника — проверять нечего"
                         % ", ".join(roots))
    return sorted(found)


def splice(text):
    """Фаза трансляции 2: склейка строк, оканчивающихся обратной косой чертой.

    Возвращает [(номер первой физической строки, логическая строка)].
    """
    physical = text.replace("\r\n", "\n").replace("\r", "\n").split("\n")
    out, i = [], 0
    while i < len(physical):
        first, line = i + 1, physical[i]
        while line.endswith("\\") and i + 1 < len(physical):
            line = line[:-1] + physical[i + 1]
            i += 1
        out.append((first, line))
        i += 1
    return out


RAW_STRING = re.compile(r'(?:u8|u|U|L)?R"([^ ()\\\t\v\f]{0,16})\(')

# Имя заголовка в директиве — не строковый литерал, а отдельный вид лексемы:
# кавычки в #include "Arduino.h" вычищать нельзя, иначе проверка ослепнет.
INCLUDE_PREFIX = re.compile(r'^\s*(?:#|%:)\s*include(?:_next)?\s*$')
HEADER_TOKEN = re.compile(r'[<"][^>"]*[>"]')

# Апостроф не всегда открывает символьный литерал: в C++ он же разделитель
# разрядов (1\'000). Открываем состояние только на настоящем литерале.
CHAR_LITERAL = re.compile(r"'(?:\\.|[^'\\]){1,4}'")


def scrub(lines):
    """Заменить пробелами содержимое комментариев и литералов.

    Длина строк и их количество сохраняются, поэтому номера строк и столбцов
    остаются верными. Возвращает (строки, конечное состояние разбора).
    """
    out, state, raw_end = [], "code", ""
    for line in lines:
        buf, i, n = [], 0, len(line)
        while i < n:
            if state == "code":
                raw = RAW_STRING.match(line, i)
                if raw:
                    raw_end = ")" + raw.group(1) + '"'
                    buf.append(" " * (raw.end() - i))
                    i, state = raw.end(), "raw"
                    continue
                if line.startswith("/*", i):
                    buf.append("  ")
                    i, state = i + 2, "block"
                    continue
                if line.startswith("//", i):
                    buf.append(" " * (n - i))
                    i = n
                    continue
                if line[i] in "<\"" and INCLUDE_PREFIX.match(line[:i]):
                    token = HEADER_TOKEN.match(line, i)
                    if token:
                        buf.append(token.group(0))
                        i = token.end()
                        continue
                if line[i] == '"':
                    state = "string"
                    buf.append(" ")
                    i += 1
                    continue
                if line[i] == "'" and CHAR_LITERAL.match(line, i):
                    state = "char"
                    buf.append(" ")
                    i += 1
                    continue
                buf.append(line[i])
                i += 1
            elif state == "block":
                end = line.find("*/", i)
                if end < 0:
                    buf.append(" " * (n - i))
                    i = n
                else:
                    buf.append(" " * (end + 2 - i))
                    i, state = end + 2, "code"
            elif state == "raw":
                end = line.find(raw_end, i)
                if end < 0:
                    buf.append(" " * (n - i))
                    i = n
                else:
                    buf.append(" " * (end + len(raw_end) - i))
                    i, state = end + len(raw_end), "code"
            else:  # string, char
                quote = '"' if state == "string" else "'"
                if line[i] == "\\":
                    step = 2 if i + 1 < n else 1
                    buf.append(" " * step)
                    i += step
                    continue
                if line[i] == quote:
                    state = "code"
                buf.append(" ")
                i += 1
        # Незакрытый литерал — ошибка компиляции, но ослепить остаток файла
        # мы ему не даём: закрываем на конце строки.
        if state in ("string", "char"):
            state = "code"
        out.append("".join(buf))
    return out, state


def read_source(path):
    """[(номер строки, исходный текст, текст без комментариев и литералов)]."""
    full = os.path.join(repo_root(), path)
    try:
        with open(full, encoding="utf-8-sig", errors="replace") as fh:
            text = fh.read()
    except OSError as err:
        raise GuardError("не удалось прочитать %s: %s" % (path, err))
    numbered = splice(text)
    clean, state = scrub([line for _, line in numbered])
    if state != "code":
        raise GuardError(
            "%s: незакрытый %s — разобрать файл нельзя"
            % (path, "блочный комментарий" if state == "block" else "сырой литерал"))
    return [(num, orig, cln) for (num, orig), cln in zip(numbered, clean)]


# ---------------------------------------------------------------------------
# Проверка 0.2.4 — заголовки
# ---------------------------------------------------------------------------

INCLUDE = re.compile(r'^\s*(?:#|%:)\s*include(?:_next)?\s*[<"]([^>"]*)[>"]')


def platform_header(raw):
    """Имя платформенного заголовка или None.

    normpath схлопывает ./ и повторные слэши: <.//avr/io.h> — это avr/io.h.
    """
    name = posixpath.normpath(raw.strip())
    if not name or name in (".", ".."):
        return None
    # Свой заголовок проекта платформенным не считается.
    if os.path.exists(os.path.join(repo_root(), "firmware", name)):
        return None
    low = name.lower()
    if "/" in low and low.split("/")[0] in PLATFORM_NAMESPACES:
        return name
    if low in PLATFORM_HEADERS:
        return name
    return None


def check_headers():
    """0.2.4 — платформенные заголовки только в hal_atmega328p.cpp."""
    files = sources("firmware")
    violations, used = [], {}

    for path in files:
        allowed = HEADER_EXCEPTIONS.get(path, (frozenset(), ""))[0]
        for num, orig, clean in read_source(path):
            match = INCLUDE.match(clean)
            if not match:
                continue
            header = platform_header(match.group(1))
            if header is None or path == PLATFORM_FILE:
                continue
            if header in allowed:
                used.setdefault(path, set()).add(header)
                continue
            violations.append((path, num, header, orig.strip()))

    if violations:
        print("Сторож 0.2.4: платформенный заголовок вне " + PLATFORM_FILE, file=sys.stderr)
        print(file=sys.stderr)
        for path, num, header, line in violations:
            print("    %s:%d   %s" % (path, num, line[:100]), file=sys.stderr)
            print("        найдено: %s" % header, file=sys.stderr)
        print(file=sys.stderr)
        print("Правило: платформенные заголовки разрешены только в %s." % PLATFORM_FILE, file=sys.stderr)
        print("Это то, что делает проект переносимым: смена платформы должна", file=sys.stderr)
        print("сводиться к замене одного файла.", file=sys.stderr)
        print("Считаются платформенными: Arduino.h и ядро Arduino, а также всё", file=sys.stderr)
        print("из пространств %s." % ", ".join(n + "/*" for n in PLATFORM_NAMESPACES), file=sys.stderr)
        print("Основание: ADR-0001, docs/CONVENTIONS.md §1.1, CLAUDE.md §3 правило 1.", file=sys.stderr)
        print("Исключение возможно только новым ADR (docs/CONVENTIONS.md:23).", file=sys.stderr)
        return RC_VIOLATION

    print("Сторож 0.2.4: платформенные заголовки только в %s (просмотрено %d файлов)."
          % (PLATFORM_FILE, len(files)))
    for path in sorted(HEADER_EXCEPTIONS):
        allowed, reason = HEADER_EXCEPTIONS[path]
        spent = used.get(path, set())
        for header in sorted(allowed):
            if header in spent:
                print("    исключение: %s / %s (%s)" % (path, header, reason))
            else:
                print("    исключение БОЛЬШЕ НЕ НУЖНО: %s / %s — убрать из сторожа"
                      % (path, header))
    return RC_CLEAN


# ---------------------------------------------------------------------------
# Проверка 0.2.5 — функции
# ---------------------------------------------------------------------------

# Длинные имена раньше коротких: delayMicroseconds не должен разбираться
# как delay с хвостом.
CALLS = re.compile(r"\b(" + "|".join(sorted(ARDUINO_CALLS, key=len, reverse=True)) + r")\s*\(")
OBJECTS = re.compile(r"\b(" + "|".join(ARDUINO_OBJECTS) + r")\s*[.(]")


def check_functions():
    """0.2.5 — функции Arduino не вызываются нигде, включая HAL."""
    files = sources("firmware", "tests")
    hits = []

    for path in files:
        for num, orig, clean in read_source(path):
            for pattern in (CALLS, OBJECTS):
                for match in pattern.finditer(clean):
                    hits.append((path, num, match.group(1), orig.strip()))

    counted = {}
    for hit in hits:
        key = (hit[0], hit[2])
        counted.setdefault(key, []).append(hit)

    violations, over = [], set()
    for key, group in sorted(counted.items()):
        allowed = FUNCTION_EXCEPTIONS.get(key)
        if allowed is None:
            violations.extend(group)
        elif len(group) > allowed:
            # Печатаются ВСЕ вхождения: какое из них лишнее, видно только
            # человеку, знающему историю файла.
            violations.extend(group)
            over.add(key)

    if violations:
        print("Сторож 0.2.5: вызов функции Arduino", file=sys.stderr)
        print(file=sys.stderr)
        for path, num, name, line in sorted(violations):
            print("    %s:%d   %s" % (path, num, line[:100]), file=sys.stderr)
            print("        найдено: %s" % name, file=sys.stderr)
        for key in sorted(over):
            print(file=sys.stderr)
            print("    %s: исключение ADR-0014 допускает %d вхождений %s, найдено %d."
                  % (key[0], FUNCTION_EXCEPTIONS[key], key[1], len(counted[key])),
                  file=sys.stderr)
            print("    Перечислены все — лишнее среди них.", file=sys.stderr)
        print(file=sys.stderr)
        print("Правило: функции Arduino не используются нигде, включая HAL —", file=sys.stderr)
        print("вместо них собственные реализации на регистрах.", file=sys.stderr)
        print("Запрещены: %s." % ", ".join(ARDUINO_FUNCTIONS), file=sys.stderr)
        print("Основание: ADR-0002, docs/CONVENTIONS.md §1.2.", file=sys.stderr)
        print("Исключение возможно только новым ADR (docs/CONVENTIONS.md:23).", file=sys.stderr)
        return RC_VIOLATION

    print("Сторож 0.2.5: вызовов функций Arduino вне исключений нет (просмотрено %d файлов)."
          % len(files))
    for key in sorted(FUNCTION_EXCEPTIONS):
        path, name = key
        allowed, actual = FUNCTION_EXCEPTIONS[key], len(counted.get(key, ()))
        note = "" if actual == allowed else "  <- найдено %d, исключение можно сузить" % actual
        print("    исключение: %s / %s, до %d (%s)%s"
              % (path, name, allowed, FUNCTION_EXCEPTION_REASON, note))
    return RC_CLEAN


def main(argv):
    checks = {"headers": check_headers, "functions": check_functions}
    what = argv[1] if len(argv) > 1 else "all"
    if what != "all" and what not in checks:
        print("Использование: %s {headers|functions|all}" % argv[0], file=sys.stderr)
        return RC_BROKEN
    try:
        if what == "all":
            return max(check_headers(), check_functions())
        return checks[what]()
    except GuardError as err:
        print(file=sys.stderr)
        print("ОТКАЗ СТОРОЖА: %s" % err, file=sys.stderr)
        print("Это не нарушение границ, а невозможность проверить. Код возврата 2.", file=sys.stderr)
        return RC_BROKEN


if __name__ == "__main__":
    sys.exit(main(sys.argv))
