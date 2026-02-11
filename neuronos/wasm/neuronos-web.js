/**
 * NeuronOS Web — JavaScript API wrapper for browser-based inference.
 *
 * Architecture:
 *   neuronos-web.js (this) ──▶ neuronos-worker.js (Web Worker)
 *                                    ▼
 *                           neuronos-worker.wasm (C/WASM)
 *                                    ▼
 *                    llama.cpp + NeuronOS Agent + SQLite
 *
 * Features:
 *   - Download GGUF model with progress (parallel chunk downloads)
 *   - Cache model in OPFS (Origin Private File System)
 *   - Load model into WASM engine
 *   - Generate text / chat with agent (streaming)
 *   - Persistent memory (SQLite+FTS5 via WASM)
 *   - Auto-detect multi-thread support (SharedArrayBuffer)
 *
 * Usage:
 *   const neuronos = new NeuronOS();
 *   await neuronos.init();
 *   await neuronos.loadModel('https://huggingface.co/.../model.gguf');
 *   const response = await neuronos.chat('Hello!');
 */

class NeuronOS {
  constructor(options = {}) {
    /** @type {Worker|null} */
    this._worker = null;
    /** @type {boolean} */
    this._ready = false;
    /** @type {boolean} */
    this._modelLoaded = false;
    /** @type {Map<number, {resolve, reject}>} */
    this._pending = new Map();
    /** @type {number} */
    this._nextId = 1;

    // Configuration
    this._config = {
      wasmPaths: options.wasmPaths || {
        'multi-thread': './dist/neuronos-worker.js',
        'single-thread': './dist/neuronos-worker-st.js',
      },
      maxParallelDownloads: options.maxParallelDownloads || 3,
      cacheEnabled: options.cacheEnabled !== false,
      nCtx: options.nCtx || 2048,
      nThreads: options.nThreads || 0, // 0 = auto
      ...options,
    };

    // Callbacks
    /** @type {function|null} */
    this.onStatus = options.onStatus || null;
    /** @type {function|null} */
    this.onToken = options.onToken || null;
    /** @type {function|null} */
    this.onAgentStep = options.onAgentStep || null;
    /** @type {function|null} */
    this.onProgress = options.onProgress || null;
    /** @type {function|null} */
    this.onError = options.onError || null;
  }

  /* ═══════════════════════════════════════════════════
   * Initialization
   * ═══════════════════════════════════════════════════ */

  /**
   * Initialize the NeuronOS WASM engine.
   * Auto-detects multi-thread support and starts the Web Worker.
   */
  async init() {
    if (this._ready) return;

    const multiThreadSupported = this._checkMultiThreadSupport();
    const wasmPath = multiThreadSupported
      ? this._config.wasmPaths['multi-thread']
      : this._config.wasmPaths['single-thread'];

    this._emitStatus(`Using ${multiThreadSupported ? 'multi-thread' : 'single-thread'} WASM build`);

    // Create Web Worker from the inference worker script
    this._worker = new Worker(new URL('./neuronos-inference-worker.js', import.meta.url), {
      type: 'module',
    });

    // Listen for messages from the worker
    this._worker.onmessage = (e) => this._handleWorkerMessage(e.data);
    this._worker.onerror = (e) => {
      this._emitError(`Worker error: ${e.message}`);
    };

    // Initialize the WASM module in the worker
    await this._sendCommand('init', {
      wasmPath,
      nThreads: this._config.nThreads,
    });

    this._ready = true;
    this._emitStatus('NeuronOS WASM engine ready');
  }

  /**
   * Check if the browser supports multi-threading (SharedArrayBuffer).
   */
  _checkMultiThreadSupport() {
    try {
      return (
        typeof SharedArrayBuffer !== 'undefined' &&
        typeof Atomics !== 'undefined' &&
        // Check COOP/COEP headers via crossOriginIsolated
        (typeof crossOriginIsolated !== 'undefined' ? crossOriginIsolated : false)
      );
    } catch {
      return false;
    }
  }

