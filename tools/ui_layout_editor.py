#!/usr/bin/env python3
"""Run the standalone Darkest Dungeon UI layout editor.

The editor intentionally uses only Python's standard library.  It scans a
folder of prepared images, serves them to a small browser editor, and writes a
portable JSON layout file containing association ids and component geometry.
"""

from __future__ import annotations

import argparse
import json
import mimetypes
import re
import sys
import threading
import webbrowser
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import unquote, urlparse


ID_PATTERN = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
IMAGE_EXTENSIONS = {".png", ".jpg", ".jpeg", ".webp", ".gif", ".bmp", ".svg"}
DEFAULT_LAYOUT = {
    "version": 1,
    "canvas": {"width": 0, "height": 0},
    "associations": [],
    "components": [],
}


def load_presets(path: Path) -> list[dict[str, str]]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, list):
        raise ValueError("preset file must contain an array")
    result: list[dict[str, str]] = []
    seen: set[str] = set()
    for item in value:
        if not isinstance(item, dict):
            raise ValueError("preset must be an object")
        identifier, name, placement = item.get("id"), item.get("name"), item.get("placement", "free")
        if not isinstance(identifier, str) or not ID_PATTERN.fullmatch(identifier):
            raise ValueError(f"invalid preset id: {identifier!r}")
        if identifier in seen:
            raise ValueError(f"duplicate preset id: {identifier}")
        if not isinstance(name, str) or not name.strip():
            raise ValueError(f"preset {identifier} needs a name")
        if placement not in {"free", "background", "bottom_bar"}:
            raise ValueError(f"invalid preset placement: {placement!r}")
        result.append({"id": identifier, "name": name, "placement": placement})
        seen.add(identifier)
    return result


def resolve_inside(root: Path, relative: str) -> Path | None:
    candidate = (root / Path(relative)).resolve()
    try:
        candidate.relative_to(root.resolve())
    except ValueError:
        return None
    return candidate


def validate_layout(value: object) -> tuple[dict | None, str | None]:
    if not isinstance(value, dict):
        return None, "layout must be a JSON object"
    canvas = value.get("canvas", {})
    if not isinstance(canvas, dict):
        return None, "canvas must be an object"
    canvas_width, canvas_height = canvas.get("width"), canvas.get("height")
    if not isinstance(canvas_width, (int, float)) or not isinstance(canvas_height, (int, float)):
        return None, "canvas width and height must be numbers"
    if canvas_width < 0 or canvas_height < 0 or (canvas_width == 0) != (canvas_height == 0):
        return None, "canvas width and height must both be positive or both be zero"
    associations = value.get("associations", [])
    components = value.get("components", [])
    if not isinstance(associations, list) or not isinstance(components, list):
        return None, "associations and components must be arrays"
    association_ids: set[str] = set()
    for association in associations:
        if not isinstance(association, dict):
            return None, "association must be an object"
        identifier = association.get("id")
        name = association.get("name")
        if not isinstance(identifier, str) or not ID_PATTERN.fullmatch(identifier):
            return None, f"invalid association id: {identifier!r}"
        if identifier in association_ids:
            return None, f"duplicate association id: {identifier}"
        if not isinstance(name, str) or not name.strip():
            return None, f"association {identifier} needs a name"
        placement = association.get("placement", "free")
        if placement not in {"free", "background", "bottom_bar"}:
            return None, f"invalid placement for association {identifier}"
        association_ids.add(identifier)
    component_ids: set[str] = set()
    background_count = 0
    for component in components:
        if not isinstance(component, dict):
            return None, "component must be an object"
        identifier = component.get("associationId")
        asset = component.get("assetPath")
        if not isinstance(identifier, str) or identifier not in association_ids:
            return None, f"component references unknown association: {identifier!r}"
        if not isinstance(asset, str) or not asset or Path(asset).is_absolute() or ".." in Path(asset).parts:
            return None, f"invalid component asset path: {asset!r}"
        if identifier in component_ids:
            return None, f"duplicate component association: {identifier}"
        component_ids.add(identifier)
        placement = next(item["placement"] for item in associations if item["id"] == identifier)
        numbers = (component.get(key) for key in ("x", "y", "width", "height", "zIndex"))
        if not all(isinstance(number, (int, float)) for number in numbers):
            return None, f"component {identifier} has invalid geometry"
        if component["width"] <= 0 or component["height"] <= 0:
            return None, f"component {identifier} must have a positive size"
        if placement == "background":
            background_count += 1
            if (component["x"] != 0 or component["y"] != 0 or
                    component["width"] != canvas_width or component["height"] != canvas_height):
                return None, "background must fill the canvas from its top-left corner"
        if placement == "bottom_bar":
            if (component["x"] != 0 or component["width"] != canvas_width or
                    abs(component["y"] + component["height"] - canvas_height) > 0.01):
                return None, "bottom_bar must span the canvas width and touch its bottom edge"
    if background_count != 1:
        return None, "a layout must contain exactly one background component"
    return value, None


