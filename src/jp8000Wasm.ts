type WasmBuffer = ArrayBuffer | Uint8Array;

type EmscriptenModule = {
  HEAPU8: Uint8Array;
  HEAPU32: Uint32Array;
  HEAP32: Int32Array;
  wasmMemory?: WebAssembly.Memory;
  _malloc(size: number): number;
  _free(ptr: number): void;
  _jp8_create(
    romPtr: number,
    romSize: number,
    ramPtr: number,
    ramSize: number,
  ): number;
  _jp8_destroy(handle: number): void;
  _jp8_generate_samples(
    handle: number,
    outInterleaved: number,
    frames: number,
  ): number;
  _jp8_lcd_width(): number;
  _jp8_lcd_height(): number;
  _jp8_render_lcd(handle: number): void;
  _jp8_copy_lcd_pixels(
    handle: number,
    outPixels: number,
    maxPixels: number,
  ): number;
  _jp8_press_button(handle: number, which: number): void;
  _jp8_release_button(handle: number, which: number): void;
  _jp8_provide_midi(handle: number, dataPtr: number, len: number): void;
  _jp8_build_esp_wasm_snapshot(
    handle: number,
    asic: number,
    core: number,
  ): number;
  _jp8_get_esp_wasm_snapshot_ptr(handle: number): number;
  _jp8_get_esp_wasm_snapshot_size(handle: number): number;
  _jp8_fill_esp_wasm_runtime_ptrs(
    handle: number,
    asic: number,
    core: number,
    outPtrs: number,
    maxPtrs: number,
  ): number;
  _jp8_run_esp_wasm_reference_once(
    handle: number,
    asic: number,
    core: number,
  ): number;
  _jp8_esp_coredata_size(): number;
  _jp8_reset_jit_scheduler(handle: number): void;
  _jp8_step_host_and_count_dsp_samples(handle: number): number;
  _jp8_get_esp_program_dirty_mask(handle: number): number;
};

type CreateWasmModule = (opts?: {
  locateFile?: (path: string) => string;
}) => Promise<EmscriptenModule>;

declare global {
  interface Window {
    JP8000WasmModule?: CreateWasmModule;
  }
}

function asUint8Array(input: WasmBuffer): Uint8Array {
  return input instanceof Uint8Array ? input : new Uint8Array(input);
}

let modulePromise: Promise<EmscriptenModule> | null = null;
let scriptPromise: Promise<void> | null = null;

function ensureWasmScriptLoaded(): Promise<void> {
  if (window.JP8000WasmModule) {
    return Promise.resolve();
  }
  if (scriptPromise) {
    return scriptPromise;
  }

  scriptPromise = new Promise<void>((resolve, reject) => {
    const existing = document.querySelector(
      'script[data-jp8000-wasm="1"]',
    ) as HTMLScriptElement | null;
    if (existing) {
      existing.addEventListener("load", () => resolve(), { once: true });
      existing.addEventListener(
        "error",
        () => reject(new Error("Failed to load /jp8000_wasm.js")),
        { once: true },
      );
      return;
    }

    const script = document.createElement("script");
    script.src = "/jp8000_wasm.js";
    script.async = true;
    script.dataset.jp8000Wasm = "1";
    script.onload = () => resolve();
    script.onerror = () => reject(new Error("Failed to load /jp8000_wasm.js"));
    document.head.appendChild(script);
  });

  return scriptPromise;
}

async function getModule(): Promise<EmscriptenModule> {
  if (modulePromise == null) {
    modulePromise = (async () => {
      await ensureWasmScriptLoaded();
      const factory = window.JP8000WasmModule;
      if (!factory) {
        throw new Error("JP8000WasmModule factory not found on window");
      }
      return factory({
        locateFile: (path) => `/${path}`,
      });
    })();
  }
  return modulePromise;
}

export class JP8000WasmEmulator {
  private readonly module: EmscriptenModule;
  private readonly handle: number;
  private readonly lcdBufPtr: number;
  private readonly lcdPixelCount: number;
  private audioBufPtr = 0;
  private audioBufFrames = 0;
  private midiBufPtr = 0;
  private midiBufSize = 0;
  readonly lcdWidth: number;
  readonly lcdHeight: number;
  private destroyed = false;

