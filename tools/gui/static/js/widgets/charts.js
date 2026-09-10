/**
 * charts.js — графики во времени (Chart.js)
 *
 * Три графика по умолчанию:
 *   1. PWM target vs current
 *   2. Pedals: gas effective + brake effective (+ tooltip с PHYS/UART/EFF)
 *   3. Speed L vs R (RPM каждого колеса)
 *
 * Добавление нового графика = новый объект в CHART_DEFS.
 */

const CHART_POINTS = 100;  // ~10 секунд при 10 Гц

const CHART_DEFS = [
  {
    id: 'pwm',
    title: 'PWM TARGET / CURRENT',
    yMin: 0, yMax: 1023,
    series: [
      { key: 'currentPwm', label: 'Current', color: '#f5a623' },
      { key: 'targetPwm',  label: 'Target',  color: '#5dc8e3', dash: [3, 3] },
    ],
  },
  {
    id: 'pedals',
    title: 'PEDAL INPUT (effective)',
    yMin: 0, yMax: 1023,
    series: [
      { key: 'gasEffective',   label: 'Gas',   color: '#4ade80' },
      { key: 'brakeEffective', label: 'Brake', color: '#ef4444' },
    ],
  },
  {
    id: 'speed',
    title: 'WHEEL RPM L / R',
    yMin: 0, yMax: 500,
    series: [
      { key: 'speedRpmL', label: 'L', color: '#b794f6' },
      { key: 'speedRpmR', label: 'R', color: '#f472b6' },
    ],
  },
];

export class ChartsWidget {
  constructor(container, store) {
    this.container = container;
    this.store = store;
    this.charts = [];
    this._build();
    this._unsubscribe = store.subscribe((s) => this._push(s));
  }

  _build() {
    this.container.innerHTML = CHART_DEFS.map(def => `
      <div class="chart-card" data-chart-id="${def.id}">
        <div class="chart-header">
          <span class="chart-title">${def.title}</span>
          <span class="chart-legend">
            ${def.series.map(s => `
              <span class="chart-legend-item">
                <span class="chart-legend-dot" style="background: ${s.color}"></span>
                <span class="mono" data-legend="${s.key}">0</span>
              </span>
            `).join('')}
          </span>
        </div>
        <div class="chart-canvas-wrap">
          <canvas data-canvas="${def.id}" role="img" aria-label="${def.title}"></canvas>
        </div>
      </div>
    `).join('');

    // Создать графики
    for (const def of CHART_DEFS) {
      const canvas = this.container.querySelector(`canvas[data-canvas="${def.id}"]`);
      const chart = new Chart(canvas.getContext('2d'), {
        type: 'line',
        data: {
          labels: Array(CHART_POINTS).fill(''),
          datasets: def.series.map(s => ({
            label: s.label,
            data: Array(CHART_POINTS).fill(null),
            borderColor: s.color,
            backgroundColor: s.color + '20',
            borderWidth: 1.5,
            tension: 0.2,
            pointRadius: 0,
            borderDash: s.dash || [],
          })),
        },
        options: {
          responsive: true,
          maintainAspectRatio: false,
          animation: false,
          plugins: { legend: { display: false } },
          scales: {
            x: { display: false, grid: { display: false } },
            y: {
              min: def.yMin, max: def.yMax,
              grid: { color: 'rgba(255,255,255,0.04)' },
              ticks: {
                color: '#5a6171',
                font: { family: 'Geist Mono', size: 9 },
                maxTicksLimit: 4,
              },
              border: { display: false },
            },
          },
          interaction: { intersect: false },
        },
      });
      this.charts.push({ def, chart });
    }
  }

  _push(state) {
    const t = state.telemetry;
    for (const { def, chart } of this.charts) {
      // Обновить данные графика
      for (let i = 0; i < def.series.length; i++) {
        const ds = chart.data.datasets[i];
        ds.data.shift();
        ds.data.push(t[def.series[i].key]);
      }
      // Авто-расширение Y для скорости (если выходит за начальный max)
      if (def.id === 'speed') {
        const maxVal = Math.max(t.speedRpmL, t.speedRpmR);
        if (maxVal > chart.options.scales.y.max) {
          chart.options.scales.y.max = maxVal * 1.2;
        }
      }
      chart.update('none');

      // Обновить легенду
      for (const s of def.series) {
        const el = this.container.querySelector(`[data-legend="${s.key}"]`);
        if (el) el.textContent = t[s.key] ?? 0;
      }
    }
  }

  destroy() {
    for (const { chart } of this.charts) chart.destroy();
    this._unsubscribe?.();
  }
}
