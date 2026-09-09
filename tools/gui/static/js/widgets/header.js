/**
 * header.js — верхняя строка статуса (порт, uptime, PWM-частота, ошибки)
 */
export class HeaderWidget {
  constructor(container, store) {
    this.container = container;
    this.store = store;
    this._build();
    this._unsubscribe = store.subscribe(() => this.render());
  }

  _build() {
    this.container.innerHTML = `
      <div class="brand">
        <div class="brand-mark"></div>
        EV CONTROL ROOM
      </div>
      <nav class="tabs">
        <button class="tab active">Dashboard</button>
        <button class="tab">Settings &amp; Console</button>
      </nav>
      <div class="status-cluster">
        <span class="status-port">
          <span class="status-dot" data-conn-dot></span>
          <span class="label">PORT</span>
          <span class="value mono" data-port>—</span>
        </span>
        <span><span class="label">UPTIME</span><span class="value mono" data-uptime>00:00:00</span></span>
        <span><span class="label">PWM</span><span class="value mono" data-freq>— Hz</span></span>
        <span class="faults-badge" data-faults>FAULTS · <span class="mono" data-faults-count>0</span></span>
      </div>
    `;
    this.$dot       = this.container.querySelector('[data-conn-dot]');
    this.$port      = this.container.querySelector('[data-port]');
    this.$uptime    = this.container.querySelector('[data-uptime]');
    this.$freq      = this.container.querySelector('[data-freq]');
    this.$faults    = this.container.querySelector('[data-faults]');
    this.$faultsCnt = this.container.querySelector('[data-faults-count]');
  }

  render() {
    const s = this.store.get();
    const c = s.connection;
    const t = s.telemetry;

    this.$dot.classList.toggle('disconnected', !c.connected);
    this.$port.textContent = c.port || '—';

    const secs = Math.floor(t.uptimeMs / 1000);
    const h = String(Math.floor(secs / 3600)).padStart(2, '0');
    const m = String(Math.floor(secs / 60) % 60).padStart(2, '0');
    const sec = String(secs % 60).padStart(2, '0');
    this.$uptime.textContent = `${h}:${m}:${sec}`;

    this.$freq.textContent = t.pwmFreqHz ? `${t.pwmFreqHz} Hz` : '— Hz';

    // Счёт активных faults
    let count = 0;
    let f = t.faults;
    while (f) { count += f & 1; f >>= 1; }
    this.$faultsCnt.textContent = count;
    this.$faults.classList.toggle('has-faults', count > 0);
  }

  destroy() { this._unsubscribe?.(); }
}
