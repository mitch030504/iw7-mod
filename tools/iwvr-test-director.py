#!/usr/bin/env python3
"""User-paced IWVR Phase 2B orientation test director; never sends game input.

Modes (each launched explicitly, never chained):
  --audio-check     speak two test phrases through the headset
  --mp-calibration  motion-driven HMD/game-camera calibration in an EMPTY private MP match
  --zombies-smoke   passive 15-30 s capture while the user plays Zombies normally
  --resume          continue the newest unfinished --mp-calibration run

Every mode waits for ENTER on the desktop before speaking gameplay instructions.
Calibration steps advance only when logged IWVR ORIENTATION samples show the
requested movement, and stop (never skip ahead) on timeout, camera loss, recenter,
or invalid camera output.
"""

import argparse
import datetime as dt
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
GAME_LOG = Path.home() / ".local/share/Steam/steamapps/common/Call of Duty - Infinite Warfare/iw7-mod/logs/iwvr-bootstrap.log"
ARTIFACTS = ROOT / "artifacts/phase2b"
EVENT_LOG = ARTIFACTS / "test-director-events.log"
NODES_LOG = ARTIFACTS / "audio-nodes.txt"

TIMESTAMP = re.compile(r"^(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3})")
VEC = r"\(([^)]*)\)"
ORIENTATION = re.compile(r"IWVR ORIENTATION: cg=(\S+) generation=(\d+) .*?hmd=" + VEC + r" baseForward=" + VEC +
                         r" baseLeft=" + VEC + r" baseUp=" + VEC + r" injectedForward=" + VEC + r" injectedLeft=" + VEC +
                         r" injectedUp=" + VEC + r" D_iw=\(" + VEC + "," + VEC + "," + VEC + r"\) determinant=(\S+) recentered=(\w+)")
VIEWANGLES = re.compile(r"CG_GetPlayerViewOrigin localClientNum=0 returned=true .*?psViewangles=" + VEC)

SAMPLE_STALE_SECONDS = 3.5   # ORIENTATION is logged at 1 Hz while the writer hook runs.
MOVE_CONFIRM_SAMPLES = 2
CENTER_DEGREES = 5.0
GAME_CAMERA_MOVE_DEGREES = 1.5

# Camera-local HMD angles relative to the captured neutral (intrinsic Z-Y-X):
# yaw > 0 turns forward toward IW +Y (left), pitch > 0 raises forward toward +Z,
# roll > 0 raises the left vector toward +Z (head tilted right).
HMD_STEPS = (
    ("HMD_YAW_RIGHT", "Turn your head to the right and hold.", "yaw", -1, 15.0),
    ("HMD_YAW_LEFT", "Turn your head to the left and hold.", "yaw", +1, 15.0),
    ("HMD_PITCH_UP", "Look up and hold.", "pitch", +1, 15.0),
    ("HMD_PITCH_DOWN", "Look down and hold.", "pitch", -1, 15.0),
    ("HMD_ROLL_RIGHT", "Tilt your head slightly to the right and hold.", "roll", +1, 10.0),
    ("HMD_ROLL_LEFT", "Tilt your head slightly to the left and hold.", "roll", -1, 10.0),
)
MP_STEPS = ("NEUTRAL",) + tuple(step[0] for step in HMD_STEPS) + ("GAME_CAMERA", "COMBINED")


def stamp():
    return dt.datetime.now().astimezone().isoformat(timespec="milliseconds")


def record(section, event, phrase):
    EVENT_LOG.parent.mkdir(parents=True, exist_ok=True)
    line = f'{stamp()} mono={time.monotonic():.3f} {section} {event} "{phrase}"\n'
    with EVENT_LOG.open("a", encoding="utf-8") as stream:
        stream.write(line)
    print(line, end="", flush=True)


def command_output(args):
    if not shutil.which(args[0]):
        return f"{args[0]}: unavailable\n"
    result = subprocess.run(args, capture_output=True, text=True, check=False)
    return f"$ {' '.join(args)}\n{result.stdout}{result.stderr}\n"


