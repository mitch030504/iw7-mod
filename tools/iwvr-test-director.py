#!/usr/bin/env python3
"""Speak and timestamp private IWVR Phase 2B camera tests; never send game input."""

import argparse
import datetime as dt
import hashlib
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
EVENT_LOG = ROOT / "artifacts/phase2b/test-director-events.log"
NODES_LOG = ROOT / "artifacts/phase2b/audio-nodes.txt"
CG_PATTERN = re.compile(r"IWVR camera recentered cg=(0x[0-9A-Fa-f]+)")
ORIENTATION_PATTERN = re.compile(r"IWVR ORIENTATION: cg=(0x[0-9A-Fa-f]+)")


def stamp():
    return dt.datetime.now().astimezone().isoformat(timespec="milliseconds")


def record(section, event, phrase):
    EVENT_LOG.parent.mkdir(parents=True, exist_ok=True)
    line = f'{stamp()} mono={time.monotonic():.3f} {section} {event} "{phrase}"\n'
    with EVENT_LOG.open("a", encoding="utf-8") as stream:
        stream.write(line)
        stream.flush()
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
            local_root = Path(os.environ.get("IWVR_TTS_ROOT", ROOT / "artifacts/phase2b/tts-local/root"))
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


class LogMonitor:
    def __init__(self):
        try:
            stat = GAME_LOG.stat()
            self.file_id = (stat.st_dev, stat.st_ino)
            self.position = stat.st_size  # Ignore earlier runs in an append-only game log.
        except FileNotFoundError:
            self.file_id = None
            self.position = 0
        self.last_cg = None
        self.counts = {}
        self.latest_orientation_at = 0
        self.latest_tracking_error_at = 0
        self.bad_matrix_count = 0
        self.milestones = set()

    def poll(self):
        try:
            stat = GAME_LOG.stat()
            file_id = (stat.st_dev, stat.st_ino)
            if file_id != self.file_id or stat.st_size < self.position:
                self.file_id, self.position = file_id, 0
                self.counts.clear()
                self.last_cg = None
            with GAME_LOG.open("r", encoding="utf-8", errors="replace") as stream:
                stream.seek(self.position)
                lines = stream.readlines()
                self.position = stream.tell()
        except FileNotFoundError:
            return
        for line in lines:
            for label, pattern in (("fallback", "fallback"), ("session", "xrBeginSession"),
                                   ("locate", "first xrLocateViews success"),
                                   ("installed", "IWVR camera injection installed")):
                if pattern in line:
                    self.milestones.add(label)
            recenter = CG_PATTERN.search(line)
            if recenter:
                self.last_cg = recenter.group(1).lower()
                self.counts[self.last_cg] = 0
                record("MONITOR", "RECENTER", line.strip())
            orientation = ORIENTATION_PATTERN.search(line)
            if orientation:
                cg = orientation.group(1).lower()
                self.counts[cg] = self.counts.get(cg, 0) + 1
                self.latest_orientation_at = time.monotonic()
            if "IWVR camera invalid: output basis" in line:
                self.bad_matrix_count += 1
            if "IWVR camera invalid: HMD orientation" in line:
                self.latest_tracking_error_at = time.monotonic()
            if "IWVR camera invalid:" in line or "XR_ERROR" in line:
                record("MONITOR", "ERROR", line.strip())

    def ready(self, previous_cg=None):
        cg = self.last_cg
        return (cg is not None and cg != previous_cg and self.counts.get(cg, 0) >= 3
                and time.monotonic() - self.latest_orientation_at < 3
                and {"session", "locate", "installed"}.issubset(self.milestones))


def wait_for_ready(speech, monitor, section, previous_cg=None):
    while True:
        monitor.poll()
        if monitor.bad_matrix_count >= 3:
            abort(speech, "repeated invalid camera matrices")
        if monitor.ready(previous_cg):
            return monitor.last_cg
        time.sleep(0.2)


def pause(monitor, speech, seconds):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        monitor.poll()
        if monitor.bad_matrix_count >= 3:
            abort(speech, "repeated invalid camera matrices")
        if monitor.latest_tracking_error_at and time.monotonic() - monitor.latest_orientation_at > 6:
            abort(speech, "sustained invalid HMD tracking")
        time.sleep(0.2)


def abort(speech, reason):
    record("SYSTEM", "ABORT_REASON", reason)
    speech.speak("SYSTEM", "ABORT", "IWVR test problem detected.")
    speech.speak("SYSTEM", "REMOVE_HEADSET", "Please remove the headset.")
    raise RuntimeError(reason)


def motion(speech, monitor, section, name, instruction, hold=4):
    speech.speak(section, name + " BEGIN", instruction)
    pause(monitor, speech, hold)
    speech.speak(section, name + " END", "Return your head to center.")
    pause(monitor, speech, 4)


def game_motion(speech, monitor, section, name, instruction):
    speech.speak(section, name + " BEGIN", instruction)
    pause(monitor, speech, 8)
    speech.speak(section, name + " END", "Stop camera movement.")
    pause(monitor, speech, 3)


