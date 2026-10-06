"use strict";

/*
 * Inside the Employees Access Android app, Bluetooth comes from the app
 * (window.AndroidBle) because Android's WebView has no Web Bluetooth.
 * This file wraps it as navigator.bluetooth so app.js works unchanged.
 * In a normal browser it does nothing.
 */
(function () {
  if (!window.AndroidBle) return;

  const pending = new Map();
  const devices = new Map();          // address -> BleDevice
  const characteristics = new Map();  // "address|uuid" -> BleCharacteristic
  let nextId = 1;

  function call(method, ...args) {
    return new Promise((resolve, reject) => {
      const id = nextId++;
      pending.set(id, { resolve, reject });
      window.AndroidBle[method](id, ...args);
    });
  }

  function toBase64(data) {
    const bytes = data instanceof ArrayBuffer ? new Uint8Array(data)
      : new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
    let text = "";
    for (const b of bytes) text += String.fromCharCode(b);
    return btoa(text);
  }

  function toDataView(base64) {
    const text = atob(base64);
    const bytes = new Uint8Array(text.length);
    for (let i = 0; i < text.length; i++) bytes[i] = text.charCodeAt(i);
    return new DataView(bytes.buffer);
  }

  // Called by the app
  window.__androidBle = {
    resolve(id, json) {
      const p = pending.get(id);
      if (!p) return;
      pending.delete(id);
      p.resolve(json ? JSON.parse(json) : null);
    },
    reject(id, name, message) {
      const p = pending.get(id);
      if (!p) return;
      pending.delete(id);
      const err = new Error(message);
      err.name = name;
      p.reject(err);
    },
    disconnected(address) {
      const device = devices.get(address);
      if (!device) return;
      device.gatt.connected = false;
      device.dispatchEvent(new Event("gattserverdisconnected"));
    },
    notify(address, uuid, base64) {
      const c = characteristics.get(`${address}|${uuid.toLowerCase()}`);
      if (!c) return;
      c.value = toDataView(base64);
      c.dispatchEvent(new Event("characteristicvaluechanged"));
    },
  };

  class BleCharacteristic extends EventTarget {
    constructor(service, uuid) {
      super();
      this.service = service;
      this.uuid = uuid;
      this.value = null;
    }
    get _args() { return [this.service.device.id, this.service.uuid, this.uuid]; }
    async readValue() {
      this.value = toDataView(await call("read", ...this._args));
      return this.value;
    }
    writeValueWithResponse(data) { return call("write", ...this._args, toBase64(data)); }
    writeValue(data) { return this.writeValueWithResponse(data); }
    async startNotifications() {
      await call("startNotifications", ...this._args);
      return this;
    }
  }

  class BleService {
    constructor(device, uuid) {
      this.device = device;
      this.uuid = uuid;
    }
    async getCharacteristic(uuid) {
      uuid = uuid.toLowerCase();
      await call("hasCharacteristic", this.device.id, this.uuid, uuid);
      const key = `${this.device.id}|${uuid}`;
      if (!characteristics.has(key)) characteristics.set(key, new BleCharacteristic(this, uuid));
      return characteristics.get(key);
    }
  }

  class BleServer {
    constructor(device) {
      this.device = device;
      this.connected = false;
    }
    async connect() {
      await call("connect", this.device.id);
      this.connected = true;
      return this;
    }
    disconnect() {
      window.AndroidBle.disconnect(this.device.id);
    }
    async getPrimaryService(uuid) {
      uuid = uuid.toLowerCase();
      await call("hasService", this.device.id, uuid);
      return new BleService(this.device, uuid);
    }
  }

  class BleDevice extends EventTarget {
    constructor(id, name) {
      super();
      this.id = id;
      this.name = name;
      this.gatt = new BleServer(this);
    }
  }

  function deviceFor(info) {
    if (!devices.has(info.id)) devices.set(info.id, new BleDevice(info.id, info.name));
    return devices.get(info.id);
  }

  const bluetooth = {
    async requestDevice(options = {}) {
      // The terminal advertises its service UUID; scan for that
      const services = (options.filters || []).flatMap((f) => f.services || []);
      const uuid = services[0] || (options.optionalServices || [])[0];
      return deviceFor(await call("requestDevice", uuid));
    },
    async getDevices() {
      return (await call("getDevices")).map(deviceFor);
    },
  };

  Object.defineProperty(navigator, "bluetooth", { value: bluetooth, configurable: true });
})();
