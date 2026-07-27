#!/usr/bin/env python3
"""Interactive ROI editor for Jetson Inspect.

Drag P1/P2/P3/P4 on the live camera frame, press S to save.
Saved points are in 1920x1080 reference coordinates so the C++ app can scale
them for 1280x720 or other runtime resolutions.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path

import cv2
import numpy as np

from capture_roi_tool import (
    BASE_ROI_POINTS,
    BASE_SIZE,
    DST_SIZE,
    draw_overlay,
    env_int,
    env_str,
    grab_frame,
    open_camera,
    scaled_roi_points,
    threshold_roi,
    warp_roi,
)


WINDOW = "ROI Editor - drag points, S save, Q quit"
PREVIEW = "ROI Preview"


def parse_points_file(path: Path) -> np.ndarray | None:
    if not path.exists():
        return None
    values: list[tuple[float, float]] = []
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.split("#", 1)[0].replace(",", " ").strip()
        if not line:
            continue
        parts = line.split()
        if len(parts) < 2:
            continue
        try:
            values.append((float(parts[0]), float(parts[1])))
        except ValueError:
            continue
        if len(values) == 4:
            return np.array(values, dtype=np.float32)
    return None


def save_points_file(path: Path, base_points: np.ndarray) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    lines = [
        "# ROI source points in 1920x1080 reference coordinates.",
        "# Order: P1 top-left, P2 top-right, P3 bottom-right, P4 bottom-left.",
    ]
    lines.extend(f"{p[0]:.0f},{p[1]:.0f}" for p in base_points)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def frame_to_base_points(frame_points: np.ndarray, frame_w: int, frame_h: int) -> np.ndarray:
    scale = np.array([BASE_SIZE[0] / frame_w, BASE_SIZE[1] / frame_h], dtype=np.float32)
    return frame_points * scale


def clamp_points(points: np.ndarray, frame_w: int, frame_h: int) -> np.ndarray:
    points[:, 0] = np.clip(points[:, 0], 0, frame_w - 1)
    points[:, 1] = np.clip(points[:, 1], 0, frame_h - 1)
    return points


def draw_editor(frame: np.ndarray, points: np.ndarray, selected: int, saved: bool) -> np.ndarray:
    canvas = draw_overlay(frame, points)
    if selected >= 0:
        p = tuple(np.round(points[selected]).astype(int))
        cv2.circle(canvas, p, 16, (255, 0, 255), 3, cv2.LINE_AA)

    status = "S save | R reset | Q quit"
    if saved:
        status = "Saved. Restart app to use new ROI. " + status
    cv2.rectangle(canvas, (8, 8), (720, 44), (0, 0, 0), -1)
    cv2.putText(canvas, status, (18, 34), cv2.FONT_HERSHEY_SIMPLEX, 0.72,
                (255, 255, 255), 2, cv2.LINE_AA)
    return canvas


class EditorState:
    def __init__(self, points: np.ndarray, frame_shape: tuple[int, int, int]):
        self.points = points
        self.selected = -1
        self.dragging = False
        self.saved = False
        self.frame_h, self.frame_w = frame_shape[:2]

    def nearest_point(self, x: int, y: int) -> int:
        target = np.array([x, y], dtype=np.float32)
        distances = np.linalg.norm(self.points - target, axis=1)
        idx = int(np.argmin(distances))
        return idx if distances[idx] <= 45.0 else -1


def mouse_callback(event: int, x: int, y: int, _flags: int, state: EditorState) -> None:
    if event == cv2.EVENT_LBUTTONDOWN:
        state.selected = state.nearest_point(x, y)
        if state.selected >= 0:
            state.dragging = True
    elif event == cv2.EVENT_MOUSEMOVE and state.dragging and state.selected >= 0:
        state.points[state.selected] = [x, y]
        clamp_points(state.points, state.frame_w, state.frame_h)
        state.saved = False
    elif event == cv2.EVENT_LBUTTONUP:
        state.dragging = False


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Drag ROI points and save config/roi_points.txt")
    parser.add_argument("--points-file", default="config/roi_points.txt")
    parser.add_argument("--backend", default=env_str("JETSON_CAMERA_BACKEND", env_str("OPENCV_TAIL_CAMERA_BACKEND", "v4l2")))
    parser.add_argument("--index", type=int, default=env_int("JETSON_CAM_DEVICE", env_int("OPENCV_TAIL_CAMERA_INDEX", 0)))
    parser.add_argument("--width", type=int, default=env_int("JETSON_CAM_WIDTH", env_int("OPENCV_TAIL_CAMERA_WIDTH", 1280)))
    parser.add_argument("--height", type=int, default=env_int("JETSON_CAM_HEIGHT", env_int("OPENCV_TAIL_CAMERA_HEIGHT", 720)))
    parser.add_argument("--fps", type=int, default=env_int("JETSON_CAM_FPS", env_int("OPENCV_TAIL_CAMERA_FPS", 60)))
    parser.add_argument("--warmup", type=int, default=20)
    parser.add_argument("--live", action="store_true", help="Keep updating camera frame while editing")
    parser.add_argument("--threshold", type=int, default=170)
    return parser


def main() -> int:
    args = build_parser().parse_args()
    points_file = Path(args.points_file)
    loaded_base = parse_points_file(points_file)
    if loaded_base is None:
        loaded_base = BASE_ROI_POINTS.copy()

    cap = open_camera(args)
    try:
        frame = grab_frame(cap, args.warmup)
        frame_h, frame_w = frame.shape[:2]
        state = EditorState(scaled_roi_points(frame_w, frame_h, loaded_base), frame.shape)

        cv2.namedWindow(WINDOW, cv2.WINDOW_NORMAL)
        cv2.namedWindow(PREVIEW, cv2.WINDOW_NORMAL)
        cv2.setMouseCallback(WINDOW, mouse_callback, state)

        print("ROI editor ready.")
        print("Drag P1/P2/P3/P4 with mouse. Press S to save, R reset, Q quit.")
        print(f"Saving to: {points_file}")

        while True:
            if args.live and not state.dragging:
                ok, live_frame = cap.read()
                if ok and live_frame is not None and live_frame.size:
                    frame = live_frame

            editor = draw_editor(frame, state.points.copy(), state.selected, state.saved)
            roi = warp_roi(frame, state.points, DST_SIZE)
            thresh = threshold_roi(roi, args.threshold)
            thresh_bgr = cv2.cvtColor(thresh, cv2.COLOR_GRAY2BGR)

            cv2.imshow(WINDOW, editor)
            cv2.imshow(PREVIEW, np.hstack([roi, thresh_bgr]))
            key = cv2.waitKey(20) & 0xFF
            if key in (ord("q"), 27):
                break
            if key == ord("r"):
                state.points = scaled_roi_points(frame_w, frame_h, BASE_ROI_POINTS)
                state.saved = False
            if key == ord("s"):
                base_points = frame_to_base_points(state.points, frame_w, frame_h)
                save_points_file(points_file, base_points)
                state.saved = True
                print("Saved ROI points:")
                for idx, p in enumerate(base_points, start=1):
                    print(f"  P{idx}: {p[0]:.0f},{p[1]:.0f}")
        return 0
    finally:
        cap.release()
        cv2.destroyAllWindows()


if __name__ == "__main__":
    raise SystemExit(main())