  constructor(module: EmscriptenModule, handle: number) {
    this.module = module;
    this.handle = handle;
    this.lcdWidth = this.module._jp8_lcd_width();
    this.lcdHeight = this.module._jp8_lcd_height();
    this.lcdPixelCount = this.lcdWidth * this.lcdHeight;
    this.lcdBufPtr = this.module._malloc(this.lcdPixelCount * 4);
  }

  generateSamples(outLeft: Float32Array, outRight: Float32Array): number {
    this.assertAlive();
    const frames = Math.min(outLeft.length, outRight.length);
    if (frames <= 0) {
      return 0;
    }

    this.ensureAudioBuffer(frames);
    const produced = this.module._jp8_generate_samples(
      this.handle,
      this.audioBufPtr,
      frames,
    );

    const base = this.audioBufPtr >> 2;
    const src = this.module.HEAP32;
    for (let i = 0; i < produced; i += 1) {
      const l = src[base + i * 2] >> 8;
      const r = src[base + i * 2 + 1] >> 8;
      outLeft[i] = Math.max(-1, Math.min(1, l / (32768 / 8)));
      outRight[i] = Math.max(-1, Math.min(1, r / (32768 / 8)));
    }

    for (let i = produced; i < frames; i += 1) {
      outLeft[i] = 0;
      outRight[i] = 0;
    }

    return produced;
  }

  renderLcd(): Uint32Array {
    this.assertAlive();
    this.module._jp8_render_lcd(this.handle);
    const copied = this.module._jp8_copy_lcd_pixels(
      this.handle,
      this.lcdBufPtr,
      this.lcdPixelCount,
    );
    if (copied !== this.lcdPixelCount) {
      throw new Error(
        `LCD copy failed, expected ${this.lcdPixelCount} pixels, got ${copied}`,
      );
    }

    const base = this.lcdBufPtr >> 2;
    return this.module.HEAPU32.slice(base, base + this.lcdPixelCount);
  }

  pressButton(which: number): void {
    this.assertAlive();
    this.module._jp8_press_button(this.handle, which);
  }

  releaseButton(which: number): void {
    this.assertAlive();
    this.module._jp8_release_button(this.handle, which);
  }

  provideMIDI(data: Uint8Array): void {
    this.assertAlive();
    if (data.length === 0) {
      return;
    }
    this.ensureMidiBuffer(data.length);
    this.module.HEAPU8.set(data, this.midiBufPtr);
    this.module._jp8_provide_midi(this.handle, this.midiBufPtr, data.length);
  }

  startDemoSongs(): void {
    this.assertAlive();
    this.pressButton(130);
    this.pressButton(131);
  }

  stopDemoSongsButton(): void {
    this.assertAlive();
    this.releaseButton(130);
    this.releaseButton(131);
  }

  buildEspWasmSnapshot(asic: number, core: number): Uint8Array {
    this.assertAlive();
    const built = this.module._jp8_build_esp_wasm_snapshot(
      this.handle,
      asic,
      core,
    );
    if (built <= 0) {
      throw new Error(
        `Failed to build ESP snapshot for asic=${asic} core=${core}`,
      );
    }

    const ptr = this.module._jp8_get_esp_wasm_snapshot_ptr(this.handle);
    const size = this.module._jp8_get_esp_wasm_snapshot_size(this.handle);
    if (ptr === 0 || size <= 0) {
      throw new Error("ESP snapshot pointer/size is invalid");
    }

    return this.module.HEAPU8.slice(ptr, ptr + size);
  }

  getEspWasmRuntimePointers(asic: number, core: number): Uint32Array {
    this.assertAlive();
    const count = 12;
    const outPtr = this.module._malloc(count * 4);
    try {
      const written = this.module._jp8_fill_esp_wasm_runtime_ptrs(
        this.handle,
        asic,
        core,
        outPtr,
        count,
      );
      if (written !== count) {
        throw new Error(
          `Failed to get ESP runtime pointers for asic=${asic} core=${core}`,
        );
      }
      const base = outPtr >> 2;
      return this.module.HEAPU32.slice(base, base + count);
    } finally {
      this.module._free(outPtr);
    }
  }

  getWasmMemory(): WebAssembly.Memory {
    this.assertAlive();
    const direct = this.module.wasmMemory;
    if (direct instanceof WebAssembly.Memory) {
      return direct;
    }

    const asmMem = (this.module as unknown as { asm?: { memory?: unknown } })
      .asm?.memory;
    if (asmMem instanceof WebAssembly.Memory) {
      return asmMem;
    }

    throw new Error("Emscripten wasm memory is not available");
  }