def find_audio_target():
    reports = [("wpctl status", command_output(["wpctl", "status"])),
               ("wpctl status -n", command_output(["wpctl", "status", "-n"])),
               ("pactl list short sinks", command_output(["pactl", "list", "short", "sinks"])),
               ("pw-cli ls Node", command_output(["pw-cli", "ls", "Node"]))]
    NODES_LOG.parent.mkdir(parents=True, exist_ok=True)
    NODES_LOG.write_text("\n".join(body for _, body in reports), encoding="utf-8")
    pulse = reports[2][1]
    candidates = []
    for line in pulse.splitlines():
        fields = line.split("\t")
        if len(fields) >= 2 and re.search(r"deckard|vrlink|steamvr|valve", fields[1], re.I):
            candidates.append((fields[0], fields[1]))
    if len(candidates) == 1:
        return candidates[0][1]
    status = reports[1][1]
    active = re.findall(r"^\s*\*?\s*(\d+)\.\s+([^\n]+)", status, re.M)
    matches = [(node_id, name) for node_id, name in active if re.search(r"deckard|vrlink|steamvr|valve", name, re.I)]
    if len(matches) == 1:
        return matches[0][1].split()[0]
    raise RuntimeError(f"No unique active Deckard/VR playback sink. Candidates: {candidates}; see {NODES_LOG}")


class Speech:
    def __init__(self, target):
        self.target = target
        self.engine = next((name for name in ("espeak-ng", "espeak") if shutil.which(name)), None)
        self.tts_env = None
        if not self.engine:
            local_root = Path(os.environ.get("IWVR_TTS_ROOT", ARTIFACTS / "tts-local/root"))
            local_engine = local_root / "usr/bin/espeak-ng"
            if local_engine.is_file():
                self.engine = str(local_engine)
                self.tts_env = dict(os.environ,
                                    LD_LIBRARY_PATH=str(local_root / "usr/lib") + ":" + os.environ.get("LD_LIBRARY_PATH", ""),
                                    ESPEAK_DATA_PATH=str(local_root / "usr/share"))
        if not self.engine:
            raise RuntimeError("espeak-ng/espeak unavailable. Install the small native espeak-ng package before testing.")
        self.player = "pw-play" if shutil.which("pw-play") else "paplay" if shutil.which("paplay") else None
        if not self.player:
            raise RuntimeError("Neither pw-play nor paplay is available")
        self.cache = tempfile.TemporaryDirectory(prefix="iwvr-speech-")

    def speak(self, section, event, phrase):
        record(section, event, phrase)
        name = hashlib.sha256(phrase.encode()).hexdigest() + ".wav"
        path = Path(self.cache.name) / name
        if not path.exists():
            result = subprocess.run([self.engine, "-s", "155", "-w", str(path), phrase],
                                    capture_output=True, text=True, env=self.tts_env)
            if result.returncode:
                raise RuntimeError(f"TTS failed: {result.stderr}")
        if self.player == "pw-play":
            args = ["pw-play", "--target", self.target, str(path)]
            env = None
        else:
            args = ["paplay", str(path)]
            env = dict(os.environ, PULSE_SINK=self.target)
        result = subprocess.run(args, capture_output=True, text=True, env=env, timeout=45)
        if result.returncode:
            raise RuntimeError(f"Headset playback failed: {' '.join(args)}: {result.stderr}")


class SilentSpeech:
    """--no-audio: record phrases only (dry runs against a synthetic log)."""

    player = engine = target = "none"

    def speak(self, section, event, phrase):
        record(section, event, phrase)


# ---------------------------------------------------------------- rotation math

def floats(text):
    return [float(value) for value in text.split(",")]


def transpose(a):
    return [[a[col][row] for col in range(3)] for row in range(3)]


def multiply(a, b):
    return [[sum(a[row][k] * b[k][col] for k in range(3)) for col in range(3)] for row in range(3)]


def columns(*vectors):
    return [[vectors[col][row] for col in range(3)] for row in range(3)]


