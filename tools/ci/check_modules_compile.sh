#!/usr/bin/env bash
#
# Сторож задачи #33: каждый модуль firmware/ проверяется компилятором.
#
# До этой проверки 9 из 13 модулей не компилировались ничем. В tests/Makefile
# участвуют четыре файла (util_math, cfg_settings, util_crc, svc_ramp),
# остальные не видел ни один компилятор — ни десктопный, ни AVR. Правка в них
# проверялась только чтением.
#
# Проверяется синтаксис и разрешение имён (-fsyntax-only), а не сборка прошивки:
# десктопный g++ не знает регистров ATmega328P. Модуль, зависящий от платформы,
# и не должен здесь собираться — такие перечислены ниже поимённо.
#
# Коды возврата, как у остальных сторожей:
#   0  проверено, все обязательные модули компилируются
#   1  модуль не компилируется, хотя обязан
#   2  проверить не удалось (нет компилятора, нет каталога)
#
# ЧЕГО ЭТА ПРОВЕРКА НЕ ПОКРЫВАЕТ:
#   * сборку прошивки целиком. Нужен AVR-тулчейн и Arduino IDE; кроме того,
#     EV_Controller.ino:152 вызывает hal_uart_init(UART_BAUD_DEBUG), а такого
#     макроса в репозитории нет ни в одном заголовке;
#   * компоновку. Неразрешённая внешняя ссылка -fsyntax-only не видна;
#   * предупреждения. Идут с -Wall -Wextra, но не блокируют: firmware/ их
#     сегодня даёт достаточно, чтобы -Werror означал красный навсегда;
#   * поведение. Компилируется не значит работает.

set -uo pipefail

cd "$(dirname "$(readlink -f "$0")")/../.." || exit 2

if ! command -v g++ >/dev/null 2>&1; then
    echo "ОТКАЗ: g++ не найден, проверять нечем." >&2
    exit 2
fi
if [ ! -d firmware ]; then
    echo "ОТКАЗ: каталога firmware/ нет, проверять нечего." >&2
    exit 2
fi

FLAGS=(-std=c++11 -fsyntax-only -Ifirmware -Wall -Wextra)

# Модули, привязанные к платформе: включают Arduino.h прямо или через
# MICRO_UART.h / PRINT.h. На десктопе их собрать нельзя и не нужно —
# по ADR-0001 платформенным остаётся hal_atmega328p.cpp, остальные три
# перестанут быть таковыми вместе с задачей #31.
declare -A PLATFORM_BOUND=(
    [hal_atmega328p.cpp]="платформенный по ADR-0001, собирается только AVR-тулчейном"
    [PRINT.cpp]="Arduino.h через PRINT.h; снимается с #31"
    [app_debug.cpp]="Arduino.h косвенно через PRINT.h; снимается с #31"
)

# Модули, которые обязаны компилироваться, но сегодня не компилируются.
# Это не исключения, а зафиксированные дефекты: см. docs/AUDIT.md B-5.
# Список может только сокращаться — если модуль отсюда вдруг собрался,
# проверка падает и требует убрать запись.
#
# Сейчас список пуст: последняя запись, svc_pedals.cpp, снята вместе
# с переездом каналов АЦП в настройки (ADR-0009). Сторож потребовал этого
# сам — именно так список и должен сокращаться.
declare -A KNOWN_BROKEN=()

fail=0
ok_count=0
skipped=()
broken_still=()
broken_fixed=()

for path in firmware/*.cpp; do
    name="${path#firmware/}"

    if [ -n "${PLATFORM_BOUND[$name]:-}" ]; then
        skipped+=("$name — ${PLATFORM_BOUND[$name]}")
        continue
    fi

    if output=$(g++ "${FLAGS[@]}" "$path" 2>&1); then
        if [ -n "${KNOWN_BROKEN[$name]:-}" ]; then
            broken_fixed+=("$name")
            fail=1
        else
            ok_count=$((ok_count + 1))
        fi
        continue
    fi

    if [ -n "${KNOWN_BROKEN[$name]:-}" ]; then
        broken_still+=("$name — ${KNOWN_BROKEN[$name]}")
        continue
    fi

    fail=1
    echo "Сторож #33: модуль не компилируется — $path" >&2
    echo "$output" | grep -E "error:" | head -10 | sed 's/^/    /' >&2
    echo >&2
done

if [ ${#broken_fixed[@]} -gt 0 ]; then
    echo "Сторож #33: модуль из списка известных падений собрался:" >&2
    for name in "${broken_fixed[@]}"; do
        echo "    $name" >&2
    done
    echo "    Дефект исправлен — убрать запись из KNOWN_BROKEN в этом файле" >&2
    echo "    и из docs/AUDIT.md. Список может только сокращаться." >&2
    echo >&2
fi

if [ "$fail" -ne 0 ]; then
    echo "Правило: каждый модуль firmware/, не привязанный к платформе, должен" >&2
    echo "проходить g++ -fsyntax-only. Иначе правка в нём не проверяется ничем." >&2
    echo "Основание: issue #33." >&2
    exit 1
fi

echo "Сторож #33: компилируется модулей — $ok_count."
for line in "${skipped[@]}"; do
    echo "    пропущен: $line"
done
for line in "${broken_still[@]}"; do
    echo "    известное падение: $line"
done
exit 0
