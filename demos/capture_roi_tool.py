#!/usr/bin/env python3
"""Capture full frame + current ROI images for calibration/debugging."""

from __future__ import annotations

import argparse
import os
import sys
import time
from datetime import datetime
from pathlib import Path

import cv2
import numpy as np


BASE_ROI_POINTS = np.array(
    [[604, 421], [1327, 403], [1338, 943], [595, 973]], dtype=np.float32
)
BASE_SIZE = (1920.0, 1080.0)
DST_SIZE = (1080, 804)
FAST_DST_SIZE = (540, 402)


def env_int(name: str, fallback: int) -> int:
    value = os.environ.get(name)
    if not value:
        return fallback
    try:
        return int(value)
    except ValueError:
        return fallback


def env_str(name: str, fallback: str) -> str:
    value = os.environ.get(name)
    return value if value else fallback


def parse_points(value: str | None) -> np.ndarray:
    if not value:
        return BASE_ROI_POINTS.copy()
    parts = value.replace(";", " ").split()
    if len(parts) != 4:
        raise ValueError("--points must contain exactly 4 points: x,y x,y x,y x,y")
    points = []
    for part in parts:
        xy = part.split(",")
        if len(xy) != 2:
            raise ValueError("invalid point format, expected x,y")
        points.append([float(xy[0]), float(xy[1])])
    return np.array(points, dtype=np.float32)


def load_points_file(path: str) -> np.ndarray | None:
    file_path = Path(path)
    if not file_path.exists():
        return None
    points = []
    for raw in file_path.read_text(encoding="utf-8").splitlines():
        line = raw.split("#", 1)[0].replace(",", " ").strip()
        if not line:
            continue
        parts = line.split()
        if len(parts) < 2:
            continue
        try:
            points.append([float(parts[0]), float(parts[1])])
        except ValueError:
            continue
        if len(points) == 4:
            return np.array(points, dtype=np.float32)
    return None


def get_base_points(args: argparse.Namespace) -> np.ndarray:
    if args.points:
        return parse_points(args.points)
    loaded = load_points_file(args.points_file)
    return loaded if loaded is not None else BASE_ROI_POINTS.copy()


def scaled_roi_points(frame_width: int, frame_height: int, base_points: np.ndarray) -> np.ndarray:
    scale = np.array([frame_width / BASE_SIZE[0], frame_height / BASE_SIZE[1]], dtype=np.float32)
    return base_points * scale


def open_camera(args: argparse.Namespace) -> cv2.VideoCapture:
    backend = args.backend.lower()
    if backend in ("v4l2", "usb"):
        cap = cv2.VideoCapture(args.index, cv2.CAP_V4L2)
    else:
        cap = cv2.VideoCapture(args.index)

    cap.set(cv2.CAP_PROP_FRAME_WIDTH, args.width)
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, args.height)
    cap.set(cv2.CAP_PROP_FPS, args.fps)
    cap.set(cv2.CAP_PROP_BUFFERSIZE, 1)
    if not cap.isOpened():
        raise RuntimeError(f"Cannot open camera index={args.index} backend={args.backend}")
    return cap


def grab_frame(cap: cv2.VideoCapture, warmup: int) -> np.ndarray:
    frame = None
    for _ in range(max(1, warmup)):
        ok, current = cap.read()
        if ok and current is not None and current.size:
            frame = current
        time.sleep(0.01)
    if frame is None:
        raise RuntimeError("Camera returned empty frame")
    return frame


def draw_overlay(frame: np.ndarray, points: np.ndarray) -> np.ndarray:
    overlay = frame.copy()
    pts = np.round(points).astype(np.int32)
    cv2.polylines(overlay, [pts], True, (0, 255, 255), 3, cv2.LINE_AA)
    for idx, pt in enumerate(pts, start=1):
        x, y = int(pt[0]), int(pt[1])
        cv2.circle(overlay, (x, y), 9, (0, 0, 255), -1, cv2.LINE_AA)
        label = f"P{idx} ({x},{y})"
        cv2.putText(
            overlay,
            label,
            (x + 12, y - 12),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.65,
            (0, 0, 0),
            4,
            cv2.LINE_AA,
        )
        cv2.putText(
            overlay,
            label,
            (x + 12, y - 12),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.65,
            (255, 255, 255),
            2,
            cv2.LINE_AA,
        )
    return overlay


