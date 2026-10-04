// canary-local/tests/native/core_standin.js — what a browser probe's server
// answers in place of emulator/dist/<core>.js under LAB_CORES=native (sweep
// A41; served by probe_cores.js, never part of the Lab and never a test).
//
// It defines the dist's factory (createCanaryVisionCore, createCanaryAudioCore)
// with the dist module's shape: call it, await a module, cwrap its exports;
// ccall, UTF8ToString and the HEAP views are there when the dist has them. A
// call is a synchronous same-origin request to the probe server, which runs
// it on the core cores.js built from this tree's sources and answers with
// the return value and the bytes of every memory window the core opened.
// Calls stay synchronous, as a wasm call is. The page's policy allows all of
// it as served: this file is a 'self' script, and its requests are 'self'
// connects (no eval, no inline script, no new source).
//
// The heap here mirrors the probe server's: a pointer export returns an
// offset into it, the page writes through HEAP16 (or any view) there, and
// each call carries those windows to the core and brings them back.

(function () {
  "use strict";

  // Written by probe_cores.js when it serves this file:
  // { core, exportName, endpoint }.
  const CONFIG = __LAB_NATIVE_CORE__;

  const VIEWS = { HEAP8: Int8Array, HEAPU8: Uint8Array, HEAP16: Int16Array, HEAPU16: Uint16Array,
    HEAP32: Int32Array, HEAPU32: Uint32Array, HEAPF32: Float32Array, HEAPF64: Float64Array };

  function rpc(msg) {
    const xhr = new XMLHttpRequest();
    xhr.open("POST", CONFIG.endpoint, false);
    xhr.setRequestHeader("content-type", "application/json");
    xhr.send(JSON.stringify(msg));
    if (xhr.status !== 200) {
      throw new Error(`native core ${CONFIG.core}: the probe server answered ${xhr.status}`);
    }
    const reply = JSON.parse(xhr.responseText);
    if (reply.error) throw new Error(reply.error);
    return reply;
  }

  const HEX = "0123456789abcdef";
  const hexOf = (bytes) => {
    let s = "";
    for (let i = 0; i < bytes.length; i++) s += HEX[bytes[i] >> 4] + HEX[bytes[i] & 15];
    return s;
  };
  const bytesOf = (hex) => {
    const out = new Uint8Array(hex.length / 2);
    for (let i = 0; i < out.length; i++) out[i] = parseInt(hex.substr(2 * i, 2), 16);
    return out;
  };

  function create() {
    const info = rpc({ op: "new", core: CONFIG.core });
    const mod = { nativeCore: info.nativeCore };
    let heap = new ArrayBuffer(info.heap);
    let windows = [];
    const views = () => {
      for (const v of info.views) if (VIEWS[v]) mod[v] = new VIEWS[v](heap);
    };
    const take = (reply) => {
      if (reply.heap > heap.byteLength) {
        // as wasm memory growth does: the old buffer is detached, re-view it
        if (typeof heap.transfer === "function") heap = heap.transfer(reply.heap);
        else {
          const grown = new ArrayBuffer(reply.heap);
          new Uint8Array(grown).set(new Uint8Array(heap));
          heap = grown;
        }
        views();
      }
      windows = reply.mem.map(([offset, hex]) => {
        const bytes = bytesOf(hex);
        new Uint8Array(heap, offset, bytes.length).set(bytes);
        return [offset, bytes.length];
      });
      return reply.ret;
    };
    const mem = () => windows.map(([offset, len]) => [offset, hexOf(new Uint8Array(heap, offset, len))]);
    views();
    take(info);

    if (info.functions.includes("cwrap")) {
      // refused by the server's cwrap, here, before any call runs
      mod.cwrap = (fn, ret, argTypes) => {
        const { wrap } = rpc({ op: "cwrap", id: info.id, fn, ret, argTypes });
        return (...args) => take(rpc({ op: "call", id: info.id, wrap, args, mem: mem() }));
      };
    }
    if (info.functions.includes("ccall")) {
      mod.ccall = (fn, ret, argTypes, args) =>
        take(rpc({ op: "ccall", id: info.id, fn, ret, argTypes, args: args || [], mem: mem() }));
    }
    if (info.functions.includes("UTF8ToString")) {
      mod.UTF8ToString = (...args) => take(rpc({ op: "UTF8ToString", id: info.id, args, mem: mem() }));
    }
    return mod;
  }

  globalThis[CONFIG.exportName] = function () {
    return Promise.resolve().then(create);
  };
})();
