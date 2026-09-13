# ============================================================================
#  Сборка прошивки EV Controller под ATmega328P без ядра Arduino
#
#    make            собрать .elf/.hex и показать расход памяти
#    make size       только расход памяти
#    make flash      прошить через загрузчик (см. PORT ниже)
#    make clean      убрать результаты сборки
#
#  Требуется: gcc-avr, avr-libc, avrdude
#    Debian/Ubuntu:  sudo apt install gcc-avr avr-libc avrdude
#
#  Зачем не Arduino IDE: docs/BUILD.md
# ============================================================================

MCU      := atmega328p
F_CPU    := 16000000UL
TARGET   := ev_controller
BUILD    := build

# Порт и протокол загрузчика. Стоковый Nano - old bootloader (57600).
# После прошивки optiboot ставить 115200 (см. ADR-0022).
PORT     ?= /dev/ttyUSB0
PROGRAMMER ?= arduino
BAUD     ?= 57600

# ---------------------------------------------------------------------------
#  Флаги
#
#  -Os            оптимизация по размеру: 32 КБ флеша и 2 КБ ОЗУ
#  -ffunction-sections/-fdata-sections + --gc-sections
#                 линковщик выбрасывает то, что не вызывается
#  -fno-exceptions/-fno-rtti  на AVR не нужны и стоят килобайтов
#  -flto          межмодульная оптимизация
#  -Wall -Wextra  предупреждения как в десктопных тестах
# ---------------------------------------------------------------------------
CXXFLAGS := -mmcu=$(MCU) -DF_CPU=$(F_CPU) -Os -std=c++11 \
            -Wall -Wextra -Wundef -Wshadow \
            -ffunction-sections -fdata-sections \
            -fno-exceptions -fno-rtti -fno-threadsafe-statics \
            -flto -I.

LDFLAGS  := -mmcu=$(MCU) -Os -flto -Wl,--gc-sections -Wl,-Map,$(BUILD)/$(TARGET).map

# Скетч собирается как обычный C++: ядра Arduino нет, макрос ARDUINO не
# определён, поэтому точка входа берётся из самого .ino (см. его хвост).
SRC_CPP  := $(wildcard *.cpp)
SRC_INO  := EV_Controller.ino
OBJ      := $(addprefix $(BUILD)/,$(SRC_CPP:.cpp=.o)) $(BUILD)/EV_Controller.ino.o

.PHONY: all size flash clean help

all: $(BUILD)/$(TARGET).hex size

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/%.o: %.cpp | $(BUILD)
	$(CXX_AVR) $(CXXFLAGS) -c $< -o $@

$(BUILD)/EV_Controller.ino.o: $(SRC_INO) | $(BUILD)
	$(CXX_AVR) $(CXXFLAGS) -x c++ -c $< -o $@

$(BUILD)/$(TARGET).elf: $(OBJ)
	$(CXX_AVR) $(LDFLAGS) $^ -o $@

$(BUILD)/$(TARGET).hex: $(BUILD)/$(TARGET).elf
	avr-objcopy -O ihex -R .eeprom $< $@

CXX_AVR := avr-g++

size: $(BUILD)/$(TARGET).elf
	@echo ""
	@avr-size --format=avr --mcu=$(MCU) $<

flash: $(BUILD)/$(TARGET).hex
	avrdude -p $(MCU) -c $(PROGRAMMER) -P $(PORT) -b $(BAUD) -U flash:w:$<:i

clean:
	rm -rf $(BUILD)

help:
	@echo "make        - собрать прошивку"
	@echo "make size   - расход флеша и ОЗУ"
	@echo "make flash  - прошить (PORT=/dev/ttyUSB0 BAUD=57600)"
	@echo "make clean  - убрать результаты"