def warp_roi(frame: np.ndarray, points: np.ndarray, size: tuple[int, int]) -> np.ndarray:
    w, h = size
    dst = np.array([[0, 0], [w, 0], [w, h], [0, h]], dtype=np.float32)
    matrix = cv2.getPerspectiveTransform(points.astype(np.float32), dst)
    return cv2.warpPerspective(frame, matrix, (w, h))


def threshold_roi(roi: np.ndarray, threshold: int) -> np.ndarray:
    gray = cv2.cvtColor(roi, cv2.COLOR_BGR2GRAY)
    blurred = cv2.GaussianBlur(gray, (11, 11), 2.5)
    _, thresh = cv2.threshold(blurred, threshold, 255, cv2.THRESH_BINARY)
    close_kernel = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (7, 7))
    open_kernel = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (9, 9))
    thresh = cv2.morphologyEx(thresh, cv2.MORPH_CLOSE, close_kernel, iterations=2)
    thresh = cv2.morphologyEx(thresh, cv2.MORPH_OPEN, open_kernel, iterations=2)
    return thresh


def wait_for_trigger(args: argparse.Namespace) -> None:
    try:
        import Jetson.GPIO as GPIO
    except Exception as exc:  # pragma: no cover - Jetson-only path
        raise RuntimeError(f"Jetson.GPIO import failed: {exc}") from exc

    GPIO.setwarnings(False)
    GPIO.setmode(GPIO.BOARD)
    pull = GPIO.PUD_DOWN if args.trigger_active_high else GPIO.PUD_UP
    GPIO.setup(args.trigger_pin, GPIO.IN, pull_up_down=pull)
    try:
        active_state = GPIO.HIGH if args.trigger_active_high else GPIO.LOW
        idle_state = GPIO.LOW if args.trigger_active_high else GPIO.HIGH
        deadline = time.monotonic() + args.trigger_timeout
        last_state = GPIO.input(args.trigger_pin)
        print(
            f"Waiting trigger BOARD {args.trigger_pin} active={'HIGH' if args.trigger_active_high else 'LOW'}...",
            flush=True,
        )
        while time.monotonic() < deadline:
            state = GPIO.input(args.trigger_pin)
            if state == active_state and last_state == idle_state:
                time.sleep(args.debounce_ms / 1000.0)
                if GPIO.input(args.trigger_pin) == active_state:
                    return
            last_state = state
            time.sleep(0.001)
        raise TimeoutError("Timed out waiting for trigger")
    finally:
        GPIO.cleanup(args.trigger_pin)


