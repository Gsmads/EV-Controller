/**
 * app.js — точка входа: собирает виджеты, подключает транспорт к store
 */
import { Transport } from './transport.js';
import { Protocol }  from './protocol.js';
import { Store }     from './store.js';

import { HeaderWidget }       from './widgets/header.js';
import { GaugeWidget }        from './widgets/gauge.js';
import { BatteryWidget }      from './widgets/battery.js';
import { ChartsWidget }       from './widgets/charts.js';
import { PedalSliderWidget }  from './widgets/pedals.js';
import { ModesWidget }        from './widgets/modes.js';
import { EStopWidget }        from './widgets/estop.js';
import {
  SourceIndicatorWidget,
  ExternalInputsWidget,
  LightsWidget,
  CommStatsWidget,
} from './widgets/panels.js';

const WS_URL = `ws://${location.hostname || 'localhost'}:8765`;

// ============== Core ==============

const store     = new Store();
const transport = new Transport(WS_URL);
const protocol  = new Protocol(transport);

// ============== Transport → Store ==============

transport.addEventListener('connection', (e) => {
  store.update({ connection: { connected: e.detail.connected } });
});

transport.addEventListener('message', (e) => {
  const msg = e.detail;
  switch (msg.type) {
    case 'telemetry':
      // Все поля телеметрии записываем в store
      store.update({ telemetry: msg });
      break;
    case 'status':
      store.update({
        connection: { connected: msg.state === 'connected', port: msg.port, error: msg.error }
      });
      break;
    case 'log':
      store.appendLog(msg.message);
      break;
    case 'ack':
      // тихо
      break;
    case 'nack':
      store.appendLog(`[NACK] cmd=0x${msg.cmd.toString(16)} err=0x${msg.err.toString(16)}`);
      break;
    case 'pong':
      store.appendLog('[PONG]');
      break;
  }
});

transport.connect();

// ============== Widgets ==============

const widgets = [];

widgets.push(new HeaderWidget(
  document.querySelector('[data-region="header"]'), store));

widgets.push(new SourceIndicatorWidget(
  document.querySelector('[data-region="source"]'), store));

widgets.push(new ExternalInputsWidget(
  document.querySelector('[data-region="inputs"]')));

widgets.push(new LightsWidget(
  document.querySelector('[data-region="lights"]')));

widgets.push(new CommStatsWidget(
  document.querySelector('[data-region="comm"]'), store));

widgets.push(new BatteryWidget(
  document.querySelector('[data-region="battery"]'), store));

widgets.push(new GaugeWidget(
  document.querySelector('[data-region="gauge"]'), store, protocol));

widgets.push(new ChartsWidget(
  document.querySelector('[data-region="charts"]'), store));

const pedalGas = new PedalSliderWidget({
  container: document.querySelector('[data-region="pedal-gas"]'),
  protocol, store, kind: 'gas'
});
widgets.push(pedalGas);

const pedalBrake = new PedalSliderWidget({
  container: document.querySelector('[data-region="pedal-brake"]'),
  protocol, store, kind: 'brake'
});
widgets.push(pedalBrake);

widgets.push(new ModesWidget(
  document.querySelector('[data-region="modes"]'), store, protocol));

widgets.push(new EStopWidget(
  document.querySelector('[data-region="estop"]'), protocol,
  [pedalGas, pedalBrake]));

console.log('EV Control Room initialized. Widgets:', widgets.length);
