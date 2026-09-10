/**
 * gauge.js — центральный циферблат с двумя дугами
 *
 * Внешняя толстая АМБЕР-дуга = текущая скорость (км/ч, от энкодеров).
 *   До MVP-2 без энкодеров будет 0 — это нормально, дуга останется пустой.
 *
 * Внутренняя тонкая ГОЛУБАЯ дуга = текущий PWM.
 *
 * Видимая разница между ними показывает физическую нагрузку:
 *   - На старте: PWM большой, скорость 0 → внутренняя дуга длиннее
 *   - Установившееся движение: обе дуги ≈ одинаковые
 *   - С горки: PWM малый, скорость большая → внешняя длиннее
 *
 * Центральная цифра переключается PWM ↔ SPEED.
 * Под цифрой — мелкие индикаторы L/R для двух колёс (RPM или PWM раздельно).
 */

// 270° дуга от 135° до 405°. Длина = 2π × r × 0.75
const ARC_LEN_OUTER = 754;  // r=160 → 2π·160·0.75 ≈ 754
const ARC_LEN_INNER = 589;  // r=125 → 2π·125·0.75 ≈ 589

const MAX_SPEED_KMH = 12;   // Максимум шкалы скорости (для детского ЭВ)
const MAX_PWM = 1023;

export class GaugeWidget {
  constructor(container, store, protocol) {
    this.container = container;
    this.store = store;
    this.protocol = protocol;
    this._build();
    this._wireEvents();
    this._unsubscribe = store.subscribe(() => this.render());
    this.render();
  }

  _build() {
    this.container.innerHTML = `
      <div class="gauge-wrapper">
        <svg class="gauge-svg" viewBox="0 0 400 400">
          <!-- Track outer (speed, амбер) -->
          <circle class="gauge-track-outer" cx="200" cy="200" r="160"
                  stroke-dasharray="754 1005"
                  transform="rotate(135 200 200)" />
          <!-- Track inner (pwm, голубой) -->
          <circle class="gauge-track-inner" cx="200" cy="200" r="125"
                  stroke-dasharray="589 785"
                  transform="rotate(135 200 200)" />
          <!-- Inner PWM arc — рисуется сначала, чтобы внешняя была сверху -->
          <circle class="gauge-arc-pwm" cx="200" cy="200" r="125"
                  stroke-dasharray="589 785"
                  stroke-dashoffset="589"
                  transform="rotate(135 200 200)"
                  data-arc="pwm" />
          <!-- Outer Speed arc -->
          <circle class="gauge-arc-speed" cx="200" cy="200" r="160"
                  stroke-dasharray="754 1005"
                  stroke-dashoffset="754"
                  transform="rotate(135 200 200)"
                  data-arc="speed" />
          <g class="gauge-ticks" data-ticks></g>
        </svg>

        <div class="gauge-center">
          <div class="gauge-value-row">
            <span class="gauge-value" data-value>0</span>
          </div>
          <div class="gauge-unit" data-unit>KM / H</div>
          <div class="gauge-mode" data-mode>ECO</div>
          <div class="gauge-wheels">
            <span class="wheel-l">L <span class="mono" data-wheel-l>0</span></span>
            <span class="wheel-r">R <span class="mono" data-wheel-r>0</span></span>
          </div>
          <div class="gauge-legend">
            <span class="gauge-legend-item">
              <span class="gauge-legend-swatch current"></span>SPEED
            </span>
            <span class="gauge-legend-item">
              <span class="gauge-legend-swatch target"></span>PWM
            </span>
          </div>
        </div>

        <div class="display-toggle" data-toggle>
          <button data-mode="speed" class="active">SPEED</button>
          <button data-mode="pwm">PWM</button>
        </div>
      </div>
    `;

    this._buildTicks();

    // Кеш ссылок для быстрого доступа в render()
    this.$arcSpeed = this.container.querySelector('[data-arc="speed"]');
    this.$arcPwm   = this.container.querySelector('[data-arc="pwm"]');
    this.$value    = this.container.querySelector('[data-value]');
    this.$unit     = this.container.querySelector('[data-unit]');
    this.$mode     = this.container.querySelector('[data-mode]');
    this.$wheelL   = this.container.querySelector('[data-wheel-l]');
    this.$wheelR   = this.container.querySelector('[data-wheel-r]');
  }