def save_capture(frame: np.ndarray, args: argparse.Namespace, capture_index: int) -> Path:
    ts = datetime.now().strftime("%Y%m%d_%H%M%S_%f")[:-3]
    out_dir = Path(args.out) / f"roi_capture_{ts}_{capture_index:02d}"
    out_dir.mkdir(parents=True, exist_ok=True)

    frame_h, frame_w = frame.shape[:2]
    base_points = get_base_points(args)
    points = scaled_roi_points(frame_w, frame_h, base_points)
    roi = warp_roi(frame, points, DST_SIZE)
    roi_fast = warp_roi(frame, points, FAST_DST_SIZE)
    overlay = draw_overlay(frame, points)
    thresh = threshold_roi(roi_fast if args.fast_threshold else roi, args.threshold)

    cv2.imwrite(str(out_dir / "01_full_frame.jpg"), frame)
    cv2.imwrite(str(out_dir / "02_roi_overlay.jpg"), overlay)
    cv2.imwrite(str(out_dir / "03_roi_warp.jpg"), roi)
    cv2.imwrite(str(out_dir / "04_roi_warp_fast.jpg"), roi_fast)
    cv2.imwrite(str(out_dir / "05_threshold.jpg"), thresh)

    metadata = out_dir / "metadata.txt"
    with metadata.open("w", encoding="utf-8") as f:
        f.write(f"captured_at={ts}\n")
        f.write(f"camera={frame_w}x{frame_h}\n")
        f.write(f"backend={args.backend}\n")
        f.write(f"index={args.index}\n")
        f.write(f"fps={args.fps}\n")
        f.write(f"threshold={args.threshold}\n")
        f.write("base_points_1920x1080=" + " ".join(f"{p[0]:.0f},{p[1]:.0f}" for p in base_points) + "\n")
        f.write("scaled_points_frame=" + " ".join(f"{p[0]:.1f},{p[1]:.1f}" for p in points) + "\n")
        f.write("files=01_full_frame.jpg 02_roi_overlay.jpg 03_roi_warp.jpg 04_roi_warp_fast.jpg 05_threshold.jpg\n")
    return out_dir


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Capture frame and ROI images for ROI calibration")
    parser.add_argument("--out", default="debug/roi_captures", help="Output directory")
    parser.add_argument("--backend", default=env_str("JETSON_CAMERA_BACKEND", env_str("OPENCV_TAIL_CAMERA_BACKEND", "v4l2")))
    parser.add_argument("--index", type=int, default=env_int("JETSON_CAM_DEVICE", env_int("OPENCV_TAIL_CAMERA_INDEX", 0)))
    parser.add_argument("--width", type=int, default=env_int("JETSON_CAM_WIDTH", env_int("OPENCV_TAIL_CAMERA_WIDTH", 1280)))
    parser.add_argument("--height", type=int, default=env_int("JETSON_CAM_HEIGHT", env_int("OPENCV_TAIL_CAMERA_HEIGHT", 720)))
    parser.add_argument("--fps", type=int, default=env_int("JETSON_CAM_FPS", env_int("OPENCV_TAIL_CAMERA_FPS", 60)))
    parser.add_argument("--warmup", type=int, default=20, help="Frames to discard before saving")
    parser.add_argument("--count", type=int, default=1, help="Number of captures")
    parser.add_argument("--interval", type=float, default=0.5, help="Delay between captures")
    parser.add_argument("--threshold", type=int, default=170)
    parser.add_argument("--fast-threshold", action="store_true", help="Threshold the half-res ROI like fast trigger path")
    parser.add_argument("--points", help="Override ROI source points in 1920x1080 coords: 'x,y x,y x,y x,y'")
    parser.add_argument("--points-file", default="config/roi_points.txt", help="ROI points file saved by roi_editor_tool.py")
    parser.add_argument("--wait-trigger", action="store_true", help="Capture when GPIO trigger edge arrives")
    parser.add_argument("--trigger-pin", type=int, default=22)
    parser.add_argument("--trigger-active-high", action="store_true", default=True)
    parser.add_argument("--trigger-active-low", action="store_false", dest="trigger_active_high")
    parser.add_argument("--debounce-ms", type=float, default=40.0)
    parser.add_argument("--trigger-timeout", type=float, default=30.0)
    return parser


def main() -> int:
    args = build_parser().parse_args()
    cap = None
    try:
        if args.wait_trigger:
            print("Stop jetson_inspect_v2 before using --wait-trigger to avoid GPIO conflict.", flush=True)
        cap = open_camera(args)
        saved = []
        for i in range(1, args.count + 1):
            if args.wait_trigger:
                wait_for_trigger(args)
            frame = grab_frame(cap, args.warmup)
            out_dir = save_capture(frame, args, i)
            saved.append(out_dir)
            print(f"Saved ROI capture: {out_dir}", flush=True)
            if i < args.count:
                time.sleep(args.interval)
        print("Send these files back: 01_full_frame.jpg, 02_roi_overlay.jpg, 03_roi_warp.jpg, metadata.txt")
        return 0
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    finally:
        if cap is not None:
            cap.release()


if __name__ == "__main__":
    raise SystemExit(main())