  /* ═══════════════════════════════════════════════════
   * Model Loading
   * ═══════════════════════════════════════════════════ */

  /**
   * Load a GGUF model from a URL.
   * Supports split models (automatically detects -00001-of-NNNNN pattern).
   * Uses OPFS caching for subsequent loads.
   *
   * @param {string} url - URL to the GGUF model file
   * @param {object} options - { nCtx, nThreads, forceRedownload }
   */
  async loadModel(url, options = {}) {
    if (!this._ready) throw new Error('NeuronOS not initialized. Call init() first.');

    const nCtx = options.nCtx || this._config.nCtx;
    const nThreads = options.nThreads || this._config.nThreads;

    // Check if model is cached in OPFS
    const cacheKey = this._urlToCacheKey(url);
    let modelBuffer = null;

    if (this._config.cacheEnabled && !options.forceRedownload) {
      modelBuffer = await this._loadFromCache(cacheKey);
      if (modelBuffer) {
        this._emitStatus('Model loaded from browser cache (OPFS)');
      }
    }

    // Download if not cached
    if (!modelBuffer) {
      const isSplit = this._isSplitModel(url);
      if (isSplit) {
        modelBuffer = await this._downloadSplitModel(url);
      } else {
        modelBuffer = await this._downloadModel(url);
      }

      // Cache the downloaded model
      if (this._config.cacheEnabled && modelBuffer) {
        await this._saveToCache(cacheKey, modelBuffer);
        this._emitStatus('Model cached in browser storage (OPFS)');
      }
    }

    if (!modelBuffer) {
      throw new Error('Failed to download model');
    }

    this._emitStatus(`Loading model into engine (${(modelBuffer.byteLength / 1024 / 1024).toFixed(0)} MB)...`);

    // Send model buffer to worker
    await this._sendCommand('loadModel', {
      buffer: modelBuffer,
      nCtx,
      nThreads,
    }, [modelBuffer]); // Transfer ownership of the ArrayBuffer

    this._modelLoaded = true;
    this._emitStatus('Model ready for inference');
  }

  /**
   * Load a GGUF model from a File object (user picks from disk).
   */
  async loadModelFromFile(file, options = {}) {
    if (!this._ready) throw new Error('NeuronOS not initialized.');

    this._emitStatus(`Reading file: ${file.name} (${(file.size / 1024 / 1024).toFixed(0)} MB)...`);
    const buffer = await file.arrayBuffer();

    await this._sendCommand('loadModel', {
      buffer,
      nCtx: options.nCtx || this._config.nCtx,
      nThreads: options.nThreads || this._config.nThreads,
    }, [buffer]);

    this._modelLoaded = true;
    this._emitStatus('Model ready for inference');
  }

  /**
   * Load model from HuggingFace Hub.
   * @param {string} repo - e.g. 'microsoft/BitNet-b1.58-2B-4T-gguf'
   * @param {string} filename - e.g. 'ggml-model-i2_s.gguf'
   */
  async loadModelFromHF(repo, filename, options = {}) {
    const url = `https://huggingface.co/${repo}/resolve/main/${filename}`;
    return this.loadModel(url, options);
  }

  /* ═══════════════════════════════════════════════════
   * Inference API
   * ═══════════════════════════════════════════════════ */

  /**
   * Generate text from a prompt.
   * @param {string} prompt
   * @param {object} options - { nPredict, temp }
   * @returns {Promise<string>} Generated text
   */
  async generate(prompt, options = {}) {
    if (!this._modelLoaded) throw new Error('No model loaded.');
    return this._sendCommand('generate', {
      prompt,
      nPredict: options.nPredict || 256,
      temp: options.temp || 0.7,
    });
  }

  /**
   * Chat with the NeuronOS agent (ReAct loop).
   * Streams agent steps via onAgentStep callback.
   *
   * @param {string} message - User message
   * @param {object} options - { nPredict }
   * @returns {Promise<string>} Agent response
   */
  async chat(message, options = {}) {
    if (!this._modelLoaded) throw new Error('No model loaded.');
    return this._sendCommand('chat', {
      message,
      nPredict: options.nPredict || 512,
    });
  }

