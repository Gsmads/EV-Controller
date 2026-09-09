/**
 * battery.js — вертикальный бар заряда аккумулятора
 *
 * MVP-1: значения заглушены до подключения BMS (MVP-3+).
 * Покажет 0% до тех пор пока не появятся реальные данные.
 */
export class BatteryWidget {
  constructor(container, store) {
    this.container = container;
    this.store = store;
    this._build();
    this._unsubscribe = store.subscribe(() => this.render());
  }

  _build() {
    this.container.innerHTML = `
      <div class="battery-label">BATTERY</div>
      <div class="battery-bar">
        <div class="battery-fill" data-fill style="height: 0%"></div>
        <div class="battery-cells">
          <div></div><div></div><div></div><div></div>
          <div></div><div></div><div></div><div></div>
        </div>
      </div>
      <div class="battery-stats">
        <div class="battery-pct" data-pct>—%</div>
        <div class="battery-sub" data-volts>—V</div>
        <div class="battery-sub" data-amps>—A</div>
        <span class="upcoming-tag">MVP-3</span>
      </div>
    `;
    this.$fill  = this.container.querySelector('[data-fill]');
    this.$pct   = this.container.querySelector('[data-pct]');
    this.$volts = this.container.querySelector('[data-volts]');
    this.$amps  = this.container.querySelector('[data-amps]');
  }

  render() {
    // BMS будет в MVP-3+. Пока пусто.
    // Когда появится — добавить поля в store.telemetry: batteryPct, voltageV, ampsA
    const t = this.store.getTelemetry();
    const pct = t.batteryPct ?? 0;
    this.$fill.style.height = pct + '%';
    this.$pct.textContent   = pct > 0 ? `${Math.round(pct)}%` : '—%';
    this.$volts.textContent = t.voltageV   ? `${t.voltageV.toFixed(1)}V` : '—V';
    this.$amps.textContent  = t.batteryAmps ? `${t.batteryAmps.toFixed(1)}A` : '—A';
    this.$fill.classList.toggle('warn', pct > 0 && pct < 35);
    this.$fill.classList.toggle('crit', pct > 0 && pct < 20);
  }

  destroy() { this._unsubscribe?.(); }
}