def hmd_angles(r):
    """Yaw, pitch, roll and total rotation (degrees) of a camera-local rotation."""
    yaw = math.degrees(math.atan2(r[1][0], r[0][0]))
    pitch = math.degrees(math.atan2(r[2][0], math.hypot(r[0][0], r[1][0])))
    roll = math.degrees(math.atan2(r[2][1], r[2][2]))
    trace = (r[0][0] + r[1][1] + r[2][2] - 1) / 2
    total = math.degrees(math.acos(max(-1.0, min(1.0, trace))))
    return {"yaw": yaw, "pitch": pitch, "roll": roll, "total": total}


def world_yaw_pitch(forward):
    return (math.degrees(math.atan2(forward[1], forward[0])),
            math.degrees(math.atan2(forward[2], math.hypot(forward[0], forward[1]))))


def wrap(degrees):
    return (degrees + 180.0) % 360.0 - 180.0


def yaw_span(yaws):
    """Range of a yaw sequence after unwrapping across +/-180 degrees."""
    unwrapped = yaws[:1]
    for yaw in yaws[1:]:
        unwrapped.append(unwrapped[-1] + wrap(yaw - unwrapped[-1]))
    return round(max(unwrapped) - min(unwrapped), 2) if unwrapped else None


class Sample:
    def __init__(self, match, log_time):
        self.log_time = log_time
        self.cg = match.group(1).lower()
        self.generation = int(match.group(2))
        self.hmd = floats(match.group(3))
        self.base = columns(floats(match.group(4)), floats(match.group(5)), floats(match.group(6)))
        self.injected = columns(floats(match.group(7)), floats(match.group(8)), floats(match.group(9)))
        self.d_iw = [floats(match.group(10)), floats(match.group(11)), floats(match.group(12))]
        self.determinant = float(match.group(13))
        self.recentered = match.group(14) == "yes"
        self.relative = None  # D relative to the captured neutral; set by the director.
        self.angles = None

    def writer_error(self):
        """Largest difference between the written axes and base * D_iw (log has 4 decimals)."""
        expected = multiply(self.base, self.d_iw)
        return max(abs(expected[r][c] - self.injected[r][c]) for r in range(3) for c in range(3))

    def base_forward(self):
        return [self.base[row][0] for row in range(3)]

    def injected_forward(self):
        return [self.injected[row][0] for row in range(3)]

    def summary(self):
        base_yaw, base_pitch = world_yaw_pitch(self.base_forward())
        injected_yaw, injected_pitch = world_yaw_pitch(self.injected_forward())
        result = {"log_time": self.log_time, "generation": self.generation, "hmd": self.hmd,
                  "base_yaw": round(base_yaw, 2), "base_pitch": round(base_pitch, 2),
                  "injected_yaw": round(injected_yaw, 2), "injected_pitch": round(injected_pitch, 2),
                  "determinant": self.determinant, "writer_error": round(self.writer_error(), 5)}
        if self.angles:
            result["relative"] = {key: round(value, 2) for key, value in self.angles.items()}
        return result


