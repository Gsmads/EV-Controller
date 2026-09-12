#!/usr/bin/env python3
"""Сторож границ платформы: две проверки из этапа 0.2 docs/SLICE_0_PLAN.md.

  headers    задача 0.2.4 — Arduino.h и avr/* не включаются нигде, кроме
             firmware/hal_atmega328p.cpp. Основание: ADR-0001, CONVENTIONS §1.1.

  functions  задача 0.2.5 — функции Arduino не вызываются нигде, ВКЛЮЧАЯ HAL.
             Основание: ADR-0002, CONVENTIONS §1.2 (список из 13 имён).

Обе проверки вырезают комментарии перед поиском: закомментированный вызов
нарушением не является, а в этом проекте платформенные имена в комментариях
встречаются часто (например «Timer0 используется Arduino для millis()»).

Исключения ниже — временные, введены ADR-0014 по решению владельца. Каждое
привязано к issue, которая его снимает. Список может только сокращаться:
для функций задано точное число допустимых вхождений, поэтому новый вызов
в том же файле сторож поймает.

ЧЕГО ЭТОТ СТОРОЖ НЕ ПОКРЫВАЕТ — читать до того, как на него положиться:

  * Косвенное включение. firmware/app_debug.cpp включает MICRO_UART.h и PRINT.h
    (строки 7-8), а те тянут Arduino.h. То есть файл слоя app_* получает весь
    Arduino Core, и проверка headers этого не видит: она смотрит только прямые
    директивы #include в самом файле.
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
  * Вызов через макрос или псевдоним. Поиск идёт по тексту исходника.
  * Каталог tools/. Там хостовый Python и JavaScript, где serial.Serial — это
    pyserial, а не Arduino. Проверка functions его не смотрит вовсе.
  * Точные строки исключений. Для функций проверяется пара «файл + имя» и число
    вхождений, а не конкретные строки: перемещение вызова внутри того же файла
    сторож не заметит.

Запуск:
    tools/ci/check_layer_boundaries.py headers
    tools/ci/check_layer_boundaries.py functions
    tools/ci/check_layer_boundaries.py all
"""

import os
import re
import sys

# ---------------------------------------------------------------------------
# Правила
# ---------------------------------------------------------------------------

# Единственный файл, которому платформенные заголовки разрешены (ADR-0001).
PLATFORM_FILE = "firmware/hal_atmega328p.cpp"

# Запрещённые идентификаторы, CONVENTIONS §1.2 (docs/CONVENTIONS.md:19-21).
ARDUINO_FUNCTIONS = [
    "digitalWrite", "digitalRead", "pinMode", "analogRead", "analogWrite",
    "millis", "micros", "delay", "delayMicroseconds", "shiftOut",
    "Serial", "Wire", "SPI",
]

SOURCE_SUFFIXES = (".c", ".cpp", ".h", ".ino")

# ---------------------------------------------------------------------------
# Исключения — ADR-0014. Временные. Снимаются вместе с указанными issue.
# ---------------------------------------------------------------------------

