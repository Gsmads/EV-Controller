#!/usr/bin/env python3
"""
EV Controller — WebSocket Server

Мост между Serial-портом контроллера (Arduino Nano через USB-UART)
и веб-интерфейсом в браузере.

Архитектура:

    ┌──────────────┐   USB-UART    ┌──────────┐   WebSocket   ┌─────────┐
    │  Arduino     │ ◀══════════▶  │  server  │ ◀══════════▶  │ browser │
    │  Nano        │  9600 baud    │  (this)  │  port 8765    │  (JS)   │
    │  binary proto│               │          │  JSON proto   │         │
    └──────────────┘               └──────────┘               └─────────┘

Сервер:
- Открывает Serial-порт и WebSocket (порт 8765)
- Парсит бинарные пакеты от Arduino (CRC16-CCITT)
- Конвертирует телеметрию в JSON, отправляет всем подключённым клиентам
- Принимает JSON-команды от клиентов, конвертирует в бинарные пакеты,
  отправляет Arduino
- Отслеживает соединение, переподключается при потере

Зависимости:  pip install pyserial websockets

Запуск:
    python server.py                          # автопоиск порта
    python server.py --port /dev/ttyUSB0      # явный порт
    python server.py --port COM3 --baud 9600  # Windows
    python server.py --ws-port 8765           # другой порт WebSocket
"""

import argparse
import asyncio
import json
import logging
import struct
import sys
import time
from pathlib import Path
from typing import Optional, Set

try:
    import serial
    import serial.tools.list_ports
    import websockets
    from websockets.server import WebSocketServerProtocol
except ImportError:
    print("ERROR: установите зависимости: pip install pyserial websockets")
    sys.exit(1)

# ============================================================================
#  Константы протокола (должны совпадать с app_protocol.h)
# ============================================================================

PROTO_SYNC = 0xAA
PROTO_MAX_PAYLOAD = 60

# Команды Host → Controller
CMD_SET_GAS_VIRTUAL    = 0x01
CMD_SET_BRAKE_VIRTUAL  = 0x02
CMD_SET_MODE           = 0x03
CMD_EMERGENCY_STOP     = 0x04
CMD_RELEASE_CTRL       = 0x05
CMD_SET_PARAM          = 0x10
CMD_SAVE_SETTINGS      = 0x11
CMD_RESET_DEFAULTS     = 0x12
CMD_RESET_ODOMETER     = 0x13
CMD_GET_TELEMETRY      = 0x20
CMD_SET_TELEM_RATE     = 0x21
CMD_PING               = 0xFE

# Ответы Controller → Host
RSP_ACK       = 0x80
RSP_NACK      = 0x81
RSP_TELEMETRY = 0x82
RSP_PONG      = 0x83

# Размер telemetry_packet_t (36 байт) — должен совпадать с app_protocol.h
TELEMETRY_SIZE = 36

# Маппинг JSON-команд (от веба) в CMD-байты
COMMAND_MAP = {
    'setGasVirtual':   CMD_SET_GAS_VIRTUAL,
    'setBrakeVirtual': CMD_SET_BRAKE_VIRTUAL,
    'setMode':         CMD_SET_MODE,
    'emergencyStop':   CMD_EMERGENCY_STOP,
    'releaseControl':  CMD_RELEASE_CTRL,
    'setParam':        CMD_SET_PARAM,
    'saveSettings':    CMD_SAVE_SETTINGS,
    'resetDefaults':   CMD_RESET_DEFAULTS,
    'resetOdometer':   CMD_RESET_ODOMETER,
    'getTelemetry':    CMD_GET_TELEMETRY,
    'ping':            CMD_PING,
}

# ============================================================================
#  CRC16-CCITT (должна совпадать с util_crc.cpp)
# ============================================================================

def crc16_ccitt(data: bytes, init: int = 0xFFFF) -> int:
    crc = init
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if (crc & 0x8000) else (crc << 1)
            crc &= 0xFFFF
    return crc

# ============================================================================
#  Сборка и разбор пакетов
# ============================================================================

def build_packet(cmd: int, payload: bytes = b"") -> bytes:
    """Построить бинарный пакет [SYNC][LEN][CMD][PAYLOAD][CRC_L][CRC_H]"""
    length = 1 + len(payload)
    data = bytes([length, cmd]) + payload
    crc = crc16_ccitt(data)
    return bytes([PROTO_SYNC]) + data + struct.pack("<H", crc)