class LogMonitor:
    """Tails the game's bootstrap log from the director's start position."""

    def __init__(self, path):
        self.path = path
        try:
            stat = path.stat()
            self.file_id, self.position = (stat.st_dev, stat.st_ino), stat.st_size
        except FileNotFoundError:
            self.file_id, self.position = None, 0
        self.partial = ""
        self.latest = None
        self.latest_received = 0.0
        self.invalid_output = 0
        self.errors = []
        self.viewangles = []

    def poll(self):
        """Return new ORIENTATION samples; also tracks errors and psViewangles."""
        try:
            stat = self.path.stat()
            file_id = (stat.st_dev, stat.st_ino)
            if file_id != self.file_id or stat.st_size < self.position:
                self.file_id, self.position, self.partial = file_id, 0, ""
            with self.path.open("r", encoding="utf-8", errors="replace") as stream:
                stream.seek(self.position)
                chunk = stream.read()
                self.position = stream.tell()
        except FileNotFoundError:
            return []
        lines = (self.partial + chunk).split("\n")
        self.partial = lines.pop()
        samples = []
        for line in lines:
            match_time = TIMESTAMP.match(line)
            log_time = match_time.group(1) if match_time else ""
            match = ORIENTATION.search(line)
            if match:
                samples.append(Sample(match, log_time))
                continue
            angles = VIEWANGLES.search(line)
            if angles:
                self.viewangles.append((log_time, floats(angles.group(1))))
            if "IWVR camera invalid:" in line or "XR_ERROR" in line:
                self.errors.append(line.strip())
                if "output basis" in line:
                    self.invalid_output += 1
                record("MONITOR", "ERROR", line.strip())
            elif "IWVR camera recentered" in line:
                record("MONITOR", "RECENTER", line.strip())
        if samples:
            self.latest = samples[-1]
            self.latest_received = time.monotonic()
        return samples

    def age(self):
        return time.monotonic() - self.latest_received if self.latest else math.inf


class Stop(Exception):
    """Calibration must not progress; carries the spoken reason."""

    def __init__(self, event, phrase, detail):
        super().__init__(detail)
        self.event, self.phrase, self.detail = event, phrase, detail


# ---------------------------------------------------------------- director