HEADER_EXCEPTIONS = {
    # Унаследовано от Grbl вместе с кодом UART. Снимается задачами #8 и #9:
    # обе правят этот файл по существу (TX-буфер и сентинел 0xFF).
    "firmware/MICRO_UART.h": "ADR-0014; снимается с #31",
    # Унаследованная отладочная печать, тот же источник.
    "firmware/PRINT.h": "ADR-0014; снимается с #31",
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


def repo_root():
    return os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def sources(*roots):
    """Все исходники под указанными каталогами, путями от корня репозитория."""
    root = repo_root()
    found = []
    for sub in roots:
        for dirpath, _dirnames, filenames in os.walk(os.path.join(root, sub)):
            for name in sorted(filenames):
                if name.endswith(SOURCE_SUFFIXES):
                    full = os.path.join(dirpath, name)
                    found.append(os.path.relpath(full, root).replace(os.sep, "/"))
    return sorted(found)


def strip_comments(text):
    """Убрать // и /* */, сохранив нумерацию строк.

    Строковые литералы не разбираются: "http://..." внутри строки будет усечён.
    Для поиска #include и имён функций это безразлично, но знать об этом надо.
    """
    out = []
    i, n = 0, len(text)
    while i < n:
        if text.startswith("/*", i):
            end = text.find("*/", i + 2)
            end = n if end < 0 else end + 2
            out.append(re.sub(r"[^\n]", "", text[i:end]))  # оставить только переводы строк
            i = end
        elif text.startswith("//", i):
            end = text.find("\n", i)
            i = n if end < 0 else end
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def read_lines(path):
    with open(os.path.join(repo_root(), path), encoding="utf-8", errors="replace") as fh:
        return strip_comments(fh.read()).split("\n")


def check_headers():
    """0.2.4 — Arduino.h и avr/* только в hal_atmega328p.cpp."""
    pattern = re.compile(r"^\s*#\s*include\s*[<\"]\s*(Arduino\.h|avr/[A-Za-z0-9_./]+)\s*[>\"]")
    violations, used_exceptions = [], set()

    for path in sources("firmware"):
        for num, line in enumerate(read_lines(path), 1):
            match = pattern.match(line)
            if not match:
                continue
            if path == PLATFORM_FILE:
                continue
            if path in HEADER_EXCEPTIONS:
                used_exceptions.add(path)
                continue
            violations.append((path, num, match.group(1), line.strip()))

    if violations:
        print("Сторож 0.2.4: платформенный заголовок вне " + PLATFORM_FILE, file=sys.stderr)
        print(file=sys.stderr)
        for path, num, header, line in violations:
            print("    %s:%d   %s" % (path, num, line), file=sys.stderr)
            print("        найдено: %s" % header, file=sys.stderr)
        print(file=sys.stderr)
        print("Правило: Arduino.h и avr/* разрешены только в %s." % PLATFORM_FILE, file=sys.stderr)
        print("Это то, что делает проект переносимым: смена платформы должна", file=sys.stderr)
        print("сводиться к замене одного файла.", file=sys.stderr)
        print("Основание: ADR-0001, docs/CONVENTIONS.md §1.1, CLAUDE.md §3 правило 1.", file=sys.stderr)
        print("Исключение возможно только новым ADR (docs/CONVENTIONS.md:23).", file=sys.stderr)
        return 1

    print("Сторож 0.2.4: платформенные заголовки только в %s." % PLATFORM_FILE)
    for path in sorted(used_exceptions):
        print("    исключение: %s (%s)" % (path, HEADER_EXCEPTIONS[path]))
    for path in sorted(set(HEADER_EXCEPTIONS) - used_exceptions):
        print("    исключение БОЛЬШЕ НЕ НУЖНО: %s — убрать из сторожа" % path)
    return 0


def check_functions():
    """0.2.5 — функции Arduino не вызываются нигде, включая HAL."""
    pattern = re.compile(r"\b(" + "|".join(ARDUINO_FUNCTIONS) + r")\b")
    violations = []
    counted = {}

    for path in sources("firmware", "tests"):
        for num, line in enumerate(read_lines(path), 1):
            for match in pattern.finditer(line):
                name = match.group(1)
                key = (path, name)
                if key in FUNCTION_EXCEPTIONS:
                    counted[key] = counted.get(key, 0) + 1
                    if counted[key] <= FUNCTION_EXCEPTIONS[key]:
                        continue
                violations.append((path, num, name, line.strip()))

    if violations:
        print("Сторож 0.2.5: вызов функции Arduino", file=sys.stderr)
        print(file=sys.stderr)
        for path, num, name, line in violations:
            print("    %s:%d   %s" % (path, num, line[:100]), file=sys.stderr)
            print("        найдено: %s" % name, file=sys.stderr)
            allowed = FUNCTION_EXCEPTIONS.get((path, name))
            if allowed is not None:
                print("        исключение допускает %d вхождений, найдено больше" % allowed,
                      file=sys.stderr)
        print(file=sys.stderr)
        print("Правило: функции Arduino не используются нигде, включая HAL —", file=sys.stderr)
        print("вместо них собственные реализации на регистрах.", file=sys.stderr)
        print("Запрещены: %s." % ", ".join(ARDUINO_FUNCTIONS), file=sys.stderr)
        print("Основание: ADR-0002, docs/CONVENTIONS.md §1.2.", file=sys.stderr)
        print("Исключение возможно только новым ADR (docs/CONVENTIONS.md:23).", file=sys.stderr)
        return 1

    print("Сторож 0.2.5: вызовов функций Arduino вне исключений нет.")
    for key in sorted(FUNCTION_EXCEPTIONS):
        path, name = key
        allowed, actual = FUNCTION_EXCEPTIONS[key], counted.get(key, 0)
        note = "" if actual == allowed else "  <- найдено %d, исключение можно сузить" % actual
        print("    исключение: %s / %s, до %d (%s)%s"
              % (path, name, allowed, FUNCTION_EXCEPTION_REASON, note))
    return 0


def main(argv):
    checks = {"headers": check_headers, "functions": check_functions}
    what = argv[1] if len(argv) > 1 else "all"
    if what == "all":
        return max(check_headers(), check_functions())
    if what not in checks:
        print("Использование: %s {headers|functions|all}" % argv[0], file=sys.stderr)
        return 2
    return checks[what]()


if __name__ == "__main__":
    sys.exit(main(sys.argv))