def parse_telemetry(payload: bytes) -> Optional[dict]:
    """Распарсить telemetry_packet_t v2 (36 байт) в JSON-словарь."""
    if len(payload) < TELEMETRY_SIZE:
        return None
    # Формат должен точно соответствовать struct в app_protocol.h:
    #   uint16 ×13, uint8 ×2, uint16, uint32
    #   gas_phys, gas_uart, gas_eff,
    #   brake_phys, brake_uart, brake_eff,
    #   target_pwm, current_pwm, pwm_freq,
    #   rpm_l, rpm_r, kmh_x10,
    #   current_ma_l, current_ma_r,
    #   mode (u8), uart_flags (u8), faults (u16), uptime (u32)
    fields = struct.unpack("<HHHHHHHHHHHHHHBBHI", payload[:TELEMETRY_SIZE])
    return {
        "type":              "telemetry",
        "gasPhysical":       fields[0],
        "gasUart":           fields[1],
        "gasEffective":      fields[2],
        "brakePhysical":     fields[3],
        "brakeUart":         fields[4],
        "brakeEffective":    fields[5],
        "targetPwm":         fields[6],
        "currentPwm":        fields[7],
        "pwmFreqHz":         fields[8],
        "speedRpmL":         fields[9],
        "speedRpmR":         fields[10],
        "speedKmhX10":       fields[11],
        "currentMaL":        fields[12],
        "currentMaR":        fields[13],
        "driveMode":         fields[14],
        "uartActiveFlags":   fields[15],
        "faults":            fields[16],
        "uptimeMs":          fields[17],
    }

# ============================================================================
#  Парсер входящего потока от Arduino
# ============================================================================

class PacketParser:
    """Конечный автомат разбора бинарных пакетов от контроллера."""

    STATE_IDLE   = 0
    STATE_LEN    = 1
    STATE_DATA   = 2
    STATE_CRC_L  = 3
    STATE_CRC_H  = 4

    def __init__(self):
        self.state = self.STATE_IDLE
        self.pkt_len = 0
        self.pkt_data = bytearray()
        self.crc_recv = 0
        self.text_buf = bytearray()
        self.stats = {'tx': 0, 'rx': 0, 'crc_err': 0}

    def feed(self, byte: int):
        """Скормить парсеру один байт. Возвращает (kind, payload) либо None."""
        if self.state == self.STATE_IDLE:
            if byte == PROTO_SYNC:
                # Сохраняем накопленный текст (debug-вывод от Arduino)
                text = None
                if self.text_buf:
                    text = self.text_buf.decode("ascii", errors="replace").strip()
                    self.text_buf.clear()
                self.state = self.STATE_LEN
                if text:
                    return ('text', text)
            else:
                self.text_buf.append(byte)
                if byte == 0x0A:  # newline
                    text = self.text_buf.decode("ascii", errors="replace").strip()
                    self.text_buf.clear()
                    if text:
                        return ('text', text)

        elif self.state == self.STATE_LEN:
            if byte == 0 or byte > PROTO_MAX_PAYLOAD + 1:
                self.state = self.STATE_IDLE
            else:
                self.pkt_len = byte
                self.pkt_data = bytearray()
                self.state = self.STATE_DATA

        elif self.state == self.STATE_DATA:
            self.pkt_data.append(byte)
            if len(self.pkt_data) >= self.pkt_len:
                self.state = self.STATE_CRC_L

        elif self.state == self.STATE_CRC_L:
            self.crc_recv = byte
            self.state = self.STATE_CRC_H

        elif self.state == self.STATE_CRC_H:
            self.crc_recv |= byte << 8
            crc_data = bytes([self.pkt_len]) + bytes(self.pkt_data)
            crc_calc = crc16_ccitt(crc_data)
            self.state = self.STATE_IDLE
            self.stats['rx'] += 1
            if crc_calc == self.crc_recv:
                return ('packet', bytes(self.pkt_data))
            else:
                self.stats['crc_err'] += 1

        return None

# ============================================================================
#  Главный сервер
# ============================================================================

