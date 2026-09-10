/**
 * store.js — централизованное реактивное состояние
 *
 * Виджеты подписываются (subscribe) на интересующие их части состояния.
 * При обновлении из транспорта (телеметрии) подписчики получают callback.
 *
 * Принцип расширения: добавилось поле в телеметрии — оно автоматически
 * появится в store. Виджеты, использующие старые поля, не сломаются.
 */
export class Store {
  constructor(initial = {}) {
    this._state = {
      // Подключение
      connection: { connected: false, port: '', error: null },

      // Телеметрия (значения по умолчанию для пустого UI)
      telemetry: {
        gasPhysical: 0,
        gasUart: 0,
        gasEffective: 0,
        brakePhysical: 0,
        brakeUart: 0,
        brakeEffective: 0,
        targetPwm: 0,
        currentPwm: 0,
        pwmFreqHz: 0,
        speedRpmL: 0,
        speedRpmR: 0,
        speedKmhX10: 0,
        currentMaL: 0,
        currentMaR: 0,
        driveMode: 0,
        uartActiveFlags: 0,
        faults: 0,
        uptimeMs: 0,
      },

      // Локальное UI-состояние (не из контроллера)
      ui: {
        gaugeMode: 'speed',  // 'speed' или 'pwm' — что показывать в центре
        activeMode: 'ECO',   // выбранный режим
      },

      // Логи (debug-сообщения от контроллера)
      logLines: [],

      // Статистика связи
      stats: { tx: 0, rx: 0, latency: 0, crcErr: 0 },

      ...initial,
    };
    this._subscribers = new Set();
  }

  /** Получить текущее состояние (read-only) */
  get() { return this._state; }

  /** Удобный шорткат для срезов */
  getTelemetry() { return this._state.telemetry; }
  getConnection() { return this._state.connection; }
  getUi() { return this._state.ui; }

  /**
   * Обновить часть состояния (deep merge для секций).
   * Вызывает всех подписчиков.
   */
  update(patch) {
    for (const key of Object.keys(patch)) {
      if (typeof patch[key] === 'object' && patch[key] !== null && !Array.isArray(patch[key])) {
        this._state[key] = { ...this._state[key], ...patch[key] };
      } else {
        this._state[key] = patch[key];
      }
    }
    this._notify();
  }

  /**
   * Подписаться на изменения.
   * @param {Function} callback (state) => void
   * @returns функция отписки
   */
  subscribe(callback) {
    this._subscribers.add(callback);
    return () => this._subscribers.delete(callback);
  }

  /** Добавить лог-строку (с ограничением буфера) */
  appendLog(line) {
    this._state.logLines = [...this._state.logLines.slice(-99), line];
    this._notify();
  }

  _notify() {
    for (const cb of this._subscribers) {
      try { cb(this._state); }
      catch (e) { console.error('Subscriber error:', e); }
    }
  }
}