def run(speech):
    monitor = LogMonitor()
    speech.speak("SYSTEM", "AUDIO_CHECK_1", "IWVR test audio check.")
    speech.speak("SYSTEM", "AUDIO_CHECK_2", "You will hear spoken instructions during the test.")
    time.sleep(2)
    for event, phrase in (("READY", "IWVR camera test ready."), ("SEATED", "Remain seated for this test."),
                          ("INPUT", "Keep a controller or mouse within reach."),
                          ("COMFORT", "If anything feels uncomfortable, remove the headset."),
                          ("INSTRUCTIONS", "I will speak each instruction. You do not need to read the computer."),
                          ("ENTER", "Enter the requested private game normally.")):
        speech.speak("INTRO", event, phrase)
    speech.speak("ZOMBIES", "ENTER", "Please enter a Zombies custom or private match.")
    zombies_cg = wait_for_ready(speech, monitor, "ZOMBIES")
    speech.speak("ZOMBIES", "READY", "Zombies camera is ready.")
    speech.speak("ZOMBIES", "FORWARD", "Face comfortably forward.")
    speech.speak("ZOMBIES", "STILL", "Do not move the mouse or right stick.")
    pause(monitor, speech, 3)
    speech.speak("ZOMBIES", "NEUTRAL BEGIN", "Hold your head comfortably forward.")
    pause(monitor, speech, 4)
    record("ZOMBIES", "NEUTRAL END", "")
    for name, phrase in (("HMD_YAW_RIGHT", "Turn your head slowly to the right and hold."),
                         ("HMD_YAW_LEFT", "Turn your head slowly to the left and hold."),
                         ("HMD_PITCH_UP", "Look upward and hold."),
                         ("HMD_PITCH_DOWN", "Look downward and hold."),
                         ("HMD_ROLL_RIGHT", "Tilt your head slightly to the right and hold."),
                         ("HMD_ROLL_LEFT", "Tilt your head slightly to the left and hold.")):
        motion(speech, monitor, "ZOMBIES", name, phrase)
    speech.speak("ZOMBIES", "HMD_DONE", "Head movement test complete.")
    speech.speak("ZOMBIES", "HEAD_FORWARD", "Keep your head facing forward.")
    pause(monitor, speech, 3)
    game_motion(speech, monitor, "ZOMBIES", "GAME_CAMERA_YAW", "Now use the mouse or right stick to look slowly right and left.")
    game_motion(speech, monitor, "ZOMBIES", "GAME_CAMERA_PITCH", "Now use the mouse or right stick to look slowly up and down.")
    speech.speak("ZOMBIES", "COMBINED_INTRO", "Combined test.")
    speech.speak("ZOMBIES", "COMBINED_HEAD", "Turn your head slightly right and keep it there.")
    pause(monitor, speech, 3)
    speech.speak("ZOMBIES", "COMBINED BEGIN", "While holding your head there, slowly rotate the game camera with the mouse or right stick.")
    pause(monitor, speech, 8)
    speech.speak("ZOMBIES", "COMBINED END", "Stop camera movement and return your head to center.")
    pause(monitor, speech, 4)
    speech.speak("ZOMBIES", "DONE", "Zombies test complete.")
    speech.speak("MULTIPLAYER", "MENU", "Please return to the menu.")
    speech.speak("MULTIPLAYER", "ENTER", "Then enter a multiplayer custom or private match.")
    speech.speak("MULTIPLAYER", "PRIVATE", "Do not use public matchmaking yet.")
    wait_for_ready(speech, monitor, "MULTIPLAYER", zombies_cg)
    speech.speak("MULTIPLAYER", "READY", "Multiplayer camera is ready.")
    speech.speak("MULTIPLAYER", "STILL", "Keep the mouse or right stick still.")
    for name, phrase in (("HMD_YAW_RIGHT", "Turn your head slowly to the right and hold."),
                         ("HMD_YAW_LEFT", "Turn your head slowly to the left and hold."),
                         ("HMD_PITCH_UP", "Look upward and hold."),
                         ("HMD_PITCH_DOWN", "Look downward and hold.")):
        motion(speech, monitor, "MULTIPLAYER", name, phrase)
    speech.speak("MULTIPLAYER", "HEAD_FORWARD", "Keep your head forward.")
    game_motion(speech, monitor, "MULTIPLAYER", "GAME_CAMERA_YAW", "Use the mouse or right stick to look right and left.")
    speech.speak("MULTIPLAYER", "COMBINED_HEAD", "Turn your head slightly right.")
    pause(monitor, speech, 3)
    speech.speak("MULTIPLAYER", "COMBINED BEGIN", "Now rotate the game camera slowly.")
    pause(monitor, speech, 8)
    speech.speak("MULTIPLAYER", "COMBINED END", "Stop and return your head to center.")
    speech.speak("MULTIPLAYER", "DONE", "Multiplayer test complete.")
    speech.speak("SYSTEM", "DONE", "The IWVR Phase 2B test is finished.")
    speech.speak("SYSTEM", "REMOVE_HEADSET", "You can remove the headset.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--audio-check-only", action="store_true")
    args = parser.parse_args()
    target = find_audio_target()
    speech = Speech(target)
    print(f"TTS={speech.engine} target={target} player={speech.player}", flush=True)
    if args.audio_check_only:
        speech.speak("SYSTEM", "AUDIO_CHECK_1", "IWVR test audio check.")
        speech.speak("SYSTEM", "AUDIO_CHECK_2", "You will hear spoken instructions during the test.")
    else:
        run(speech)


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.TimeoutExpired) as exc:
        print(f"IWVR test director stopped: {exc}\nAudio candidates: {NODES_LOG}", file=sys.stderr)
        sys.exit(1)