class EVServer:
    def __init__(self, port: str, baud: int):
        self.port_name = port
        self.baud = baud
        self.ser: Optional[serial.Serial] = None
        self.parser = PacketParser()
        self.clients: Set[WebSocketServerProtocol] = set()
        self.last_telemetry: Optional[dict] = None
        self.log = logging.getLogger("ev")

    # ---- Serial ----

    async def open_serial(self):
        """Открыть Serial-порт с переподключением."""
        while True:
            try:
                self.ser = serial.Serial(
                    self.port_name, self.baud, timeout=0.05, write_timeout=0.1)
                self.log.info(f"Serial opened: {self.port_name} @ {self.baud}")
                await self.broadcast_status('connected')
                return
            except Exception as e:
                self.log.warning(f"Serial open failed: {e}, retrying in 2s...")
                await self.broadcast_status('disconnected', str(e))
                await asyncio.sleep(2)

    async def serial_read_loop(self):
        """Фоновая задача: читает Serial, парсит пакеты, рассылает клиентам."""
        await self.open_serial()
        while True:
            try:
                # Неблокирующее чтение через asyncio
                raw = await asyncio.get_event_loop().run_in_executor(
                    None, lambda: self.ser.read(64) if self.ser else b"")
                if not raw:
                    await asyncio.sleep(0.01)
                    continue

                for byte in raw:
                    result = self.parser.feed(byte)
                    if result is None:
                        continue
                    kind, data = result
                    if kind == 'text':
                        await self.broadcast({'type': 'log', 'message': data})
                    elif kind == 'packet':
                        await self.handle_packet(data)

            except (serial.SerialException, OSError) as e:
                self.log.error(f"Serial error: {e}, reconnecting...")
                if self.ser:
                    try: self.ser.close()
                    except Exception: pass
                self.ser = None
                await self.broadcast_status('disconnected', str(e))
                await asyncio.sleep(1)
                await self.open_serial()

    async def handle_packet(self, data: bytes):
        """Разобрать пакет от контроллера и переправить веб-клиентам."""
        if len(data) < 1:
            return
        cmd = data[0]
        payload = data[1:]

        if cmd == RSP_TELEMETRY:
            telem = parse_telemetry(payload)
            if telem:
                self.last_telemetry = telem
                await self.broadcast(telem)
        elif cmd == RSP_ACK and len(payload) >= 1:
            await self.broadcast({'type': 'ack', 'cmd': payload[0]})
        elif cmd == RSP_NACK and len(payload) >= 2:
            await self.broadcast({'type': 'nack', 'cmd': payload[0], 'err': payload[1]})
        elif cmd == RSP_PONG:
            await self.broadcast({'type': 'pong'})

    # ---- WebSocket ----

    async def handle_client(self, ws: WebSocketServerProtocol):
        """Обработчик одного веб-клиента."""
        self.clients.add(ws)
        self.log.info(f"Client connected: {ws.remote_address}, total: {len(self.clients)}")

        # Отправить статус подключения сразу
        await ws.send(json.dumps({
            'type': 'status',
            'state': 'connected' if self.ser else 'disconnected',
            'port': self.port_name,
            'baud': self.baud,
        }))
        # И последнюю телеметрию (если есть)
        if self.last_telemetry:
            await ws.send(json.dumps(self.last_telemetry))

        try:
            async for raw_msg in ws:
                try:
                    msg = json.loads(raw_msg)
                    await self.handle_client_message(msg)
                except json.JSONDecodeError:
                    self.log.warning(f"Bad JSON from client: {raw_msg[:80]}")
                except Exception as e:
                    self.log.warning(f"Error handling client msg: {e}")
        except websockets.exceptions.ConnectionClosed:
            pass
        finally:
            self.clients.discard(ws)
            self.log.info(f"Client disconnected, total: {len(self.clients)}")

    async def handle_client_message(self, msg: dict):
        """Команда от веб-клиента → бинарный пакет на Arduino."""
        cmd_name = msg.get('cmd')
        if not cmd_name or cmd_name not in COMMAND_MAP:
            self.log.warning(f"Unknown command: {cmd_name}")
            return

        cmd = COMMAND_MAP[cmd_name]
        payload = self.build_payload(cmd_name, msg)
        packet = build_packet(cmd, payload)

        if self.ser:
            try:
                self.ser.write(packet)
                self.parser.stats['tx'] += 1
            except Exception as e:
                self.log.error(f"Serial write failed: {e}")

    def build_payload(self, cmd_name: str, msg: dict) -> bytes:
        """Сформировать payload для команды на основе JSON."""
        if cmd_name in ('setGasVirtual', 'setBrakeVirtual'):
            v = int(msg.get('value', 0))
            v = max(0, min(1023, v))
            return struct.pack("<H", v)

        if cmd_name == 'setMode':
            return bytes([int(msg.get('mode', 0)) & 0xFF])

        if cmd_name == 'setParam':
            offset = int(msg['offset'])
            size = int(msg['size'])
            value = int(msg['value'])
            if size == 1:
                data = struct.pack("<B", value & 0xFF)
            elif size == 2:
                data = struct.pack("<H", value & 0xFFFF)
            else:
                data = struct.pack("<I", value & 0xFFFFFFFF)[:size]
            return struct.pack("<HB", offset, size) + data

        if cmd_name == 'setTelemRate':
            return bytes([int(msg.get('hz', 10)) & 0xFF])

        # Все остальные команды без payload
        return b""

    # ---- Broadcast ----

    async def broadcast(self, msg: dict):
        """Отправить JSON-сообщение всем подключённым клиентам."""
        if not self.clients:
            return
        data = json.dumps(msg)
        # Параллельная отправка всем
        await asyncio.gather(
            *(ws.send(data) for ws in self.clients),
            return_exceptions=True
        )

    async def broadcast_status(self, state: str, error: str = None):
        await self.broadcast({
            'type': 'status',
            'state': state,
            'port': self.port_name,
            'error': error,
        })

    # ---- Main ----

    async def run(self, ws_host: str, ws_port: int, http_port: int):
        # Запускаем Serial-цикл в фоне
        serial_task = asyncio.create_task(self.serial_read_loop())

        # Запускаем простой HTTP-сервер для статических файлов
        http_task = asyncio.create_task(self.run_http_server(ws_host, http_port))

        # Запускаем WebSocket-сервер
        async with websockets.serve(self.handle_client, ws_host, ws_port):
            self.log.info(f"WebSocket listening on ws://{ws_host}:{ws_port}")
            self.log.info(f"GUI:    http://{ws_host}:{http_port}/")
            await asyncio.gather(serial_task, http_task)

    async def run_http_server(self, host: str, port: int):
        """Простой HTTP-сервер для отдачи static/ файлов (без зависимостей)."""
        import http.server
        import functools
        static_dir = Path(__file__).parent / 'static'
        if not static_dir.exists():
            self.log.warning(f"static/ directory not found at {static_dir}")
            return

        handler = functools.partial(http.server.SimpleHTTPRequestHandler,
                                     directory=str(static_dir))
        loop = asyncio.get_event_loop()

        def serve():
            with http.server.ThreadingHTTPServer((host, port), handler) as httpd:
                httpd.serve_forever()

        await loop.run_in_executor(None, serve)

