/**
 * pedals.js — слайдеры виртуальных педалей (UART-override)
 *
 * Ключевое поведение: при отпускании ползунка он плавно возвращается к нулю
 * в течение DECAY_MS, отправляя команды на контроллер по пути.
 * Это даёт ощущение настоящей педали и не создаёт рывков мотора.
 *
 * Команды отправляются с троттлингом TROTTLE_MS чтобы не перегружать UART
 * (контроллер всё равно обработает только последнее значение в момент чтения).
 *
 * Watchdog svc_pedals на стороне Arduino дополнительно сбросит виртуальную
 * педаль в 0 если команды перестали приходить (потеря связи).
 */

const DECAY_MS    = 300;   // длительность плавного возврата к 0
const THROTTLE_MS = 50;    // отправлять команду не чаще

export class PedalSliderWidget {
  /**
   * @param {Object} options
   * @param {HTMLElement} options.container  Корневой элемент со слайдером
   * @param {Object}      options.protocol   Protocol instance
   * @param {Object}      options.store      Store instance
   * @param {String}      options.kind       'gas' или 'brake'
   */
  constructor({ container, protocol, store, kind }) {
    this.container = container;
    this.protocol = protocol;
    this.store = store;
    this.kind = kind;
    this.lastSendMs = 0;
    this.decayHandle = null;
    this._build();
    this._wireEvents();
    this._unsubscribe = store.subscribe(() => this.renderTelemetry());
  }

  _build() {
    const label = this.kind.toUpperCase();
    this.container.innerHTML = `
      <div class="slider-row ${this.kind}">
        <span class="slider-label">${label}</span>
        <div class="slider">
          <div class="slider-track">
            <div class="slider-fill" data-fill></div>
            <div class="slider-thumb" data-thumb></div>
          </div>
          <input type="range" min="0" max="1023" value="0" data-input>
        </div>
        <div class="slider-value">
          <span data-value class="mono">0000</span>
          <span class="max">/1023</span>
        </div>
      </div>
      <div class="slider-sources mono" data-sources>
        <span class="src-tag physical">PHYS <span data-phys>0</span></span>
        <span class="src-tag uart">UART <span data-uart>0</span></span>
        <span class="src-tag effective">EFF <span data-eff>0</span></span>
      </div>
    `;
    this.$input = this.container.querySelector('[data-input]');
    this.$fill  = this.container.querySelector('[data-fill]');
    this.$thumb = this.container.querySelector('[data-thumb]');
    this.$value = this.container.querySelector('[data-value]');
    this.$phys  = this.container.querySelector('[data-phys]');
    this.$uart  = this.container.querySelector('[data-uart]');
    this.$eff   = this.container.querySelector('[data-eff]');
  }

  _wireEvents() {
    this.$input.addEventListener('input', () => {
      this._cancelDecay();
      const v = parseInt(this.$input.value);
      this._setLocal(v);
      this._sendThrottled(v);
    });

    // pointerup ловит и mouse и touch
    this.$input.addEventListener('pointerup', () => this._startDecay());
    this.$input.addEventListener('mouseleave', (e) => {
      // Если пользователь продолжает удерживать кнопку и ушёл с ползунка,
      // ничего не делаем
      if (e.buttons === 0) this._startDecay();
    });
  }

  /**
   * Плавное возвращение к нулю с отправкой команд по пути.
   * После завершения отправляется releaseControl как финализатор.
   */
  _startDecay() {
    const start = parseInt(this.$input.value);
    if (start === 0) return;

    this._cancelDecay();
    const startTime = performance.now();

    const step = () => {
      const elapsed = performance.now() - startTime;
      const t = Math.min(1, elapsed / DECAY_MS);
      // Ease-out квадратичная
      const ease = 1 - (1 - t) * (1 - t);
      const v = Math.round(start * (1 - ease));

      this._setLocal(v);
      this._sendThrottled(v, true);  // force send

      if (t < 1) {
        this.decayHandle = requestAnimationFrame(step);
      } else {
        this.decayHandle = null;
        // Финальный 0
        this._setLocal(0);
        this._send(0, true);
      }
    };
    this.decayHandle = requestAnimationFrame(step);
  }

  _cancelDecay() {
    if (this.decayHandle) {
      cancelAnimationFrame(this.decayHandle);
      this.decayHandle = null;
    }
  }

  _setLocal(v) {
    this.$input.value = v;
    const pct = (v / 1023) * 100;
    this.$fill.style.width = pct + '%';
    this.$thumb.style.left = pct + '%';
    this.$value.textContent = String(v).padStart(4, '0');
  }

  _sendThrottled(v, force = false) {
    const now = performance.now();
    if (!force && (now - this.lastSendMs) < THROTTLE_MS) return;
    this.lastSendMs = now;
    this._send(v);
  }

  _send(v, isLast = false) {
    if (this.kind === 'gas')   this.protocol.setGasVirtual(v);
    if (this.kind === 'brake') this.protocol.setBrakeVirtual(v);
  }

  /** Обновить индикаторы physical/uart/effective из телеметрии */
  renderTelemetry() {
    const t = this.store.getTelemetry();
    if (this.kind === 'gas') {
      this.$phys.textContent = t.gasPhysical;
      this.$uart.textContent = t.gasUart;
      this.$eff.textContent  = t.gasEffective;
    } else {
      this.$phys.textContent = t.brakePhysical;
      this.$uart.textContent = t.brakeUart;
      this.$eff.textContent  = t.brakeEffective;
    }
  }

  destroy() {
    this._cancelDecay();
    this._unsubscribe?.();
  }
}