  /**
   * Get model information.
   * @returns {Promise<object>} Model info
   */
  async getModelInfo() {
    if (!this._modelLoaded) throw new Error('No model loaded.');
    const json = await this._sendCommand('modelInfo');
    return JSON.parse(json);
  }

  /* ═══════════════════════════════════════════════════
   * Memory API
   * ═══════════════════════════════════════════════════ */

  /**
   * Initialize persistent memory (SQLite+FTS5).
   */
  async initMemory() {
    return this._sendCommand('memoryInit');
  }

  /**
   * Store a fact in archival memory.
   */
  async memoryStore(key, content) {
    return this._sendCommand('memoryStore', { key, content });
  }

  /**
   * Search archival memory.
   * @returns {Promise<Array>} Search results
   */
  async memorySearch(query, maxResults = 5) {
    const json = await this._sendCommand('memorySearch', { query, maxResults });
    return JSON.parse(json);
  }

  /* ═══════════════════════════════════════════════════
   * Model Download (with progress + parallel chunks)
   * ═══════════════════════════════════════════════════ */

  async _downloadModel(url) {
    this._emitStatus('Downloading model...');

    const response = await fetch(url);
    if (!response.ok) throw new Error(`Download failed: ${response.status}`);

    const contentLength = parseInt(response.headers.get('Content-Length') || '0', 10);
    const reader = response.body.getReader();
    const chunks = [];
    let received = 0;

    while (true) {
      const { done, value } = await reader.read();
      if (done) break;
      chunks.push(value);
      received += value.length;

      if (contentLength > 0 && this.onProgress) {
        this.onProgress({
          loaded: received,
          total: contentLength,
          percent: Math.round((received / contentLength) * 100),
        });
      }
    }

    // Combine chunks into a single ArrayBuffer
    const buffer = new Uint8Array(received);
    let offset = 0;
    for (const chunk of chunks) {
      buffer.set(chunk, offset);
      offset += chunk.length;
    }

    return buffer.buffer;
  }

  /**
   * Download split model files in parallel.
   * Pattern: model-00001-of-00003.gguf, model-00002-of-00003.gguf, etc.
   */
  async _downloadSplitModel(firstPartUrl) {
    const match = firstPartUrl.match(/(.+)-(\d{5})-of-(\d{5})\.gguf$/);
    if (!match) return this._downloadModel(firstPartUrl);

    const base = match[1];
    const totalParts = parseInt(match[3], 10);
    this._emitStatus(`Downloading split model (${totalParts} parts)...`);

    const urls = [];
    for (let i = 1; i <= totalParts; i++) {
      const padded = String(i).padStart(5, '0');
      const padTotal = String(totalParts).padStart(5, '0');
      urls.push(`${base}-${padded}-of-${padTotal}.gguf`);
    }

    // Download in parallel batches
    const buffers = [];
    let totalSize = 0;
    const batchSize = this._config.maxParallelDownloads;

    for (let i = 0; i < urls.length; i += batchSize) {
      const batch = urls.slice(i, i + batchSize);
      const results = await Promise.all(
        batch.map(async (url, idx) => {
          const partNum = i + idx + 1;
          this._emitStatus(`Downloading part ${partNum}/${totalParts}...`);
          const resp = await fetch(url);
          if (!resp.ok) throw new Error(`Failed to download part ${partNum}`);
          return resp.arrayBuffer();
        })
      );
      buffers.push(...results);
      totalSize += results.reduce((sum, buf) => sum + buf.byteLength, 0);

      if (this.onProgress) {
        this.onProgress({
          loaded: buffers.length,
          total: totalParts,
          percent: Math.round((buffers.length / totalParts) * 100),
        });
      }
    }

    // Concatenate all parts
    const combined = new Uint8Array(totalSize);
    let offset = 0;
    for (const buf of buffers) {
      combined.set(new Uint8Array(buf), offset);
      offset += buf.byteLength;
    }

    return combined.buffer;
  }

