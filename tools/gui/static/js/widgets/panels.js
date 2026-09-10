/**
 * panels.js — статические виджеты левой колонки
 *
 * - SourceIndicator: показывает какие источники активны (physical/uart)
 * - ExternalInputs:  заглушка под PCA9535 кнопки (MVP-3+)
 * - Lights:          заглушка под ESP32-управление светом (MVP-2+)
 * - CommStats:       статистика связи
 */

export class SourceIndicatorWidget {
  constructor(container, store) {
    this.container = container;
    this.store = store;
    this._build();
    this._unsubscribe = store.subscribe(() => this.render());
  }

  _build() {
    this.container.innerHTML = `
      <div class="source-indicator">
        <div class="source-row">
          <span class="source-label">GAS</span>
          <span class="source-tag" data-tag="gas-phys">PHYS</span>
          <span class="source-tag" data-tag="gas-uart">UART</span>
        </div>
        <div class="source-row">
          <span class="source-label">BRAKE</span>
          <span class="source-tag" data-tag="brake-phys">PHYS</span>
          <span class="source-tag" data-tag="brake-uart">UART</span>
        </div>
      </div>
    `;
  }

  render() {
    const t = this.store.getTelemetry();
    const tags = {
      'gas-phys':   t.gasPhysical > 0,
      'gas-uart':   t.gasUart > 0,
      'brake-phys': t.brakePhysical > 0,
      'brake-uart': t.brakeUart > 0,
    };
    for (const [key, active] of Object.entries(tags)) {
      const el = this.container.querySelector(`[data-tag="${key}"]`);
      if (el) el.classList.toggle('active', active);
    }
  }

  destroy() { this._unsubscribe?.(); }
}


export class ExternalInputsWidget {
  constructor(container) {
    this.container = container;
    this._build();
    // MVP-3+: подписаться на телеметрию externalInputs
  }

  _build() {
    const inputs = [
      ['ignition',  'Ignition',  true],
      ['neutral',   'Neutral',   false],
      ['handbrake', 'Handbrake', false],
      ['eco',       'Eco',       true],
      ['sport',     'Sport',     false],
      ['reverse',   'Reverse',   false],
      ['turnL',     'Turn L',    false],
      ['turnR',     'Turn R',    false],
    ];
    this.container.innerHTML = inputs.map(([key, label, on]) => `
      <div class="input-row ${on ? 'active' : ''}" data-input="${key}">
        <span class="led ${on ? 'on' : ''}"></span>${label}
      </div>
    `).join('');
  }

  destroy() {}
}


export class LightsWidget {
  constructor(container) {
    this.container = container;
    this._build();
  }

  _build() {
    const lights = [
      ['head',  'HEAD',   false, ''],
      ['tail',  'TAIL',   false, ''],
      ['brake', 'BRAKE',  false, 'brake-on'],
      ['rev',   'REV',    false, ''],
      ['turnL', 'TURN L', false, ''],
      ['turnR', 'TURN R', false, ''],
    ];
    this.container.innerHTML = lights.map(([key, label, on, cls]) => `
      <div class="light-cell ${on ? 'on ' + cls : ''}" data-light="${key}">
        <div class="icon">${on ? '◉' : '◯'}</div>
        <span>${label}</span>
        <span class="state">${on ? 'ON' : 'OFF'}</span>
      </div>
    `).join('');
  }

  destroy() {}
}


export class CommStatsWidget {
  constructor(container, store) {
    this.container = container;
    this.store = store;
    this.rxCount = 0;
    this.lastRxSec = Math.floor(Date.now() / 1000);
    this.rxRate = 0;

    this._build();
    this._unsubscribe = store.subscribe((s) => {
      // Считаем приёмы для оценки rate
      this.rxCount++;
      const sec = Math.floor(Date.now() / 1000);
      if (sec !== this.lastRxSec) {
        this.rxRate = this.rxCount;
        this.rxCount = 0;
        this.lastRxSec = sec;
      }
      this.render();
    });
  }

  _build() {
    this.container.innerHTML = `
      <div class="stat-row"><span class="key">RX rate</span><span class="val" data-rx>0 / s</span></div>
      <div class="stat-row"><span class="key">CRC errors</span><span class="val" data-crc>0</span></div>
      <div class="stat-row"><span class="key">Connection</span><span class="val" data-conn>—</span></div>
      <div class="stat-row"><span class="key">Baud</span><span class="val">9600</span></div>
    `;
    this.$rx = this.container.querySelector('[data-rx]');
    this.$crc = this.container.querySelector('[data-crc]');
    this.$conn = this.container.querySelector('[data-conn]');
  }

  render() {
    const s = this.store.get();
    this.$rx.textContent = `${this.rxRate} / s`;
    this.$crc.textContent = s.stats.crcErr;
    this.$conn.textContent = s.connection.connected ? 'online' : 'offline';
    this.$conn.style.color = s.connection.connected ? 'var(--accent-success)' : 'var(--accent-danger)';
  }

  destroy() { this._unsubscribe?.(); }
}
