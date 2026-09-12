#!/usr/bin/env python3
"""Самопроверка сторожа границ платформы.

Каждый случай — из подтверждённой атаки на check_layer_boundaries.py. На
версии сторожа до исправления красными были случаи, помеченные ATTACK:
они проходили молча, то есть правило было выключено, а CI это не показывал.

Проверка идёт на временном дереве, а не на репозитории: сторож копируется
в <tmp>/tools/ci/, рядом создаётся <tmp>/firmware/ с нужным содержимым.
Так проверяется и вычисление корня, и обход, а не только регулярные выражения.

Коды возврата сторожа: 0 — чисто, 1 — нарушение, 2 — проверить не удалось.

Запуск:
    tools/ci/test_check_layer_boundaries.py
"""

import os
import shutil
import subprocess
import sys
import tempfile

GUARD = os.path.join(os.path.dirname(os.path.realpath(__file__)),
                     "check_layer_boundaries.py")

CLEAN, VIOLATION, BROKEN = 0, 1, 2

passed = failed = 0

# Ровно те вхождения, которые разрешает ADR-0014: pinMode 3, остальные по 1.
HAL = """\
#include <Arduino.h>
#include <avr/io.h>
void hal_gpio_mode(uint8_t p, uint8_t m) {
    if (m == 0) { pinMode(p, INPUT); }
    else if (m == 1) { pinMode(p, OUTPUT); }
    else { pinMode(p, INPUT_PULLUP); }
}
void hal_gpio_write(uint8_t p, uint8_t v) { digitalWrite(p, v); }
uint8_t hal_gpio_read(uint8_t p) { return digitalRead(p); }
uint32_t hal_system_millis(void) { return millis(); }
void hal_system_delay_us(uint16_t us) { delayMicroseconds(us); }
"""

RAMP = """\
/* Рампа разгона. Timer0 занят Arduino под millis(), трогать нельзя. */
#include "svc_ramp.h"
#include <stdint.h>
void svc_ramp_update(void) { }
"""

BASE = {
    "firmware/hal_atmega328p.cpp": HAL,
    "firmware/svc_ramp.cpp": RAMP,
    "firmware/svc_ramp.h": "#pragma once\nvoid svc_ramp_update(void);\n",
    "firmware/MICRO_UART.h": '#pragma once\n#include "Arduino.h"\nvoid serial_init(void);\n',
    "firmware/PRINT.h": '#pragma once\n#include "Arduino.h"\nvoid printInteger(long n);\n',
    "tests/test_util_math.c": "#include <stdint.h>\nint main(void) { return 0; }\n",
}


def make_tree(files=None, drop=(), links=()):
    """Временное дерево с копией сторожа. files дописывается поверх BASE."""
    root = tempfile.mkdtemp(prefix="guard-selftest-")
    content = dict(BASE)
    for path in drop:
        content.pop(path, None)
    content.update(files or {})
    for path, text in content.items():
        full = os.path.join(root, path)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        mode = "wb" if isinstance(text, bytes) else "w"
        with open(full, mode) as fh:
            fh.write(text)
    for path, target in links:
        full = os.path.join(root, path)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        os.symlink(target, full)
    os.makedirs(os.path.join(root, "tools", "ci"), exist_ok=True)
    shutil.copy2(GUARD, os.path.join(root, "tools", "ci"))
    return root


def run(root, what="all", script=None):
    """Запустить сторожа в дереве. Возвращает (код, весь вывод)."""
    path = script or os.path.join(root, "tools", "ci", "check_layer_boundaries.py")
    proc = subprocess.run([sys.executable, path, what],
                          capture_output=True, text=True, cwd="/")
    return proc.returncode, proc.stdout + proc.stderr


def case(name, expect_rc, files=None, drop=(), what="all", expect_text=(),
         reject_text=(), script_at=None, links=()):
    global passed, failed
    root = make_tree(files, drop, links)
    try:
        script = None
        if script_at:
            script = os.path.join(root, script_at)
            if not os.path.exists(script):   # символическая ссылка уже есть
                os.makedirs(os.path.dirname(script), exist_ok=True)
                shutil.copy2(GUARD, script)
        rc, out = run(root, what, script)
        problems = []
        if rc != expect_rc:
            problems.append("код %d, ожидался %d" % (rc, expect_rc))
        for needle in expect_text:
            if needle not in out:
                problems.append("в выводе нет %r" % needle)
        for needle in reject_text:
            if needle in out:
                problems.append("в выводе есть лишнее %r" % needle)
        if problems:
            failed += 1
            print("FAIL: %s" % name)
            for problem in problems:
                print("      %s" % problem)
            print("      вывод: %s" % out.strip().replace("\n", "\n              "))
        else:
            passed += 1
            print("PASS: %s" % name)
    finally:
        shutil.rmtree(root, ignore_errors=True)


