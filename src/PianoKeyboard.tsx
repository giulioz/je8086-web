import { useCallback, useEffect, useRef, useState } from "react";

type PianoWhiteKey = {
  note: number;
  label: string;
};

type PianoBlackKey = {
  note: number;
  left: number;
};

type PianoKeyboardProps = {
  onMidiMessage: (status: number, data1: number, data2: number) => void;
};

const WHITE_KEY_WIDTH = 52;
const WHITE_KEY_HEIGHT = 180;
const BLACK_KEY_WIDTH = 34;
const BLACK_KEY_HEIGHT = 112;
const WHITE_KEYS: PianoWhiteKey[] = [
  { note: 60, label: "C4" },
  { note: 62, label: "D" },
  { note: 64, label: "E" },
  { note: 65, label: "F" },
  { note: 67, label: "G" },
  { note: 69, label: "A" },
  { note: 71, label: "B" },
  { note: 72, label: "C5" },
  { note: 74, label: "D" },
  { note: 76, label: "E" },
  { note: 77, label: "F" },
  { note: 79, label: "G" },
  { note: 81, label: "A" },
  { note: 83, label: "B" },
];
const BLACK_KEYS: PianoBlackKey[] = [
  { note: 61, left: 35 },
  { note: 63, left: 87 },
  { note: 66, left: 191 },
  { note: 68, left: 243 },
  { note: 70, left: 295 },
  { note: 73, left: 399 },
  { note: 75, left: 451 },
  { note: 78, left: 555 },
  { note: 80, left: 607 },
  { note: 82, left: 659 },
];

export function PianoKeyboard({ onMidiMessage }: PianoKeyboardProps) {
  const activeNotesRef = useRef<Set<number>>(new Set());
  const [activeNotes, setActiveNotes] = useState<Set<number>>(new Set());

  const noteOn = useCallback(
    (note: number, velocity = 100) => {
      if (activeNotesRef.current.has(note)) {
        return;
      }
      const next = new Set(activeNotesRef.current);
      next.add(note);
      activeNotesRef.current = next;
      setActiveNotes(next);
      onMidiMessage(0x90, note, velocity);
    },
    [onMidiMessage],
  );

  const noteOff = useCallback(
    (note: number) => {
      if (!activeNotesRef.current.has(note)) {
        return;
      }
      const next = new Set(activeNotesRef.current);
      next.delete(note);
      activeNotesRef.current = next;
      setActiveNotes(next);
      onMidiMessage(0x80, note, 0);
    },
    [onMidiMessage],
  );

  const releaseAllNotes = useCallback(() => {
    if (activeNotesRef.current.size === 0) {
      return;
    }
    for (const note of activeNotesRef.current) {
      onMidiMessage(0x80, note, 0);
    }
    const next = new Set<number>();
    activeNotesRef.current = next;
    setActiveNotes(next);
  }, [onMidiMessage]);

  useEffect(() => {
    return () => {
      releaseAllNotes();
    };
  }, [releaseAllNotes]);

  useEffect(() => {
    const onPointerRelease = () => {
      releaseAllNotes();
    };

    window.addEventListener("pointerup", onPointerRelease);
    window.addEventListener("pointercancel", onPointerRelease);
    return () => {
      window.removeEventListener("pointerup", onPointerRelease);
      window.removeEventListener("pointercancel", onPointerRelease);
    };
  }, [releaseAllNotes]);

  return (
    <div
      style={{
        position: "relative",
        width: `${WHITE_KEYS.length * WHITE_KEY_WIDTH}px`,
        height: `${WHITE_KEY_HEIGHT}px`,
        marginBottom: "12px",
        touchAction: "none",
        userSelect: "none",
      }}
    >
      {WHITE_KEYS.map((key, index) => {
        const isActive = activeNotes.has(key.note);
        return (
          <button
            key={key.note}
            type="button"
            aria-label={`Play ${key.label}`}
            onPointerDown={() => noteOn(key.note)}
            onPointerUp={() => noteOff(key.note)}
            onPointerLeave={() => noteOff(key.note)}
            style={{
              position: "absolute",
              left: `${index * WHITE_KEY_WIDTH}px`,
              bottom: 0,
              width: `${WHITE_KEY_WIDTH}px`,
              height: `${WHITE_KEY_HEIGHT}px`,
              border: "1px solid #222",
              borderRadius: "0 0 6px 6px",
              background: isActive ? "#d4d4d4" : "#f4f4f4",
              color: "#222",
              display: "flex",
              alignItems: "flex-end",
              justifyContent: "center",
              paddingBottom: "10px",
              fontSize: "12px",
              zIndex: 1,
            }}
          >
            {key.label}
          </button>
        );
      })}
      {BLACK_KEYS.map((key) => {
        const isActive = activeNotes.has(key.note);
        return (
          <button
            key={key.note}
            type="button"
            aria-label={`Play note ${key.note}`}
            onPointerDown={() => noteOn(key.note)}
            onPointerUp={() => noteOff(key.note)}
            onPointerLeave={() => noteOff(key.note)}
            style={{
              position: "absolute",
              left: `${key.left}px`,
              top: 0,
              width: `${BLACK_KEY_WIDTH}px`,
              height: `${BLACK_KEY_HEIGHT}px`,
              border: "1px solid #111",
              borderRadius: "0 0 4px 4px",
              background: isActive ? "#4f4f4f" : "#111",
              color: "#eee",
              zIndex: 2,
            }}
          />
        );
      })}
    </div>
  );
}