  _buildTicks() {
    const g = this.container.querySelector('[data-ticks]');
    const cx = 200, cy = 200;
    const SVG_NS = 'http://www.w3.org/2000/svg';

    // Major ticks с подписями каждые 20%
    for (let i = 0; i <= 10; i++) {
      const pct = i * 10;
      const angle = (135 + (pct / 100) * 270) * Math.PI / 180;
      const x1 = cx + Math.cos(angle) * 178;
      const y1 = cy + Math.sin(angle) * 178;
      const x2 = cx + Math.cos(angle) * 168;
      const y2 = cy + Math.sin(angle) * 168;
      const tick = document.createElementNS(SVG_NS, 'line');
      tick.setAttribute('x1', x1); tick.setAttribute('y1', y1);
      tick.setAttribute('x2', x2); tick.setAttribute('y2', y2);
      tick.setAttribute('class', i % 2 === 0 ? 'gauge-tick major' : 'gauge-tick');
      g.appendChild(tick);

      if (i % 2 === 0) {
        const lx = cx + Math.cos(angle) * 152;
        const ly = cy + Math.sin(angle) * 152 + 4;
        const label = document.createElementNS(SVG_NS, 'text');
        label.setAttribute('x', lx);
        label.setAttribute('y', ly);
        label.setAttribute('text-anchor', 'middle');
        label.setAttribute('class', 'gauge-tick-label');
        label.textContent = pct;
        g.appendChild(label);
      }
    }
    // Minor ticks
    for (let i = 0; i <= 50; i++) {
      if (i % 5 === 0) continue;
      const pct = i * 2;
      const angle = (135 + (pct / 100) * 270) * Math.PI / 180;
      const x1 = cx + Math.cos(angle) * 173;
      const y1 = cy + Math.sin(angle) * 173;
      const x2 = cx + Math.cos(angle) * 168;
      const y2 = cy + Math.sin(angle) * 168;
      const tick = document.createElementNS(SVG_NS, 'line');
      tick.setAttribute('x1', x1); tick.setAttribute('y1', y1);
      tick.setAttribute('x2', x2); tick.setAttribute('y2', y2);
      tick.setAttribute('class', 'gauge-tick');
      g.appendChild(tick);
    }
  }

  _wireEvents() {
    // Переключатель PWM/SPEED
    const toggle = this.container.querySelector('[data-toggle]');
    toggle.addEventListener('click', (e) => {
      const btn = e.target.closest('button[data-mode]');
      if (!btn) return;
      toggle.querySelectorAll('button').forEach(b => b.classList.remove('active'));
      btn.classList.add('active');
      this.store.update({ ui: { gaugeMode: btn.dataset.mode } });
    });
  }

  render() {
    const t = this.store.getTelemetry();
    const ui = this.store.getUi();

    // Speed внешняя дуга, %% от MAX_SPEED
    const speedKmh = t.speedKmhX10 / 10;
    const speedPct = Math.min(100, (speedKmh / MAX_SPEED_KMH) * 100);
    this.$arcSpeed.setAttribute(
      'stroke-dashoffset',
      ARC_LEN_OUTER - (ARC_LEN_OUTER * speedPct / 100)
    );

    // PWM внутренняя дуга
    const pwmPct = Math.min(100, (t.currentPwm / MAX_PWM) * 100);
    this.$arcPwm.setAttribute(
      'stroke-dashoffset',
      ARC_LEN_INNER - (ARC_LEN_INNER * pwmPct / 100)
    );

    // Центральное значение
    if (ui.gaugeMode === 'speed') {
      this.$value.textContent = speedKmh.toFixed(1);
      this.$unit.textContent = 'KM / H';
    } else {
      this.$value.textContent = String(Math.round(t.currentPwm));
      this.$unit.textContent = 'PWM ‰';
    }

    // Режим
    this.$mode.textContent = ui.activeMode || 'ECO';

    // L/R значения под цифрой
    if (ui.gaugeMode === 'speed') {
      this.$wheelL.textContent = t.speedRpmL;
      this.$wheelR.textContent = t.speedRpmR;
    } else {
      // В режиме PWM пока показываем общий PWM (одинаковый для обоих колёс
      // до MVP-8 с электронным дифференциалом)
      this.$wheelL.textContent = Math.round(t.currentPwm);
      this.$wheelR.textContent = Math.round(t.currentPwm);
    }
  }

  destroy() {
    this._unsubscribe?.();
    this.container.innerHTML = '';
  }
}
