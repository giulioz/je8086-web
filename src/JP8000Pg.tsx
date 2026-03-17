import { useCallback, useEffect, useRef, useState } from "react";
import { JP8000WasmEmulator, createJP8000Emulator } from "./jp8000Wasm";
import { PianoKeyboard } from "./PianoKeyboard";

type JitRunFn = (...args: number[]) => void;

type JitCoreRunner = {
  run: JitRunFn;
  args: number[];
  instance: WebAssembly.Instance;
};

type JitAsicRunners = {
  core0: JitCoreRunner;
  core1: JitCoreRunner;
  gramPtr: number;
  iramPos0Ptr: number;
  iramPos1Ptr: number;
  eramPosPtr: number;
  hasEram: boolean;
};

type JitRuntime = {
  asics: JitAsicRunners[];
};

const NUMBER_SWITCHES = [
  { label: "1", value: 89 },
  { label: "2", value: 81 },
  { label: "3", value: 73 },
  { label: "4", value: 90 },
  { label: "5", value: 82 },
  { label: "6", value: 74 },
  { label: "7", value: 91 },
  { label: "8", value: 83 },
] as const;


export function JP8000Pg() {
  const [emulator, setEmulator] = useState<JP8000WasmEmulator | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [loading, setLoading] = useState(false);
  const [jitEnabled, setJitEnabled] = useState(true);
  const [romFile, setRomFile] = useState<File | null>(null);
  const [ramFile, setRamFile] = useState<File | null>(null);
  const jitEnabledRef = useRef(jitEnabled);
  const canvasRef = useRef<HTMLCanvasElement | null>(null);
  const releaseDemoTimeoutRef = useRef<number | null>(null);
  const audioContextRef = useRef<AudioContext | null>(null);
  const audioNodeRef = useRef<ScriptProcessorNode | null>(null);
  const emulatorRef = useRef<JP8000WasmEmulator | null>(null);
  const jitRuntimeRef = useRef<JitRuntime | null>(null);
  const pendingJitSamplesRef = useRef(0);
  const switchReleaseTimeoutsRef = useRef<number[]>([]);

  const drawLcd = useCallback((target: JP8000WasmEmulator) => {
    const canvas = canvasRef.current;
    if (!canvas) {
      return;
    }

    if (canvas.width !== target.lcdWidth) canvas.width = target.lcdWidth;
    if (canvas.height !== target.lcdHeight) canvas.height = target.lcdHeight;

    const ctx = canvas.getContext("2d");
    if (!ctx) {
      return;
    }

    const argbPixels = target.renderLcd();
    const imageData = ctx.createImageData(target.lcdWidth, target.lcdHeight);
    const out = imageData.data;

    for (let i = 0; i < argbPixels.length; i += 1) {
      const p = argbPixels[i];
      const a = (p >>> 24) & 0xff;
      const r = (p >>> 16) & 0xff;
      const g = (p >>> 8) & 0xff;
      const b = p & 0xff;
      const o = i * 4;
      out[o] = r;
      out[o + 1] = g;
      out[o + 2] = b;
      out[o + 3] = a === 0 ? 255 : a;
    }

    ctx.putImageData(imageData, 0, 0);
  }, []);

  useEffect(() => {
    emulatorRef.current = emulator;
  }, [emulator]);

  useEffect(() => {
    return () => {
      if (releaseDemoTimeoutRef.current != null) {
        window.clearTimeout(releaseDemoTimeoutRef.current);
      }
      for (const timeoutId of switchReleaseTimeoutsRef.current) {
        window.clearTimeout(timeoutId);
      }
      switchReleaseTimeoutsRef.current = [];
      if (audioNodeRef.current) {
        audioNodeRef.current.disconnect();
        audioNodeRef.current.onaudioprocess = null;
        audioNodeRef.current = null;
      }
      if (audioContextRef.current) {
        void audioContextRef.current.close();
        audioContextRef.current = null;
      }
      jitRuntimeRef.current = null;
      emulatorRef.current?.destroy();
    };
  }, []);

  useEffect(() => {
    if (!emulator) {
      return;
    }

    let rafId = 0;
    let cancelled = false;
    const frame = () => {
      if (cancelled) {
        return;
      }

      try {
        drawLcd(emulator);
      } catch (e) {
        setError(e instanceof Error ? e.message : String(e));
        return;
      }

      rafId = requestAnimationFrame(frame);
    };

    rafId = requestAnimationFrame(frame);

    return () => {
      cancelled = true;
      cancelAnimationFrame(rafId);
    };
  }, [drawLcd, emulator]);

  const stopAudio = useCallback(() => {
    if (audioNodeRef.current) {
      audioNodeRef.current.disconnect();
      audioNodeRef.current.onaudioprocess = null;
      audioNodeRef.current = null;
    }
  }, []);

  const ensureAudioContext = useCallback(async (): Promise<AudioContext> => {
    if (!audioContextRef.current) {
      audioContextRef.current = new AudioContext({ sampleRate: 88200 });
    }

    const context = audioContextRef.current;
    if (context.state !== "running") {
      await context.resume();
    }
    return context;
  }, []);

  const sendMidiBytes = useCallback(
    (status: number, data1: number, data2: number) => {
      if (!emulator) {
        return;
      }

      try {
        emulator.provideMIDI(
          Uint8Array.of(status & 0xff, data1 & 0xff, data2 & 0xff),
        );
      } catch (e) {
        setError(e instanceof Error ? e.message : String(e));
      }
    },
    [emulator],
  );

  const generateJitSamples = useCallback(
    (
      target: JP8000WasmEmulator,
      runtime: JitRuntime,
      outL: Float32Array,
      outR: Float32Array,
    ) => {
      const readGram = (asic: JitAsicRunners, offset: number): number => {
        const iramPos = target.readI32(asic.iramPos0Ptr) & 0xff;
        const index = (offset + iramPos) & 0xff;
        return target.readI32(asic.gramPtr + (index << 2));
      };

      const writeGram = (
        asic: JitAsicRunners,
        offset: number,
        value: number,
      ): void => {
        const iramPos = target.readI32(asic.iramPos0Ptr) & 0xff;
        const index = (offset + iramPos) & 0xff;
        target.writeI32(asic.gramPtr + (index << 2), value);
      };

      let frame = 0;
      while (frame < outL.length) {
        if (pendingJitSamplesRef.current <= 0) {
          pendingJitSamplesRef.current += target.stepHostAndCountDspSamples();
          if (pendingJitSamplesRef.current <= 0) {
            continue;
          }
        }

        for (
          let asicIndex = 0;
          asicIndex < runtime.asics.length;
          asicIndex += 1
        ) {
          const asic = runtime.asics[asicIndex];
          asic.core1.run(...asic.core1.args);
          asic.core0.run(...asic.core0.args);
        }

        const asic0 = runtime.asics[0];
        const asic1 = runtime.asics[1];
        const asic2 = runtime.asics[2];
        const asic3 = runtime.asics[3];

        for (let k = 0; k <= 0x4; k += 2)
          writeGram(asic1, k, readGram(asic0, 0x80 + k));
        for (let k = 0; k <= 0xa; k += 2)
          writeGram(asic2, k, readGram(asic1, 0x80 + k));
        for (let k = 0; k <= 0xe; k += 2)
          writeGram(asic3, k, readGram(asic2, 0x80 + k));
        writeGram(asic3, 0x20, readGram(asic2, 0xa0));
        writeGram(asic3, 0x22, readGram(asic2, 0xa2));

        const left = readGram(asic3, 0xe8);
        const right = readGram(asic3, 0xec);
        outL[frame] = Math.max(-1, Math.min(1, (left >> 8) / (32768 / 8)));
        outR[frame] = Math.max(-1, Math.min(1, (right >> 8) / (32768 / 8)));

        for (
          let asicIndex = 0;
          asicIndex < runtime.asics.length;
          asicIndex += 1
        ) {
          const asic = runtime.asics[asicIndex];
          const nextIramPos = (target.readI32(asic.iramPos0Ptr) - 1) & 0xff;
          target.writeI32(asic.iramPos0Ptr, nextIramPos);
          target.writeI32(asic.iramPos1Ptr, nextIramPos);
          if (asic.hasEram) {
            const nextEramPos = (target.readI32(asic.eramPosPtr) - 1) & 0x7ffff;
            target.writeI32(asic.eramPosPtr, nextEramPos);
          }
        }
        pendingJitSamplesRef.current -= 1;
        frame += 1;
      }
    },
    [],
  );

  const startAudio = useCallback(
    async (target: JP8000WasmEmulator) => {
      async function buildJitSnapshotRuntime(emulator: JP8000WasmEmulator) {
        if (!emulator) {
          return;
        }

        try {
          const asics: JitAsicRunners[] = [];
          for (let asic = 0; asic < 4; asic += 1) {
            let core0: JitCoreRunner | null = null;
            let core1: JitCoreRunner | null = null;
            for (let core = 0; core < 2; core += 1) {
              const bytes = emulator.buildEspWasmSnapshot(asic, core);
              const ptrs = Array.from(
                emulator.getEspWasmRuntimePointers(asic, core),
              );
              if (ptrs.length < 12) {
                throw new Error(
                  `Snapshot runtime pointer set is incomplete for asic=${asic} core=${core}`,
                );
              }

              const module = await WebAssembly.compile(bytes as BufferSource);
              const instance = await WebAssembly.instantiate(module, {
                env: {
                  memory: emulator.getWasmMemory(),
                },
              });
              const exports = instance.exports as Record<string, unknown>;
              const run = exports.run;
              if (typeof run !== "function") {
                throw new Error(
                  `Snapshot module does not export run(...) for asic=${asic} core=${core}`,
                );
              }

              const runner: JitCoreRunner = {
                run: run as JitRunFn,
                args: ptrs,
                instance,
              };
              if (core === 0) core0 = runner;
              else core1 = runner;
            }

            if (!core0 || !core1) {
              throw new Error(`Failed to build both cores for asic=${asic}`);
            }

            asics.push({
              core0,
              core1,
              gramPtr: core0.args[2],
              iramPos0Ptr: core0.args[4],
              iramPos1Ptr: core1.args[4],
              eramPosPtr: core0.args[3],
              hasEram: asic === 0 || asic === 3,
            });
          }

          jitRuntimeRef.current = { asics };
          console.log("JIT runtime ready (4 ASICs x 2 cores)");
        } catch (e) {
          console.error("Failed to build JIT snapshot runtime:", e);
        }
      }

      stopAudio();
      const context = await ensureAudioContext();
      const node = context.createScriptProcessor(1024, 0, 2);

      let working = false;

      node.onaudioprocess = async (event) => {
        if (working) return;

        working = true;
        const outL = event.outputBuffer.getChannelData(0);
        const outR = event.outputBuffer.getChannelData(1);
        try {
          const jitRuntime = jitRuntimeRef.current;
          const dirtyMask = emulatorRef.current?.getEspProgramDirtyMask();
          if (
            jitEnabledRef.current &&
            (!jitRuntime || dirtyMask !== 0) &&
            emulatorRef.current
          ) {
            await buildJitSnapshotRuntime(emulatorRef.current);
          }

          if (jitEnabledRef.current && jitRuntime) {
            generateJitSamples(target, jitRuntime, outL, outR);
          } else {
            target.generateSamples(outL, outR);
          }
        } catch (e) {
          outL.fill(0);
          outR.fill(0);
          setError(e instanceof Error ? e.message : String(e));
        }
        working = false;
      };

      node.connect(context.destination);
      audioNodeRef.current = node;
    },
    [ensureAudioContext, generateJitSamples, stopAudio],
  );

  const initEmulator = async () => {
    if (!romFile || !ramFile) {
      setError("Please select both ROM and RAM files before starting.");
      return;
    }

    setError(null);
    setLoading(true);
    try {
      await ensureAudioContext();
      const rom = new Uint8Array(await romFile.arrayBuffer());
      const ram = new Uint8Array(await ramFile.arrayBuffer());
      const next = await createJP8000Emulator(rom, ram);
      jitRuntimeRef.current = null;
      pendingJitSamplesRef.current = 0;
      await startAudio(next);
      drawLcd(next);

      setEmulator((current) => {
        current?.destroy();
        return next;
      });
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e));
    } finally {
      setLoading(false);
    }
  };

  const startDemoSongs = () => {
    if (!emulator) {
      return;
    }

    setError(null);
    try {
      emulator.startDemoSongs();
      if (releaseDemoTimeoutRef.current != null) {
        window.clearTimeout(releaseDemoTimeoutRef.current);
      }
      releaseDemoTimeoutRef.current = window.setTimeout(() => {
        try {
          emulator.stopDemoSongsButton();
        } catch (e) {
          setError(e instanceof Error ? e.message : String(e));
        }
      }, 120);
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e));
    }
  };

  const tapPanelSwitch = (which: number) => {
    if (!emulator) {
      return;
    }

    setError(null);
    try {
      emulator.pressButton(which);
      const timeoutId = window.setTimeout(() => {
        try {
          emulator.releaseButton(which);
        } catch (e) {
          setError(e instanceof Error ? e.message : String(e));
        }
      }, 120);
      switchReleaseTimeoutsRef.current.push(timeoutId);
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e));
    }
  };

  return (
    <div className="app">
      <h1>JP8000 WASM Bridge</h1>
      <div style={{ display: "flex", gap: "16px", marginTop: "12px", alignItems: "center", flexWrap: "wrap" }}>
        <label style={{ fontSize: "12px" }}>
          ROM (.BIN)
          <input
            type="file"
            accept=".BIN,.bin"
            style={{ marginLeft: "6px" }}
            onChange={(e) => setRomFile(e.target.files?.[0] ?? null)}
          />
        </label>
        <label style={{ fontSize: "12px" }}>
          RAM dump (.bin)
          <input
            type="file"
            accept=".bin,.BIN"
            style={{ marginLeft: "6px" }}
            onChange={(e) => setRamFile(e.target.files?.[0] ?? null)}
          />
        </label>
      </div>
      <div style={{ display: "flex", gap: "8px", marginTop: "12px" }}>
        <button
          className="run-button"
          onClick={initEmulator}
          disabled={loading || !romFile || !ramFile}
        >
          Start
        </button>
        <button
          className="run-button"
          onClick={startDemoSongs}
          disabled={!emulator || loading}
        >
          Start Demo Songs
        </button>
        <label
          style={{
            alignSelf: "center",
            fontSize: "12px",
            display: "flex",
            gap: "6px",
            alignItems: "center",
          }}
        >
          <input
            type="checkbox"
            checked={jitEnabled}
            onChange={(e) => (
              setJitEnabled(e.target.checked),
              (jitEnabledRef.current = e.target.checked)
            )}
          />
          Use JIT audio path
        </label>
      </div>
      <div className="output-area">
        <canvas
          ref={canvasRef}
          style={{
            width: "820px",
            height: "100px",
            border: "1px solid #111",
            imageRendering: "pixelated",
            display: "block",
            marginBottom: "12px",
          }}
        />
        <div
          style={{
            display: "flex",
            gap: "8px",
            marginBottom: "12px",
            flexWrap: "wrap",
          }}
        >
          {NUMBER_SWITCHES.map((sw) => (
            <button
              key={sw.value}
              className="run-button"
              onClick={() => tapPanelSwitch(sw.value)}
              disabled={!emulator || loading}
              style={{ marginTop: 0, minWidth: "56px" }}
            >
              {sw.label}
            </button>
          ))}
        </div>
        <PianoKeyboard onMidiMessage={sendMidiBytes} />
        {error && <pre className="output-error">{error}</pre>}
      </div>
    </div>
  );
}
