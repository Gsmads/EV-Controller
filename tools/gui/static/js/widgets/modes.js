/**
 * modes.js — кнопки выбора режима вождения
 */
import { DRIVE_MODE } from '../protocol.js';

const MODES = [
  { id: DRIVE_MODE.LOCKED,    label: 'LCK', name: 'LOCKED'    },
  { id: DRIVE_MODE.NEUTRAL,   label: 'NEU', name: 'NEUTRAL'   },
  { id: DRIVE_MODE.HANDBRAKE, label: 'HBR', name: 'HANDBRAKE' },
  { id: DRIVE_MODE.ECO,       label: 'ECO', name: 'ECO',     active: true },
  { id: DRIVE_MODE.NORMAL,    label: 'NRM', name: 'NORMAL'   },
  { id: DRIVE_MODE.SPORT,     label: 'SPT', name: 'SPORT'    },
  { id: DRIVE_MODE.REVERSE,   label: 'REV', name: 'REVERSE'  },
  { id: DRIVE_MODE.PARENT,    label: 'PRT', name: 'PARENT'   },
];

export class ModesWidget {
  constructor(container, store, protocol) {
    this.container = container;
    this.store = store;
    this.protocol = protocol;
    this._build();
  }

  _build() {
    this.container.innerHTML = MODES.map(m => `
      <button class="mode-btn ${m.active ? 'active' : ''}"
              data-mode-id="${m.id}" data-mode-name="${m.name}">
        ${m.label}
      </button>
    `).join('');

    this.container.addEventListener('click', (e) => {
      const btn = e.target.closest('.mode-btn');
      if (!btn) return;
      const id = parseInt(btn.dataset.modeId);
      const name = btn.dataset.modeName;
      this.container.querySelectorAll('.mode-btn').forEach(b => b.classList.remove('active'));
      btn.classList.add('active');
      this.store.update({ ui: { activeMode: name } });
      this.protocol.setMode(id);
    });
  }

  destroy() {}
}