class EditorHandler(BaseHTTPRequestHandler):
    server_version = "DDSELayoutEditor/1.0"

    @property
    def editor_server(self) -> "EditorServer":
        return self.server  # type: ignore[return-value]

    def log_message(self, format: str, *args: object) -> None:
        sys.stderr.write("[ui-layout-editor] " + (format % args) + "\n")

    def send_json(self, payload: object, status: HTTPStatus = HTTPStatus.OK) -> None:
        body = json.dumps(payload, ensure_ascii=False, indent=2).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self) -> None:  # noqa: N802
        parsed = urlparse(self.path)
        if parsed.path == "/api/presets":
            self.send_json(self.editor_server.presets)
            return
        if parsed.path == "/api/assets":
            self.send_json(self.scan_assets())
            return
        if parsed.path == "/api/layout":
            if self.editor_server.output_path.is_file():
                try:
                    self.send_json(json.loads(self.editor_server.output_path.read_text(encoding="utf-8")))
                    return
                except (OSError, json.JSONDecodeError) as error:
                    self.send_json({"error": f"unable to read layout: {error}"}, HTTPStatus.INTERNAL_SERVER_ERROR)
                    return
            self.send_json(DEFAULT_LAYOUT)
            return
        if parsed.path.startswith("/asset/"):
            relative = unquote(parsed.path.removeprefix("/asset/"))
            asset = resolve_inside(self.editor_server.assets_root, relative)
            if asset is None or not asset.is_file() or asset.suffix.lower() not in IMAGE_EXTENSIONS:
                self.send_error(HTTPStatus.NOT_FOUND)
                return
            try:
                body = asset.read_bytes()
            except OSError:
                self.send_error(HTTPStatus.NOT_FOUND)
                return
            self.send_response(HTTPStatus.OK)
            self.send_header("Content-Type", mimetypes.guess_type(asset.name)[0] or "application/octet-stream")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        self.serve_static(parsed.path)

    def do_POST(self) -> None:  # noqa: N802
        parsed = urlparse(self.path)
        if parsed.path != "/api/layout":
            self.send_error(HTTPStatus.NOT_FOUND)
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
            value = json.loads(self.rfile.read(length).decode("utf-8"))
        except (ValueError, json.JSONDecodeError, UnicodeDecodeError) as error:
            self.send_json({"error": f"invalid JSON: {error}"}, HTTPStatus.BAD_REQUEST)
            return
        filename = value.pop("filename", None) if isinstance(value, dict) else None
        checked, error = validate_layout(value)
        if error:
            self.send_json({"error": error}, HTTPStatus.BAD_REQUEST)
            return
        assert checked is not None
        for component in checked["components"]:
            if resolve_inside(self.editor_server.assets_root, component["assetPath"]) is None:
                self.send_json({"error": f"asset escapes configured folder: {component['assetPath']}"}, HTTPStatus.BAD_REQUEST)
                return
        output_path, filename_error = self.editor_server.resolve_output(filename)
        if filename_error:
            self.send_json({"error": filename_error}, HTTPStatus.BAD_REQUEST)
            return
        assert output_path is not None
        try:
            output_path.parent.mkdir(parents=True, exist_ok=True)
            temporary = output_path.with_suffix(output_path.suffix + ".tmp")
            temporary.write_text(json.dumps(checked, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
            temporary.replace(output_path)
        except OSError as error:
            self.send_json({"error": f"unable to save layout: {error}"}, HTTPStatus.INTERNAL_SERVER_ERROR)
            return
        self.send_json({"saved": True, "path": str(output_path)})

    def scan_assets(self) -> list[dict[str, str | int]]:
        assets: list[dict[str, str | int]] = []
        if not self.editor_server.assets_root.is_dir():
            return assets
        for path in sorted(self.editor_server.assets_root.rglob("*"), key=lambda item: item.as_posix().lower()):
            if not path.is_file() or path.suffix.lower() not in IMAGE_EXTENSIONS:
                continue
            relative = path.relative_to(self.editor_server.assets_root).as_posix()
            try:
                stat = path.stat()
            except OSError:
                continue
            assets.append({
                "path": relative,
                "name": path.stem,
                "url": "/asset/" + relative,
                "size": stat.st_size,
                "modified": stat.st_mtime_ns,
            })
        return assets

    def serve_static(self, requested: str) -> None:
        relative = "index.html" if requested in {"", "/"} else requested.removeprefix("/")
        static_file = resolve_inside(self.editor_server.static_root, relative)
        if static_file is None or not static_file.is_file():
            self.send_error(HTTPStatus.NOT_FOUND)
            return
        try:
            body = static_file.read_bytes()
        except OSError:
            self.send_error(HTTPStatus.NOT_FOUND)
            return
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", mimetypes.guess_type(static_file.name)[0] or "application/octet-stream")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


class EditorServer(ThreadingHTTPServer):
    def __init__(self, address: tuple[str, int], assets_root: Path, output_path: Path, static_root: Path, presets_path: Path):
        super().__init__(address, EditorHandler)
        self.assets_root = assets_root.resolve()
        self.output_path = output_path.resolve()
        self.static_root = static_root.resolve()
        self.presets = load_presets(presets_path.resolve())

    def resolve_output(self, filename: object) -> tuple[Path | None, str | None]:
        if filename is None or filename == "":
            return self.output_path, None
        if not isinstance(filename, str) or not filename.strip():
            return None, "保存文件名不能为空"
        candidate = Path(filename.strip())
        if candidate.name != filename.strip() or candidate.name in {".", ".."}:
            return None, "保存文件名只能是文件名，不能包含目录"
        if candidate.suffix.lower() != ".json":
            candidate = candidate.with_suffix(candidate.suffix + ".json" if candidate.suffix else ".json")
        if any(char in candidate.name for char in '<>:"/\\|?*'):
            return None, "保存文件名包含系统不允许的字符"
        return (self.output_path.parent / candidate.name).resolve(), None


def main() -> int:
    parser = argparse.ArgumentParser(description="Run the DDSE standalone UI layout editor")
    parser.add_argument("--assets-dir", type=Path, default=Path("ui-art-assets"), help="folder containing prepared images")
    parser.add_argument("--output", type=Path, default=Path("ui-layout.json"), help="layout JSON output path")
    parser.add_argument("--preset-file", type=Path, default=Path(__file__).resolve().parent / "ui_layout_editor" / "presets.json", help="association preset JSON file")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--no-browser", action="store_true", help="do not open the editor automatically")
    args = parser.parse_args()
    assets_root = args.assets_dir.resolve()
    assets_root.mkdir(parents=True, exist_ok=True)
    static_root = Path(__file__).resolve().parent / "ui_layout_editor"
    server = EditorServer((args.host, args.port), assets_root, args.output, static_root, args.preset_file)
    url = f"http://{args.host}:{server.server_port}/"
    print(f"UI layout editor: {url}")
    print(f"Assets: {assets_root}")
    print(f"Output: {args.output.resolve()}")
    if not args.no_browser:
        threading.Timer(0.2, lambda: webbrowser.open(url)).start()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nUI layout editor stopped")
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