  readMemory(ptr: number, size: number): Uint8Array {
    this.assertAlive();
    if (ptr < 0 || size < 0) {
      throw new Error("Invalid memory range");
    }
    return this.module.HEAPU8.slice(ptr, ptr + size);
  }

  writeMemory(ptr: number, bytes: Uint8Array): void {
    this.assertAlive();
    this.module.HEAPU8.set(bytes, ptr);
  }

  readI32(ptr: number): number {
    this.assertAlive();
    return this.module.HEAP32[ptr >> 2] | 0;
  }

  writeI32(ptr: number, value: number): void {
    this.assertAlive();
    this.module.HEAP32[ptr >> 2] = value | 0;
  }

  runEspWasmReferenceOnce(asic: number, core: number): void {
    this.assertAlive();
    const ok = this.module._jp8_run_esp_wasm_reference_once(
      this.handle,
      asic,
      core,
    );
    if (ok !== 1) {
      throw new Error(
        `Failed to run ESP reference once for asic=${asic} core=${core}`,
      );
    }
  }

  getEspCoreDataSize(): number {
    this.assertAlive();
    const size = this.module._jp8_esp_coredata_size();
    if (size <= 0) {
      throw new Error("Invalid ESP core data size");
    }
    return size;
  }

  resetJitScheduler(): void {
    this.assertAlive();
    this.module._jp8_reset_jit_scheduler(this.handle);
  }

  stepHostAndCountDspSamples(): number {
    this.assertAlive();
    return this.module._jp8_step_host_and_count_dsp_samples(this.handle) | 0;
  }

  getEspProgramDirtyMask(): number {
    this.assertAlive();
    return this.module._jp8_get_esp_program_dirty_mask(this.handle) >>> 0;
  }

  destroy(): void {
    if (this.destroyed) {
      return;
    }
    this.destroyed = true;
    this.module._jp8_destroy(this.handle);
    if (this.audioBufPtr !== 0) {
      this.module._free(this.audioBufPtr);
      this.audioBufPtr = 0;
      this.audioBufFrames = 0;
    }
    if (this.midiBufPtr !== 0) {
      this.module._free(this.midiBufPtr);
      this.midiBufPtr = 0;
      this.midiBufSize = 0;
    }
    this.module._free(this.lcdBufPtr);
  }

  private ensureAudioBuffer(frames: number): void {
    if (this.audioBufFrames >= frames) {
      return;
    }
    if (this.audioBufPtr !== 0) {
      this.module._free(this.audioBufPtr);
    }
    this.audioBufFrames = frames;
    this.audioBufPtr = this.module._malloc(frames * 2 * 4);
  }

  private ensureMidiBuffer(bytes: number): void {
    if (this.midiBufSize >= bytes) {
      return;
    }
    if (this.midiBufPtr !== 0) {
      this.module._free(this.midiBufPtr);
    }
    this.midiBufSize = bytes;
    this.midiBufPtr = this.module._malloc(bytes);
  }

  private assertAlive(): void {
    if (this.destroyed) {
      throw new Error("JP8000 emulator instance has been destroyed");
    }
  }
}

export async function createJP8000Emulator(
  rom: WasmBuffer,
  ram?: WasmBuffer,
): Promise<JP8000WasmEmulator> {
  const wasm = await getModule();
  const romBytes = asUint8Array(rom);
  const ramBytes = ram ? asUint8Array(ram) : new Uint8Array(0);

  if (romBytes.byteLength === 0) {
    throw new Error("ROM buffer must not be empty");
  }

  const romPtr = wasm._malloc(romBytes.byteLength);
  const ramPtr =
    ramBytes.byteLength > 0 ? wasm._malloc(ramBytes.byteLength) : 0;

  try {
    wasm.HEAPU8.set(romBytes, romPtr);
    if (ramPtr !== 0) {
      wasm.HEAPU8.set(ramBytes, ramPtr);
    }

    const handle = wasm._jp8_create(
      romPtr,
      romBytes.byteLength,
      ramPtr,
      ramBytes.byteLength,
    );
    if (handle === 0) {
      throw new Error("Failed to create JP8000 emulator instance");
    }

    return new JP8000WasmEmulator(wasm, handle);
  } finally {
    wasm._free(romPtr);
    if (ramPtr !== 0) {
      wasm._free(ramPtr);
    }
  }
}
