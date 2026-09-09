/**
 * protocol.js — высокоуровневые команды контроллеру
 *
 * Виджеты вызывают protocol.setGasVirtual(500) и не задумываются о JSON,
 * именах команд, нумерации байтов и т.д. При смене транспорта или формата
 * протокола меняется только этот файл.
 */

export class Protocol {
  constructor(transport) {
    this.transport = transport;
  }

  // ====== Управление педалями (UART-override) ======

  setGasVirtual(value) {
    this.transport.send({ cmd: 'setGasVirtual', value: Math.max(0, Math.min(1023, value)) });
  }

  setBrakeVirtual(value) {
    this.transport.send({ cmd: 'setBrakeVirtual', value: Math.max(0, Math.min(1023, value)) });
  }

  releaseControl() {
    this.transport.send({ cmd: 'releaseControl' });
  }

  emergencyStop() {
    this.transport.send({ cmd: 'emergencyStop' });
  }

  // ====== Режимы вождения ======

  setMode(modeId) {
    this.transport.send({ cmd: 'setMode', mode: modeId });
  }

  // ====== Настройки EEPROM ======

  setParam(offset, value, size = 2) {
    this.transport.send({ cmd: 'setParam', offset, value, size });
  }

  saveSettings() {
    this.transport.send({ cmd: 'saveSettings' });
  }

  resetDefaults() {
    this.transport.send({ cmd: 'resetDefaults' });
  }

  resetOdometer() {
    this.transport.send({ cmd: 'resetOdometer' });
  }

  // ====== Диагностика ======

  ping() {
    this.transport.send({ cmd: 'ping' });
  }

  requestTelemetry() {
    this.transport.send({ cmd: 'getTelemetry' });
  }

  setTelemetryRate(hz) {
    this.transport.send({ cmd: 'setTelemRate', hz: Math.max(0, Math.min(50, hz)) });
  }
}

// Идентификаторы режимов вождения (из cfg_settings.h drive_mode_id_t)
export const DRIVE_MODE = {
  LOCKED:    0,
  NEUTRAL:   1,
  HANDBRAKE: 2,
  ECO:       3,
  NORMAL:    4,
  SPORT:     5,
  REVERSE:   6,
  PARENT:    7,
  FAILSAFE:  8,
};

export const DRIVE_MODE_NAMES = {
  0: 'LOCKED',
  1: 'NEUTRAL',
  2: 'HANDBRAKE',
  3: 'ECO',
  4: 'NORMAL',
  5: 'SPORT',
  6: 'REVERSE',
  7: 'PARENT',
  8: 'FAILSAFE',
};
