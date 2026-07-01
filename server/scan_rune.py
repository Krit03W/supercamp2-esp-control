#!/usr/bin/env python3
"""Krit AI rune scanner.

Reads an image, tries AprilTag 16h5 first, then optionally falls back to
OpenAI vision classification when OPENAI_API_KEY is available.
"""

from __future__ import annotations

import argparse
import base64
import json
import mimetypes
import os
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any


RUNE_LINES = {
    1: "1Wake the stone where the first fire sleeps.",
    2: "2Raise the light and let its signal speak.",
    3: "3Follow the path where green lines bend.",
    4: "4Feed the power before shadows descend.",
    5: "5Trust the shield when danger is near.",
    6: "6Return to home, and the exit will appear.",
}

TAG_TO_RUNE = {
    0: 1,
    1: 2,
    2: 3,
    3: 4,
    4: 5,
    5: 6,
}

RUNE_HINTS = """
Rune 1: orange fire rune, inverted triangle on a long vertical staff, two small diagonal side marks, small dot below.
Rune 2: cyan blue rune, central ring or circle, three short rays above, two horizontal base bars.
Rune 3: green rune, long zigzag lightning/path line, small dots near the ends, one short mark near the top.
Rune 4: orange fire rune, diamond shape in the center, vertical lower stem splitting into three prongs, short side bars.
Rune 5: yellow-green rune, large arch or shield/portal, two glowing dots at the arch feet, short vertical mark in the center.
Rune 6: blue crescent rune, crescent moon or horn shape, small square core in the middle, thin vertical stem downward.
""".strip()


def parse_env_line(line: str) -> tuple[str, str] | None:
    line = line.strip()
    if not line or line.startswith("#") or "=" not in line:
        return None
    key, value = line.split("=", 1)
    key = key.strip()
    value = value.strip().strip('"').strip("'")
    if not key:
        return None
    return key, value


def load_env_file(path: Path) -> None:
    if not path.exists():
        return
    for line in path.read_text(encoding="utf-8").splitlines():
        parsed = parse_env_line(line)
        if not parsed:
            continue
        key, value = parsed
        os.environ.setdefault(key, value)


def load_local_env() -> None:
    here = Path(__file__).resolve().parent
    load_env_file(here.parent / ".env")
    load_env_file(here / ".env")


@dataclass
class ScanResult:
    ok: bool
    rune: int | None
    answer: str | None
    method: str
    status: str
    tag_id: int | None = None
    confidence: float | None = None
    reason: str | None = None

    def to_dict(self) -> dict[str, Any]:
        return {
            "ok": self.ok,
            "rune": self.rune,
            "answer": self.answer,
            "method": self.method,
            "status": self.status,
            "tag_id": self.tag_id,
            "confidence": self.confidence,
            "reason": self.reason,
        }


def polygon_area(points: Any) -> float:
    pts = points.reshape(-1, 2)
    area = 0.0
    for i in range(len(pts)):
        x1, y1 = pts[i]
        x2, y2 = pts[(i + 1) % len(pts)]
        area += float(x1 * y2 - x2 * y1)
    return abs(area) / 2.0


def scan_apriltag(image_path: Path) -> ScanResult | None:
    try:
        import cv2
    except Exception as exc:
        return ScanResult(
            ok=False,
            rune=None,
            answer=None,
            method="apriltag",
            status=f"OpenCV unavailable: {exc}",
        )

    img = cv2.imread(str(image_path))
    if img is None:
        return ScanResult(
            ok=False,
            rune=None,
            answer=None,
            method="apriltag",
            status="Image could not be read.",
        )

    gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
    aruco = cv2.aruco
    dict_id = getattr(aruco, "DICT_APRILTAG_16h5", None)
    if dict_id is None:
        dict_id = getattr(aruco, "DICT_APRILTAG_16H5")
    dictionary = aruco.getPredefinedDictionary(dict_id)
    params = aruco.DetectorParameters()
    if hasattr(aruco, "CORNER_REFINE_APRILTAG"):
        params.cornerRefinementMethod = aruco.CORNER_REFINE_APRILTAG

    if hasattr(aruco, "ArucoDetector"):
        detector = aruco.ArucoDetector(dictionary, params)
        corners, ids, _ = detector.detectMarkers(gray)
    else:
        corners, ids, _ = aruco.detectMarkers(gray, dictionary, parameters=params)

    if ids is None or len(ids) == 0:
        return None

    candidates = []
    for marker_id, marker_corners in zip(ids.flatten().tolist(), corners):
        candidates.append((polygon_area(marker_corners), int(marker_id)))
    _, tag_id = max(candidates, key=lambda item: item[0])
    rune = TAG_TO_RUNE.get(tag_id)
    if rune is None:
        return ScanResult(
            ok=False,
            rune=None,
            answer=None,
            method="apriltag",
            status=f"AprilTag 16h5 ID {tag_id} is not mapped to a rune.",
            tag_id=tag_id,
        )

    return ScanResult(
        ok=True,
        rune=rune,
        answer=RUNE_LINES[rune],
        method="apriltag",
        status="AprilTag matched.",
        tag_id=tag_id,
        confidence=1.0,
    )


