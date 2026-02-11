/**
 * NeuronOS Inference Worker
 *
 * Runs the NeuronOS WASM module inside a dedicated Web Worker.
 * Receives commands from the main thread (neuronos-web.js) and
 * executes them against the WASM C API.
 *
 * Architecture:
 *   Main Thread (neuronos-web.js)
 *       │ postMessage({cmd, ...})
 *       ▼
 *   This Worker
 *       │ Module.ccall() / Module.cwrap()
 *       ▼
 *   neuronos-worker.wasm (C: neuronos_wasm_glue.c)
 *       │
 *       ▼
 *   llama.cpp + NeuronOS Agent + SQLite
 */

let Module = null;
let moduleReady = false;

// WASM function wrappers (set after module initialization)
let _wasm_init = null;
let _wasm_load_model = null;
let _wasm_generate = null;
let _wasm_agent_chat = null;
let _wasm_model_info = null;
let _wasm_memory_init = null;
let _wasm_memory_store = null;
let _wasm_memory_search = null;
let _wasm_free = null;
let _wasm_free_string = null;

/**
 * Initialize the WASM module.
 */
async function initModule(wasmPath) {
  // Import and instantiate the Emscripten-generated module
  // The module file exports a factory function called NeuronOSModule
  importScripts(wasmPath);

  Module = await NeuronOSModule({
    // Emscripten module configuration
    print: (text) => postMessage({ type: 'status', text }),
    printErr: (text) => postMessage({ type: 'error', text }),

    // Override locateFile to find .wasm next to .js
    locateFile: (path) => {
      if (path.endsWith('.wasm')) {
        return wasmPath.replace(/\.js$/, '.wasm');
      }
      if (path.endsWith('.worker.js')) {
        return wasmPath.replace(/\.js$/, '.worker.js');
      }
      return path;
    },
  });

  // Create wrapped C functions matching neuronos_wasm_glue.c signatures
  _wasm_init = Module.cwrap('neuronos_wasm_init', 'number',
    ['number', 'number']);  // (n_threads, n_ctx)
  _wasm_load_model = Module.cwrap('neuronos_wasm_load_model_from_buffer', 'number',
    ['number', 'number', 'number']);  // (data_ptr, size, n_ctx)
  _wasm_generate = Module.cwrap('neuronos_wasm_generate', 'number',
    ['string', 'number', 'number']);  // (prompt, n_predict, temp)
  _wasm_agent_chat = Module.cwrap('neuronos_wasm_agent_chat', 'number',
    ['string', 'number']);  // (message, n_predict)
  _wasm_model_info = Module.cwrap('neuronos_wasm_model_info', 'number', []);
  _wasm_memory_init = Module.cwrap('neuronos_wasm_memory_init', 'number', []);
  _wasm_memory_store = Module.cwrap('neuronos_wasm_memory_store', 'number',
    ['string', 'string']);  // (key, value)
  _wasm_memory_search = Module.cwrap('neuronos_wasm_memory_search', 'number',
    ['string', 'number']);  // (query, max_results)
  _wasm_free = Module.cwrap('neuronos_wasm_free', null, []);  // shutdown
  _wasm_free_string = Module.cwrap('neuronos_wasm_free_string', null, ['number']);

  moduleReady = true;
}

/**
 * Handle commands from the main thread.
 */
self.onmessage = async function(e) {
  const { id, cmd } = e.data;

  try {
    let result = null;

    switch (cmd) {
      case 'init': {
        const { wasmPath, nThreads, nCtx } = e.data;
        await initModule(wasmPath);
        const ret = _wasm_init(nThreads || 4, nCtx || 2048);
        if (ret !== 0) throw new Error(`Engine init failed (code ${ret})`);
        result = 'ok';
        break;
      }

      case 'loadModel': {
        const { buffer, nCtx } = e.data;
        const bytes = new Uint8Array(buffer);

        // Allocate WASM memory and copy the model buffer
        const ptr = Module._malloc(bytes.length);
        if (!ptr) throw new Error('Failed to allocate WASM memory for model');

        Module.HEAPU8.set(bytes, ptr);

        postMessage({ type: 'status', text: `Model buffer in WASM heap (${(bytes.length / 1024 / 1024).toFixed(0)} MB), loading...` });

        const ret = _wasm_load_model(ptr, bytes.length, nCtx || 2048);
        Module._free(ptr);

        if (ret !== 0) throw new Error(`Model load failed (code ${ret})`);
        result = 'ok';
        break;
      }

      case 'generate': {
        const { prompt, nPredict, temp } = e.data;
        const resultPtr = _wasm_generate(prompt, nPredict || 256, temp || 0.7);
        if (!resultPtr) throw new Error('Generation returned null');

        result = Module.UTF8ToString(resultPtr);
        _wasm_free_string(resultPtr);
        break;
      }

      case 'chat': {
        const { message, nPredict } = e.data;
        const resultPtr = _wasm_agent_chat(message, nPredict || 512);
        if (!resultPtr) throw new Error('Agent chat returned null');

        result = Module.UTF8ToString(resultPtr);
        _wasm_free_string(resultPtr);
        break;
      }

      case 'modelInfo': {
        const infoPtr = _wasm_model_info();
        if (!infoPtr) throw new Error('Model info returned null');
        result = Module.UTF8ToString(infoPtr);
        _wasm_free_string(infoPtr);
        break;
      }

      case 'memoryInit': {
        const ret = _wasm_memory_init();
        if (ret !== 0) throw new Error(`Memory init failed (code ${ret})`);
        result = 'ok';
        break;
      }

      case 'memoryStore': {
        const { key, content } = e.data;
        const ret = _wasm_memory_store(key, content);
        if (ret !== 0) throw new Error(`Memory store failed (code ${ret})`);
        result = 'ok';
        break;
      }

      case 'memorySearch': {
        const { query, maxResults } = e.data;
        const resultPtr = _wasm_memory_search(query, maxResults || 5);
        if (!resultPtr) {
          result = '[]';
        } else {
          result = Module.UTF8ToString(resultPtr);
          _wasm_free_string(resultPtr);
        }
        break;
      }

      default:
        throw new Error(`Unknown command: ${cmd}`);
    }

    postMessage({ type: 'result', id, data: result });
  } catch (err) {
    postMessage({ type: 'result', id, error: err.message });
    postMessage({ type: 'error', text: err.message });
  }
};