class Director:
    def __init__(self, speech, monitor, section, step_timeout, results_path, results):
        self.speech = speech
        self.monitor = monitor
        self.section = section
        self.step_timeout = step_timeout
        self.results_path = results_path
        self.results = results
        self.cg = None
        self.neutral = None

    def say(self, event, phrase):
        self.speech.speak(self.section, event, phrase)

    def save(self):
        self.results_path.parent.mkdir(parents=True, exist_ok=True)
        temporary = self.results_path.with_suffix(".tmp")
        temporary.write_text(json.dumps(self.results, indent=2) + "\n", encoding="utf-8")
        temporary.replace(self.results_path)

    def apply_neutral(self, sample):
        if self.neutral is not None:
            sample.relative = multiply(transpose(self.neutral), sample.d_iw)
            sample.angles = hmd_angles(sample.relative)

    def check(self, sample):
        if self.cg is None:
            self.cg = sample.cg
        if sample.cg != self.cg or sample.recentered:
            raise Stop("RECENTERED", "The camera was recentered. Calibration stopped.",
                       f"cg {self.cg} -> {sample.cg}, recentered={sample.recentered} at {sample.log_time}")
        if sample.writer_error() > 0.002 or abs(sample.determinant - 1) > 0.01:
            raise Stop("BAD_OUTPUT", "Camera output problem detected. Calibration stopped.",
                       f"writer_error={sample.writer_error():.5f} determinant={sample.determinant} at {sample.log_time}")
        self.apply_neutral(sample)

    def health(self):
        if self.monitor.invalid_output >= 3:
            raise Stop("BAD_OUTPUT", "Camera output problem detected. Calibration stopped.",
                       "repeated 'IWVR camera invalid: output basis' lines")
        if self.monitor.age() > SAMPLE_STALE_SECONDS:
            raise Stop("CAMERA_INACTIVE", "Head tracking camera is not active. Calibration stopped.",
                       f"no IWVR ORIENTATION sample for {self.monitor.age():.1f} s (death, menu, or killcam)")

    def wait(self, name, predicate, needed, timeout=None):
        """Advance only after `needed` consecutive samples satisfy `predicate`."""
        deadline = time.monotonic() + (timeout or self.step_timeout)
        streak = []
        while True:
            for sample in self.monitor.poll():
                self.check(sample)
                streak = streak + [sample] if predicate(sample) else []
                if len(streak) >= needed:
                    return streak
            self.health()
            if time.monotonic() > deadline:
                raise Stop("TIMEOUT", "Step not detected. Calibration stopped.",
                           f"{name}: condition not met within {timeout or self.step_timeout:.0f} s")
            time.sleep(0.1)

    def wait_for_fresh_sample(self, timeout=5.0):
        self.monitor.poll()  # Discard everything logged while the user was at the ENTER prompt.
        self.monitor.latest = None
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            samples = self.monitor.poll()
            if samples:
                self.cg = samples[-1].cg
                return samples[-1]
            time.sleep(0.1)
        raise Stop("CAMERA_INACTIVE", "Head tracking camera is not active. Calibration not started.",
                   "no IWVR ORIENTATION sample after ENTER; is IWVR running with -vrhead in stable gameplay?")

    def countdown(self, seconds):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            for sample in self.monitor.poll():
                self.check(sample)
            self.health()
            time.sleep(0.1)

    def capture_neutral(self):
        self.say("NEUTRAL BEGIN", "Look comfortably forward and hold still.")

        previous = [None]

        def steady(sample):  # Within 2 degrees of the previous sample.
            last, previous[0] = previous[0], sample
            return last is not None and hmd_angles(multiply(transpose(last.d_iw), sample.d_iw))["total"] < 2.0

        streak = self.wait("NEUTRAL", steady, 2)
        self.neutral = streak[-1].d_iw
        offset = hmd_angles(self.neutral)
        self.results["neutral"] = {"log_time": streak[-1].log_time, "d_iw": self.neutral,
                                   "offset_from_recenter_baseline": {k: round(v, 2) for k, v in offset.items()}}
        record(self.section, "NEUTRAL END", json.dumps(self.results["neutral"]["offset_from_recenter_baseline"]))

    def hmd_step(self, name, instruction, axis, sign, threshold):
        self.say(name + " MOVE", instruction)
        streak = self.wait(name, lambda s: s.angles["total"] >= threshold, MOVE_CONFIRM_SAMPLES)
        peak = max(streak, key=lambda s: s.angles["total"])
        angles = peak.angles
        dominant = max(("yaw", "pitch", "roll"), key=lambda key: abs(angles[key]))
        verdict = "PASS" if dominant == axis and angles[axis] * sign > 0 else "FAIL"
        base_yaw, base_pitch = world_yaw_pitch(peak.base_forward())
        injected_yaw, injected_pitch = world_yaw_pitch(peak.injected_forward())
        step = {"expected": f"{'+' if sign > 0 else '-'}{axis}", "dominant": dominant, "verdict": verdict,
                "peak": peak.summary(),
                "world_injected_minus_base": {"yaw": round(wrap(injected_yaw - base_yaw), 2),
                                              "pitch": round(injected_pitch - base_pitch, 2)}}
        record(self.section, name + " DETECTED", json.dumps(step))
        if verdict == "PASS":
            self.say(name + " CENTER", "Good. Return to center.")
        else:
            self.say(name + " CENTER", "Movement detected, but the direction did not match. Return to center.")
        back = self.wait(name + " CENTER", lambda s: s.angles["total"] <= CENTER_DEGREES, MOVE_CONFIRM_SAMPLES)
        step["returned"] = back[-1].summary()
        return step

    def game_camera_capture(self, name, head_ok, needed, timeout=60.0):
        moving = []
        previous = [None]

        def moved(sample):
            last, previous[0] = previous[0], sample
            if last is None or not head_ok(sample):
                return False
            a = world_yaw_pitch(last.base_forward())
            b = world_yaw_pitch(sample.base_forward())
            if max(abs(wrap(b[0] - a[0])), abs(b[1] - a[1])) >= GAME_CAMERA_MOVE_DEGREES:
                moving.append(sample)
            return len(moving) >= needed

        viewangles_start = len(self.monitor.viewangles)
        self.wait(name, moved, 1, timeout=timeout)
        base_yaws = [world_yaw_pitch(s.base_forward())[0] for s in moving]
        offsets = [wrap(world_yaw_pitch(s.injected_forward())[0] - world_yaw_pitch(s.base_forward())[0]) for s in moving]
        views = self.monitor.viewangles[viewangles_start:]
        return {"moving_samples": len(moving),
                "base_yaw_span": yaw_span(base_yaws),
                "head_yaw_relative": [round(s.angles["yaw"], 2) for s in moving],
                "world_injected_minus_base_yaw": [round(value, 2) for value in offsets],
                "writer_error_max": round(max(s.writer_error() for s in moving), 5),
                "psViewangles_samples": len(views),
                "psViewangles_yaw_span": yaw_span([v[1][1] for v in views]),
                "samples": [s.summary() for s in moving]}

    def complete(self, step, data):
        self.results["steps"][step] = data
        self.results["completed"].append(step)
        self.save()

    def run_mp(self):
        todo = [step for step in MP_STEPS if step not in self.results["completed"]]
        self.capture_neutral()  # Always re-captured: the user may have moved since a stopped run.
        if "NEUTRAL" in todo:
            self.complete("NEUTRAL", self.results["neutral"])
        for name, instruction, axis, sign, threshold in HMD_STEPS:
            if name in todo:
                self.complete(name, self.hmd_step(name, instruction, axis, sign, threshold))
        if "GAME_CAMERA" in todo:
            self.say("HMD DONE", "Head tracking test complete.")
            self.say("GAME_CAMERA HEAD", "Keep your head comfortably forward.")
            self.say("GAME_CAMERA BEGIN", "Use the mouse or right stick to look around normally.")
            data = self.game_camera_capture("GAME_CAMERA", lambda s: s.angles["total"] <= 10.0, 10)
            self.say("GAME_CAMERA END", "Game camera test captured. Stop moving the camera.")
            self.complete("GAME_CAMERA", data)
        if "COMBINED" in todo:
            self.say("COMBINED HEAD", "Turn your head slightly right.")
            head = self.wait("COMBINED HEAD", lambda s: s.angles["yaw"] <= -10.0, MOVE_CONFIRM_SAMPLES)
            self.say("COMBINED ROTATE", "Keep your head there and rotate the game camera slowly.")
            data = self.game_camera_capture("COMBINED", lambda s: s.angles["yaw"] <= -5.0, 8)
            data["head_detected"] = head[-1].summary()
            self.say("COMBINED CENTER", "Return your head to center.")
            back = self.wait("COMBINED CENTER", lambda s: s.angles["total"] <= CENTER_DEGREES, MOVE_CONFIRM_SAMPLES)
            data["returned"] = back[-1].summary()
            self.complete("COMBINED", data)
        self.results["status"] = "complete"
        self.save()
        self.say("DONE", "Multiplayer calibration complete.")

    def run_zombies_smoke(self, minimum=20.0, maximum=30.0):
        self.say("ACTIVE", "Zombies smoke test active.")
        start = time.monotonic()
        samples = [self.monitor.latest] if self.monitor.latest else []
        ended = "duration"
        while time.monotonic() - start < maximum:
            for sample in self.monitor.poll():
                samples.append(sample)
            if time.monotonic() - start >= minimum and len(samples) >= 15:
                break
            if self.monitor.age() > SAMPLE_STALE_SECONDS:
                ended = "camera inactive (death, menu, or killcam)"
                break
            time.sleep(0.1)
        cgs = sorted({s.cg for s in samples})
        self.results.update({
            "status": "complete", "ended_by": ended, "seconds": round(time.monotonic() - start, 1),
            "samples": len(samples), "cg": cgs, "recenters": sum(s.recentered for s in samples),
            "determinant_range": [min(s.determinant for s in samples), max(s.determinant for s in samples)] if samples else None,
            "writer_error_max": round(max(s.writer_error() for s in samples), 5) if samples else None,
            "camera_invalid_lines": self.monitor.errors,
            "sample_summaries": [s.summary() for s in samples]})
        self.save()
        self.say("DONE", "Zombies smoke test captured. You may exit the match.")