def image_data_url(image_path: Path) -> str:
    mime = mimetypes.guess_type(str(image_path))[0] or "image/jpeg"
    data = base64.b64encode(image_path.read_bytes()).decode("ascii")
    return f"data:{mime};base64,{data}"


def classify_with_openai(image_path: Path, question: str) -> ScanResult:
    load_local_env()
    api_key = os.environ.get("OPENAI_API_KEY")
    if not api_key:
        return ScanResult(
            ok=False,
            rune=None,
            answer=None,
            method="openai",
            status="AprilTag not found and OPENAI_API_KEY is not set.",
        )

    try:
        from openai import OpenAI
    except Exception as exc:
        return ScanResult(
            ok=False,
            rune=None,
            answer=None,
            method="openai",
            status=f"OpenAI SDK unavailable: {exc}",
        )

    client = OpenAI(api_key=api_key)
    model = os.environ.get("OPENAI_MODEL", "gpt-4o-mini")
    prompt = (
        "Classify the rune poster in this image as exactly one rune number from 1 to 6. "
        "Use the visual rune shape and color. If an AprilTag is visible, you may use it, "
        "but the main task is to choose the matching rune class. "
        "If the image is unrelated or too unclear, return rune 0.\n\n"
        f"{RUNE_HINTS}\n\n"
        f"Student question: {question or 'What rune is in this image?'}"
    )

    schema = {
        "type": "object",
        "additionalProperties": False,
        "properties": {
            "rune": {"type": "integer", "enum": [0, 1, 2, 3, 4, 5, 6]},
            "confidence": {"type": "number"},
            "reason": {"type": "string"},
        },
        "required": ["rune", "confidence", "reason"],
    }

    try:
        response = client.chat.completions.create(
            model=model,
            temperature=0,
            messages=[
                {
                    "role": "system",
                    "content": "You are Krit AI, a concise rune-classification assistant.",
                },
                {
                    "role": "user",
                    "content": [
                        {"type": "text", "text": prompt},
                        {
                            "type": "image_url",
                            "image_url": {"url": image_data_url(image_path)},
                        },
                    ],
                },
            ],
            response_format={
                "type": "json_schema",
                "json_schema": {
                    "name": "rune_classification",
                    "schema": schema,
                    "strict": True,
                },
            },
        )
        content = response.choices[0].message.content or "{}"
        parsed = json.loads(content)
    except Exception as exc:
        return ScanResult(
            ok=False,
            rune=None,
            answer=None,
            method="openai",
            status=f"OpenAI fallback failed: {exc}",
        )

    rune = int(parsed.get("rune", 0))
    confidence = parsed.get("confidence")
    reason = str(parsed.get("reason", "")).strip() or None
    if rune not in RUNE_LINES:
        return ScanResult(
            ok=False,
            rune=None,
            answer=None,
            method="openai",
            status="The image was too unclear to match a rune.",
            confidence=float(confidence or 0),
            reason=reason,
        )

    return ScanResult(
        ok=True,
        rune=rune,
        answer=RUNE_LINES[rune],
        method="openai",
        status="OpenAI fallback matched.",
        confidence=float(confidence or 0),
        reason=reason,
    )


def scan_rune(image_path: Path, question: str = "", allow_openai: bool = True) -> ScanResult:
    if not image_path.exists():
        return ScanResult(
            ok=False,
            rune=None,
            answer=None,
            method="input",
            status=f"Image file not found: {image_path}",
        )

    tag_result = scan_apriltag(image_path)
    if tag_result and tag_result.ok:
        return tag_result
    if tag_result and tag_result.method == "apriltag" and tag_result.status.startswith("Image could not"):
        return tag_result
    if not allow_openai:
        return ScanResult(
            ok=False,
            rune=None,
            answer=None,
            method="apriltag",
            status="AprilTag not found.",
        )
    return classify_with_openai(image_path, question)


def main() -> int:
    parser = argparse.ArgumentParser(description="Krit AI rune scanner")
    parser.add_argument("image", help="Path to the image to scan")
    parser.add_argument("--question", default="", help="Optional student question")
    parser.add_argument("--json", action="store_true", help="Print JSON instead of only the rune answer")
    parser.add_argument("--no-openai", action="store_true", help="Disable OpenAI fallback")
    args = parser.parse_args()

    result = scan_rune(Path(args.image), args.question, allow_openai=not args.no_openai)
    if args.json:
        print(json.dumps(result.to_dict(), ensure_ascii=False))
    elif result.ok and result.answer:
        print(result.answer)
    else:
        print(result.status, file=sys.stderr)
        return 1
    return 0 if result.ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
