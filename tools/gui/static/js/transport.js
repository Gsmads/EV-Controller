/**
 * transport.js — абстрактный транспорт для команд и телеметрии
 *
 * Сейчас: WebSocket → Python-сервер → Serial → Arduino
 * Будущее: тот же интерфейс будет реализован для ESP32 (WebSocket напрямую)
 *
 * Все виджеты используют ТОЛЬКО эти методы, не зная про WebSocket.
 * Это позволит подменить транспорт без изменения остального кода.
 */
export class Transport extends EventTarget {
  constructor(url) {
    super();
    this.url = url;
    this.ws = null;
    this.reconnectDelay = 1000;
    this.reconnectTimer = null;
    this.isOpen = false;
  }

  connect() {
    if (this.ws) return;
    this.ws = new WebSocket(this.url);

    this.ws.addEventListener('open', () => {
      this.isOpen = true;
      this.reconnectDelay = 1000;
      this.dispatchEvent(new CustomEvent('connection', { detail: { connected: true } }));
    });

    this.ws.addEventListener('close', () => {
      this.isOpen = false;
      this.ws = null;
      this.dispatchEvent(new CustomEvent('connection', { detail: { connected: false } }));
      this.scheduleReconnect();
    });

    this.ws.addEventListener('error', (e) => {
      this.dispatchEvent(new CustomEvent('error', { detail: e }));
    });

    this.ws.addEventListener('message', (e) => {
      try {
        const msg = JSON.parse(e.data);
        this.dispatchEvent(new CustomEvent('message', { detail: msg }));
      } catch (err) {
        console.warn('Bad JSON from server:', e.data);
      }
    });
  }

  scheduleReconnect() {
    if (this.reconnectTimer) return;
    this.reconnectTimer = setTimeout(() => {
      this.reconnectTimer = null;
      this.reconnectDelay = Math.min(this.reconnectDelay * 1.5, 5000);
      this.connect();
    }, this.reconnectDelay);
  }

  send(obj) {
    if (!this.isOpen || !this.ws) return false;
    try {
      this.ws.send(JSON.stringify(obj));
      return true;
    } catch (e) {
      console.warn('Send failed:', e);
      return false;
    }
  }

  close() {
    if (this.reconnectTimer) {
      clearTimeout(this.reconnectTimer);
      this.reconnectTimer = null;
    }
    if (this.ws) this.ws.close();
  }
}