def enter_gate(prompt):
    print(f"\n{prompt}", flush=True)
    try:
        input()
    except EOFError:
        raise RuntimeError("stdin closed before ENTER; run the director in an interactive terminal")


def newest_unfinished_run():
    runs = sorted(ARTIFACTS.glob("runs/mp-calibration-*.json"))
    for path in reversed(runs):
        data = json.loads(path.read_text(encoding="utf-8"))
        if data.get("status") != "complete":
            return path, data
    raise RuntimeError("No unfinished --mp-calibration run to resume")


def main():
    global EVENT_LOG, ARTIFACTS
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--audio-check", action="store_true")
    mode.add_argument("--mp-calibration", action="store_true")
    mode.add_argument("--zombies-smoke", action="store_true")
    mode.add_argument("--resume", action="store_true")
    parser.add_argument("--countdown", type=float, default=5.0, help="seconds between ENTER and the first instruction")
    parser.add_argument("--step-timeout", type=float, default=30.0)
    parser.add_argument("--game-log", type=Path, default=GAME_LOG)
    parser.add_argument("--artifacts", type=Path, default=ARTIFACTS, help="event log and run results directory")
    parser.add_argument("--no-audio", action="store_true", help="record phrases without speaking (dry runs only)")
    args = parser.parse_args()
    ARTIFACTS = args.artifacts
    EVENT_LOG = ARTIFACTS / "test-director-events.log"

    if args.no_audio:
        speech = SilentSpeech()
    else:
        speech = Speech(find_audio_target())
    print(f"TTS={speech.engine} target={speech.target} player={speech.player}", flush=True)
    if args.audio_check:
        speech.speak("SYSTEM", "AUDIO_CHECK_1", "IWVR test audio check.")
        speech.speak("SYSTEM", "AUDIO_CHECK_2", "You will hear spoken instructions during the test.")
        return 0

    run_id = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    if args.resume:
        results_path, results = newest_unfinished_run()
        results.setdefault("resumed", []).append(run_id)
        section = "MULTIPLAYER"
        prompt = (f"Resuming {results_path.name} (completed: {', '.join(results['completed']) or 'none'}).\n"
                  "Press ENTER when you are alone in the private MP match and ready.")
    elif args.mp_calibration:
        results_path = ARTIFACTS / f"runs/mp-calibration-{run_id}.json"
        results = {"mode": "mp-calibration", "run_id": run_id, "status": "started", "completed": [], "steps": {},
                   "game_log": str(args.game_log)}
        section = "MULTIPLAYER"
        prompt = "Press ENTER when you are alone in the private MP match and ready."
    else:
        results_path = ARTIFACTS / f"runs/zombies-smoke-{run_id}.json"
        results = {"mode": "zombies-smoke", "run_id": run_id, "status": "started", "game_log": str(args.game_log)}
        section = "ZOMBIES"
        prompt = "Press ENTER when you are in the private Zombies match and ready."

    EVENT_LOG = results_path.with_name(results_path.stem + "-events.log")
    monitor = LogMonitor(args.game_log)
    director = Director(speech, monitor, section, args.step_timeout, results_path, results)
    record(section, "START", f"{results['mode']} {results_path.name}")
    enter_gate(prompt)
    try:
        director.wait_for_fresh_sample()
        if results["mode"] == "zombies-smoke":
            director.run_zombies_smoke()
        else:
            director.say("COUNTDOWN", "Calibration will begin in five seconds.")
            director.countdown(args.countdown)
            director.run_mp()
    except Stop as stop:
        results["status"] = "stopped"
        results.setdefault("stops", []).append({"time": stamp(), "event": stop.event, "detail": stop.detail})
        director.save()
        record(section, "STOP " + stop.event, stop.detail)
        speech.speak(section, "STOP", stop.phrase)
        print(f"\nStopped: {stop.detail}\nResults: {results_path}\nResume with: {sys.argv[0]} --resume", flush=True)
        return 2
    print(f"\nResults: {results_path}", flush=True)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (RuntimeError, subprocess.TimeoutExpired) as exc:
        print(f"IWVR test director stopped: {exc}\nAudio candidates: {NODES_LOG}", file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        record("SYSTEM", "INTERRUPTED", "operator pressed Ctrl+C")
        sys.exit(130)