# ============================================================================
#  Утилиты
# ============================================================================

def find_arduino_port() -> Optional[str]:
    """Найти Arduino Nano на COM/USB портах."""
    for p in serial.tools.list_ports.comports():
        desc = (p.description or "").lower()
        vid = p.vid or 0
        if vid in (0x1A86, 0x0403, 0x2341) or "ch340" in desc or "arduino" in desc:
            return p.device
    ports = list(serial.tools.list_ports.comports())
    return ports[0].device if ports else None

# ============================================================================
#  Точка входа
# ============================================================================

def main():
    parser = argparse.ArgumentParser(
        description="EV Controller WebSocket server (Serial ↔ WebSocket bridge)")
    parser.add_argument("--port", "-p", type=str, default=None,
                        help="Serial-порт (автопоиск если не указан)")
    parser.add_argument("--baud", "-b", type=int, default=9600,
                        help="Скорость Serial (по умолчанию 9600)")
    parser.add_argument("--ws-host", type=str, default="localhost",
                        help="Хост WebSocket-сервера")
    parser.add_argument("--ws-port", type=int, default=8765,
                        help="Порт WebSocket-сервера")
    parser.add_argument("--http-port", type=int, default=8000,
                        help="Порт HTTP-сервера (для статических файлов GUI)")
    parser.add_argument("--verbose", "-v", action="store_true",
                        help="Подробный лог")
    args = parser.parse_args()

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s [%(levelname)s] %(message)s",
        datefmt="%H:%M:%S",
    )

    port = args.port or find_arduino_port()
    if not port:
        print("ERROR: Arduino не найден. Укажите --port явно.")
        sys.exit(1)

    server = EVServer(port, args.baud)
    try:
        asyncio.run(server.run(args.ws_host, args.ws_port, args.http_port))
    except KeyboardInterrupt:
        print("\nServer stopped.")


if __name__ == "__main__":
    main()