  _isSplitModel(url) {
    return /-\d{5}-of-\d{5}\.gguf$/.test(url);
  }

  /* ═══════════════════════════════════════════════════
   * OPFS Caching
   * ═══════════════════════════════════════════════════ */

  _urlToCacheKey(url) {
    // Create a stable filename from URL
    const hash = Array.from(url).reduce((h, c) => ((h << 5) - h + c.charCodeAt(0)) | 0, 0);
    return `neuronos_model_${Math.abs(hash).toString(36)}`;
  }

  async _loadFromCache(key) {
    try {
      if (!('storage' in navigator && 'getDirectory' in navigator.storage)) return null;
      const root = await navigator.storage.getDirectory();
      const dirHandle = await root.getDirectoryHandle('neuronos-cache', { create: false });
      const fileHandle = await dirHandle.getFileHandle(key, { create: false });
      const file = await fileHandle.getFile();
      this._emitStatus(`Loading cached model (${(file.size / 1024 / 1024).toFixed(0)} MB)...`);
      return await file.arrayBuffer();
    } catch {
      return null; // Not cached
    }
  }

  async _saveToCache(key, buffer) {
    try {
      if (!('storage' in navigator && 'getDirectory' in navigator.storage)) return;
      const root = await navigator.storage.getDirectory();
      const dirHandle = await root.getDirectoryHandle('neuronos-cache', { create: true });
      const fileHandle = await dirHandle.getFileHandle(key, { create: true });
      const writable = await fileHandle.createWritable();
      await writable.write(buffer);
      await writable.close();
    } catch (e) {
      console.warn('OPFS cache save failed:', e);
    }
  }

  /**
   * Clear all cached models from OPFS.
   */
  async clearCache() {
    try {
      if (!('storage' in navigator && 'getDirectory' in navigator.storage)) return;
      const root = await navigator.storage.getDirectory();
      await root.removeEntry('neuronos-cache', { recursive: true });
      this._emitStatus('Model cache cleared');
    } catch {
      // No cache to clear
    }
  }

  /* ═══════════════════════════════════════════════════
   * Worker Communication
   * ═══════════════════════════════════════════════════ */

  _sendCommand(cmd, data = {}, transfer = []) {
    return new Promise((resolve, reject) => {
      const id = this._nextId++;
      this._pending.set(id, { resolve, reject });
      this._worker.postMessage({ id, cmd, ...data }, transfer);
    });
  }

  _handleWorkerMessage(msg) {
    switch (msg.type) {
      case 'result': {
        const pending = this._pending.get(msg.id);
        if (pending) {
          this._pending.delete(msg.id);
          if (msg.error) {
            pending.reject(new Error(msg.error));
          } else {
            pending.resolve(msg.data);
          }
        }
        break;
      }
      case 'token':
        if (this.onToken) this.onToken(msg.text);
        break;
      case 'agent_step':
        if (this.onAgentStep) this.onAgentStep(msg.step_type, msg.text);
        break;
      case 'status':
        this._emitStatus(msg.text);
        break;
      case 'error':
        this._emitError(msg.text);
        break;
    }
  }

  _emitStatus(text) {
    if (this.onStatus) this.onStatus(text);
  }

  _emitError(text) {
    if (this.onError) this.onError(text);
    console.error('[NeuronOS]', text);
  }

  /* ═══════════════════════════════════════════════════
   * Cleanup
   * ═══════════════════════════════════════════════════ */

  /**
   * Terminate the Web Worker and free resources.
   */
  destroy() {
    if (this._worker) {
      this._worker.terminate();
      this._worker = null;
    }
    this._ready = false;
    this._modelLoaded = false;
    this._pending.clear();
  }

  /**
   * Get NeuronOS version.
   */
  static get version() {
    return '0.9.1';
  }
}

// Export for ES modules and global scope
if (typeof module !== 'undefined' && module.exports) {
  module.exports = { NeuronOS };
}
if (typeof window !== 'undefined') {
  window.NeuronOS = NeuronOS;
}

export { NeuronOS };