def main():
    print("=" * 60)
    print("  Самопроверка сторожа границ платформы")
    print("=" * 60)

    # --- базовое поведение ------------------------------------------------
    case("чистое дерево — зелёный", CLEAN,
         expect_text=("просмотрено", "исключение: firmware/MICRO_UART.h / Arduino.h"))
    case("нарушение в комментарии не считается", CLEAN,
         {"firmware/svc_ramp.cpp": RAMP.replace(
             "void svc_ramp_update",
             "/* #include <Arduino.h> и digitalWrite(9, 1); */\nvoid svc_ramp_update")})

    # --- заголовки: прямые -------------------------------------------------
    case("Arduino.h в сервисе", VIOLATION,
         {"firmware/svc_ramp.cpp": "#include <Arduino.h>\n" + RAMP},
         expect_text=("firmware/svc_ramp.cpp:1",))
    case("avr/io.h в сервисе", VIOLATION,
         {"firmware/svc_ramp.cpp": "#include <avr/io.h>\n" + RAMP})

    # --- заголовки: находки атаки -----------------------------------------
    case("ATTACK util/delay.h — avr-libc вне пространства avr/", VIOLATION,
         {"firmware/svc_ramp.cpp": "#include <util/delay.h>\n" + RAMP})
    case("ATTACK util/atomic.h", VIOLATION,
         {"firmware/svc_ramp.cpp": "#include <util/atomic.h>\n" + RAMP})
    case("ATTACK compat/twi.h", VIOLATION,
         {"firmware/svc_ramp.cpp": "#include <compat/twi.h>\n" + RAMP})
    case("ATTACK wiring_private.h — ядро Arduino без Arduino.h", VIOLATION,
         {"firmware/svc_ramp.cpp": "#include <wiring_private.h>\n" + RAMP})
    case("ATTACK HardwareSerial.h", VIOLATION,
         {"firmware/svc_ramp.cpp": "#include <HardwareSerial.h>\n" + RAMP})
    case("ATTACK префикс ./ в имени заголовка", VIOLATION,
         {"firmware/svc_ramp.cpp": "#include <./avr/io.h>\n" + RAMP})
    case("ATTACK двойной слэш в имени заголовка", VIOLATION,
         {"firmware/svc_ramp.cpp": "#include <.//Arduino.h>\n" + RAMP})
    case("ATTACK диграф %:include", VIOLATION,
         {"firmware/svc_ramp.cpp": "%:include <Arduino.h>\n" + RAMP})
    case("ATTACK #include_next", VIOLATION,
         {"firmware/svc_ramp.cpp": "#include_next <avr/io.h>\n" + RAMP})
    case("ATTACK UTF-8 BOM прячет первую строку", VIOLATION,
         {"firmware/svc_ramp.cpp": ("﻿#include <Arduino.h>\n" + RAMP).encode("utf-8")})
    case("ATTACK склейка строк: директива разорвана", VIOLATION,
         {"firmware/svc_ramp.cpp": "#include \\\n<Arduino.h>\n" + RAMP},
         expect_text=("firmware/svc_ramp.cpp:1",))
    case("ATTACK склейка строк: имя разорвано", VIOLATION,
         {"firmware/svc_ramp.cpp": "#include <Ardui\\\nno.h>\n" + RAMP})
    case("ATTACK расширение .hpp вне списка просматриваемых", VIOLATION,
         {"firmware/bridge.hpp": "#pragma once\n#include <Arduino.h>\n"})
    case("ATTACK расширение .H в верхнем регистре", VIOLATION,
         {"firmware/BRIDGE.H": "#pragma once\n#include <avr/io.h>\n"})

    # --- слепота от строкового литерала -----------------------------------
    case("ATTACK литерал с /* гасит остаток файла (заголовок)", VIOLATION,
         {"firmware/svc_ramp.cpp":
          'static const char kGlyph[] = "/*";\n#include <Arduino.h>\n' + RAMP},
         expect_text=("firmware/svc_ramp.cpp:2",))
    case("ATTACK литерал с /* гасит остаток файла (вызов)", VIOLATION,
         {"firmware/svc_ramp.cpp":
          'static const char kGlyph[] = "/*";\nvoid boom(void) { digitalWrite(13, 1); }\n' + RAMP})
    case("ATTACK литерал с // обрезает остаток строки", VIOLATION,
         {"firmware/svc_ramp.cpp":
          'void boom(void) { const char *u = "//"; digitalWrite(13, 1); }\n' + RAMP})

    # --- функции -----------------------------------------------------------
    case("digitalWrite в сервисе", VIOLATION,
         {"firmware/svc_ramp.cpp": RAMP.replace(
             "void svc_ramp_update(void) { }",
             "void svc_ramp_update(void) { digitalWrite(9, 1); }")},
         expect_text=("digitalWrite",))
    case("ATTACK имя разорвано склейкой строк", VIOLATION,
         {"firmware/svc_ramp.cpp": RAMP.replace(
             "void svc_ramp_update(void) { }",
             "void svc_ramp_update(void) { digital\\\nWrite(9, 1); }")})

    # --- ложные срабатывания ----------------------------------------------
    case("ATTACK поле структуры uint16_t delay не вызов", CLEAN,
         {"firmware/svc_ramp.cpp": RAMP.replace(
             "void svc_ramp_update(void) { }",
             "typedef struct { uint16_t delay; uint16_t micros; } ramp_t;\n"
             "void svc_ramp_update(void) { }")})
    case("ATTACK запрещённое слово в тексте сообщения не вызов", CLEAN,
         {"firmware/svc_ramp.cpp": RAMP.replace(
             "void svc_ramp_update(void) { }",
             'static const char kMsg[] = "millis() overflow, call delay()";\n'
             "void svc_ramp_update(void) { }")})
    case("ATTACK константа перечисления SPI не обращение к объекту", CLEAN,
         {"firmware/svc_ramp.cpp": RAMP.replace(
             "void svc_ramp_update(void) { }",
             "enum bus_e { SPI = 0, I2C = 1 };\nvoid svc_ramp_update(void) { }")})
    case("ATTACK сырой литерал с текстом директивы не нарушение", CLEAN,
         {"firmware/svc_ramp.cpp": RAMP.replace(
             "void svc_ramp_update(void) { }",
             'static const char kDoc[] = R"(#include <Arduino.h>)";\n'
             "void svc_ramp_update(void) { }")})
    case("ATTACK многострочный сырой литерал: директива с начала строки", CLEAN,
         {"firmware/svc_ramp.cpp": RAMP.replace(
             "void svc_ramp_update(void) { }",
             'static const char kDoc[] = R"DOC(\n'
             "#include <Arduino.h>\n"
             '  digitalWrite(9, 1);\n'
             ')DOC";\n'
             "void svc_ramp_update(void) { }")})
    case("обращение Serial.print — нарушение", VIOLATION,
         {"firmware/svc_ramp.cpp": RAMP.replace(
             "void svc_ramp_update(void) { }",
             "void svc_ramp_update(void) { Serial.print(1); }")})

    # --- исключения ADR-0014 ----------------------------------------------
    case("лишний millis в HAL отвергается, печатаются все вхождения", VIOLATION,
         {"firmware/hal_atmega328p.cpp": HAL + "uint32_t extra(void) { return millis(); }\n"},
         expect_text=("допускает 1 вхождений millis, найдено 2",
                      "hal_atmega328p.cpp:10", "hal_atmega328p.cpp:12"))
    case("ATTACK исключение по заголовкам не пускает новый avr/wdt.h", VIOLATION,
         {"firmware/MICRO_UART.h":
          '#pragma once\n#include "Arduino.h"\n#include <avr/wdt.h>\n'},
         expect_text=("avr/wdt.h",))
    case("неиспользованное исключение названо вслух", CLEAN,
         {"firmware/PRINT.h": "#pragma once\nvoid printInteger(long n);\n"},
         expect_text=("БОЛЬШЕ НЕ НУЖНО: firmware/PRINT.h",))

    # --- отказ инструмента отличается от нарушения ------------------------
    case("ATTACK нет каталога firmware — отказ, а не зелёный", BROKEN,
         drop=tuple(p for p in BASE if p.startswith("firmware/")),
         expect_text=("ОТКАЗ СТОРОЖА",), reject_text=("вне исключений нет",))
    case("ATTACK пустой firmware — отказ, а не зелёный", BROKEN,
         {"firmware/.keep": ""},
         drop=tuple(p for p in BASE if p.startswith("firmware/")),
         expect_text=("ОТКАЗ СТОРОЖА",))
    case("ATTACK чужой корень — отказ, а не зелёный", BROKEN,
         {"sub/tools/ci/.keep": ""}, script_at="sub/tools/ci/check_layer_boundaries.py",
         expect_text=("ОТКАЗ СТОРОЖА",))

    case("ATTACK битая символическая ссылка — отказ, а не traceback", BROKEN,
         links=(("firmware/dangling.h", "/nonexistent/target.h"),),
         expect_text=("ОТКАЗ СТОРОЖА", "не удалось прочитать"))
    case("запуск через символическую ссылку не теряет корень", CLEAN,
         links=(("link/guard.py", os.path.join("..", "tools", "ci",
                                               "check_layer_boundaries.py")),),
         script_at="link/guard.py", expect_text=("просмотрено",))
    case("ATTACK подкаталог-ссылка внутри firmware просматривается", VIOLATION,
         {"outside/hidden.cpp": "#include <Arduino.h>\n"},
         links=(("firmware/ext", os.path.join("..", "outside")),),
         expect_text=("Arduino.h",))

    print("=" * 60)
    print("  Results: %d passed, %d failed" % (passed, failed))
    print("=" * 60)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
