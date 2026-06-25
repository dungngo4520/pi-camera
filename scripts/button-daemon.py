#!/usr/bin/env python3

import datetime
import gpiod
from gpiod.line import Direction, Edge, Bias
import logging
import os
import signal
import subprocess
import sys
import time
from pathlib import Path

GPIO_CHIP = "/dev/gpiochip0"
BUTTON_LINE = 17
DEBOUNCE_S = 0.3
CAPTURE_SCRIPT = "/home/pi/camera/bin/capture.sh"
PHOTO_DIR = "/home/pi/photos"
ACT_LED_BRIGHTNESS = "/sys/class/leds/ACT/brightness"
ACT_LED_TRIGGER = "/sys/class/leds/ACT/trigger"

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
    handlers=[logging.StreamHandler(sys.stdout)],
)
log = logging.getLogger("picamera-btn")

_led_ok = False


def led_init():
    global _led_ok
    try:
        with open(ACT_LED_TRIGGER, "w") as f:
            f.write("none")
        _led_ok = True
        return True
    except PermissionError:
        log.warning("Cannot control ACT LED (need root) -- LED disabled")
        _led_ok = False
        return False


def _led(val):
    if not _led_ok:
        return
    try:
        with open(ACT_LED_BRIGHTNESS, "w") as f:
            f.write("1" if val else "0")
    except OSError:
        pass


def led_on():
    _led(True)


def led_off():
    _led(False)


def led_blink(n=2, delay=0.1):
    for _ in range(n):
        led_on()
        time.sleep(delay)
        led_off()
        time.sleep(delay)


def led_error():
    led_on()
    time.sleep(1.5)
    led_off()


def capture_photo():
    Path(PHOTO_DIR).mkdir(parents=True, exist_ok=True)
    try:
        result = subprocess.run(
            [CAPTURE_SCRIPT, "--dir", PHOTO_DIR],
            capture_output=True, text=True, timeout=30,
        )
        if result.returncode == 0:
            lines = [l for l in result.stdout.strip().split("\n") if l]
            filename = lines[-1] if lines else "unknown"
            return True, filename
        log.error("Capture failed (exit %d): %s", result.returncode, result.stderr.strip())
        return False, ""
    except subprocess.TimeoutExpired:
        log.error("Capture timed out")
        return False, ""
    except FileNotFoundError:
        log.error("Script not found: %s", CAPTURE_SCRIPT)
        return False, ""


def main():
    running = True

    def handle_signal(signum, frame):
        nonlocal running
        running = False

    signal.signal(signal.SIGTERM, handle_signal)
    signal.signal(signal.SIGINT, handle_signal)

    log.info("Pi Camera button daemon starting (GPIO%d)", BUTTON_LINE)
    led_init()
    led_blink(1, 0.05)

    try:
        request = gpiod.request_lines(
            GPIO_CHIP,
            consumer="picamera-btn",
            config={
                BUTTON_LINE: gpiod.LineSettings(
                    direction=Direction.INPUT,
                    edge_detection=Edge.FALLING,
                    bias=Bias.PULL_UP,
                ),
            },
        )
    except OSError as e:
        log.error("GPIO request failed: %s", e)
        led_error()
        sys.exit(1)

    log.info("Ready! Press button on GPIO%d to capture.", BUTTON_LINE)

    last_press = 0.0
    poll_interval = datetime.timedelta(seconds=0.3)

    while running:
        try:
            has_event = request.wait_edge_events(timeout=poll_interval)
        except Exception:
            time.sleep(0.1)
            continue

        if not has_event:
            continue

        now = time.time()
        if now - last_press < DEBOUNCE_S:
            continue
        last_press = now

        events = request.read_edge_events()
        if not events:
            continue

        log.info("Button pressed -- capturing...")
        if _led_ok:
            led_on()

        success, filename = capture_photo()
        led_off()

        if success:
            log.info("Saved: %s", filename)
            led_blink(2, 0.08)
        else:
            log.error("Capture failed!")
            led_error()

    log.info("Daemon stopped")


if __name__ == "__main__":
    main()
