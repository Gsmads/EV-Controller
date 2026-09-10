/**
 * estop.js — кнопки экстренной остановки и возврата управления педалям
 */
export class EStopWidget {
  constructor(container, protocol, pedalWidgets) {
    this.container = container;
    this.protocol = protocol;
    this.pedalWidgets = pedalWidgets;  // Чтобы анимировать ползунки
    this._build();
  }

  _build() {
    this.container.innerHTML = `
      <button class="btn-release" data-release>RELEASE<br>TO PEDALS</button>
      <button class="btn-estop" data-estop>
        <span class="estop-glow"></span>
        E·STOP
      </button>
    `;

    this.container.querySelector('[data-release]').addEventListener('click', () => {
      this.protocol.releaseControl();
      // Также сбросить локальные UI-слайдеры
      for (const w of this.pedalWidgets) {
        w.$input.value = 0;
        w.$input.dispatchEvent(new Event('input'));
      }
    });

    this.container.querySelector('[data-estop]').addEventListener('click', () => {
      this.protocol.emergencyStop();
      // Визуально установить тормоз на максимум
      const brakeWidget = this.pedalWidgets.find(w => w.kind === 'brake');
      if (brakeWidget) {
        brakeWidget._setLocal(1023);
      }
    });
  }

  destroy() {}
}
