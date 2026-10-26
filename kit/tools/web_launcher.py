#!/usr/bin/env python3
"""Build the web launcher: the hub page for one or more games.

    tools/web_launcher.py --game-dir /abs/game-a [--game-dir /abs/game-b ...] --out <dir>
        [--web-build <game id>=<build dir> ...] [--stream <game id> ...] [--serve [port]]

Writes <dir>/index.html, app.js, core.js, worker.js and games.json (each game's
title, executable, digest, required folders, exclusions and store link from its
game.toml). A game's web build, when one exists, goes in <dir>/<game id>/.
--web-build copies a game's web build (build/web/recomp: index.html and the
app's .js/.wasm/.data) there. Serve <dir> over HTTPS or from localhost with
COOP/COEP headers (the game needs a secure, cross-origin isolated context);
--serve runs such a server for local testing."""

import argparse
import fnmatch
import functools
import http.server
import json
import os
from pathlib import Path
import re
import shutil
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import game_config  # noqa: E402

PAGE_FILES = ("index.html", "app.js", "core.js", "worker.js")


def game_entry(cfg):
    game, launcher = cfg["game"], cfg["launcher"]
    return {
        "id": game["id"],
        "title": launcher["title"],
        "executable": game["executable"],
        "sha256": game["sha256"],
        "requiredDirs": cfg["setup"]["required_dirs"],
        "exclude": cfg["bundle"]["exclude"],
        "store": launcher["store"],
        "installNames": launcher["install_names"],
        "minFreeMb": launcher["min_free_mb"],
    }


def build(game_dirs, out):
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    games = [game_entry(game_config.load(d)) for d in game_dirs]
    ids = [g["id"] for g in games]
    if len(set(ids)) != len(ids):
        raise ValueError("two games share an id: %s" % ", ".join(ids))
    for name in PAGE_FILES:
        shutil.copy2(ROOT / "web/launcher" / name, out / name)
    (out / "games.json").write_text(json.dumps(games, indent=2) + "\n")
    return games


WEB_BUILD_SUFFIXES = (".js", ".wasm", ".data")


def copy_web_build(game_id, build, out):
    build, dest = Path(build), Path(out) / game_id
    if not (build / "index.html").is_file():
        raise ValueError("no web build in %s (index.html is missing)" % build)
    dest.mkdir(parents=True, exist_ok=True)
    for f in build.iterdir():
        if f.is_file() and (f.name == "index.html" or f.suffix in WEB_BUILD_SUFFIXES):
            shutil.copy2(f, dest / f.name)


def excluded(relative, patterns):
    """core.js excluded(): a pattern names the top folder or the file."""
    parts = relative.lower().split("/")
    return any(fnmatch.fnmatchcase(part, p.lower()) for p in patterns for part in (parts[0], parts[-1]))


def stream_tree(cfg, out):
    """<out>/<id>/data: the installed game the page streams instead of importing it, less [bundle] exclude,
    with the auxiliary modules' prepared copies beside the executable; manifest.txt lists its files.
    Files are hard links where the file system allows, else copies."""
    install = cfg["developer_exe_path"].parent
    dest = Path(out) / cfg["game"]["id"] / "data"
    shutil.rmtree(dest, ignore_errors=True)
    files = {}
    for src in sorted(install.rglob("*")):
        relative = src.relative_to(install).as_posix()
        if src.is_file() and not excluded(relative, cfg["bundle"]["exclude"]):
            files[relative] = src
    for m in cfg["aux_modules"]:
        files[m["name"]] = m["path"]
    for relative, src in files.items():
        target = dest / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        try:
            os.link(src, target)
        except OSError:
            shutil.copy2(src, target)
    (dest / "manifest.txt").write_text("".join(r + "\n" for r in files))
    return len(files)


class IsolatedHandler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {**http.server.SimpleHTTPRequestHandler.extensions_map, ".wasm": "application/wasm",
                      ".js": "text/javascript", ".data": "application/octet-stream"}

    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cross-Origin-Resource-Policy", "same-origin")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Accept-Ranges", "bytes")
        super().end_headers()

    # Byte ranges, which a streamed game is read in (one range: what the browser asks).
    def send_head(self):
        self.remaining = None
        path = self.translate_path(self.path)
        m = re.fullmatch(r"bytes=(\d*)-(\d*)", self.headers.get("Range", "").strip())
        if not m or not (m[1] or m[2]) or not os.path.isfile(path):
            return super().send_head()
        size = os.path.getsize(path)
        start, end = (int(m[1]), int(m[2]) if m[2] else size - 1) if m[1] else (max(0, size - int(m[2])), size - 1)
        end = min(end, size - 1)
        if start > end:
            self.send_error(416)
            return None
        f = open(path, "rb")
        f.seek(start)
        self.send_response(206)
        self.send_header("Content-Type", self.guess_type(path))
        self.send_header("Content-Range", "bytes %d-%d/%d" % (start, end, size))
        self.send_header("Content-Length", str(end - start + 1))
        self.end_headers()
        self.remaining = end - start + 1
        return f

    def copyfile(self, source, outputfile):
        if self.remaining is None:
            return super().copyfile(source, outputfile)
        while self.remaining > 0:
            buf = source.read(min(self.remaining, 1 << 20))
            if not buf:
                break
            outputfile.write(buf)
            self.remaining -= len(buf)


def serve(out, port):
    handler = functools.partial(IsolatedHandler, directory=str(out))
    with http.server.ThreadingHTTPServer(("127.0.0.1", port), handler) as httpd:
        print("serving %s at http://localhost:%d/" % (out, port), flush=True)
        httpd.serve_forever()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--game-dir", type=Path, action="append", required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--web-build", action="append", default=[], metavar="ID=DIR")
    parser.add_argument("--stream", action="append", default=[], metavar="ID",
                        help="serve this game's installed files beside its page (no import)")
    parser.add_argument("--serve", type=int, nargs="?", const=8000, metavar="PORT")
    args = parser.parse_args()
    games = build(args.game_dir, args.out)
    ids = {g["id"] for g in games}
    for spec in args.web_build:
        game_id, sep, build_dir = spec.partition("=")
        if not sep or game_id not in ids:
            parser.error("--web-build wants <game id>=<dir> for one of: %s" % ", ".join(sorted(ids)))
        copy_web_build(game_id, build_dir, args.out)
    cfgs = {c["game"]["id"]: c for c in map(game_config.load, args.game_dir)}
    for game_id in args.stream:
        if game_id not in ids:
            parser.error("--stream wants one of: %s" % ", ".join(sorted(ids)))
        print("streaming %d files of %s" % (stream_tree(cfgs[game_id], args.out), game_id))
    print("web launcher for %s in %s" % (", ".join(g["title"] for g in games), args.out))
    if args.serve is not None:
        serve(args.out, args.serve)


if __name__ == "__main__":
    main()
