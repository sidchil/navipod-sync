#!/usr/bin/env python3
"""Minimal fake Navidrome (Subsonic API) server for the smoke test."""
import http.server, json, sys, urllib.parse

SONGS = [
    {"id": str(i), "title": f"Song {i}", "artist": "Artist A" if i < 3 else "Artist B",
     "album": f"Album {i % 2}", "suffix": "mp3" if i != 2 else "flac", "duration": 5,
     "bitRate": 128 if i != 2 else 900, "size": 20000, "track": i, "year": 2001,
     "genre": "Rock", "coverArt": f"al{i % 2}"}
    for i in range(1, 5)
]
AUDIO = (b"\xff\xfb\x90\x00" + b"\0" * 413) * 40


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def send_json(self, body):
        data = json.dumps({"subsonic-response": {"status": "ok", **body}}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        url = urllib.parse.urlparse(self.path)
        query = dict(urllib.parse.parse_qsl(url.query))
        endpoint = url.path.split("/")[-1]
        if endpoint == "search3":
            self.send_json({"searchResult3": {"song": SONGS if query.get("songOffset") == "0" else []}})
        elif endpoint in ("download", "stream"):
            self.send_response(200)
            self.send_header("Content-Type", "audio/mpeg")
            self.end_headers()
            self.wfile.write(AUDIO)
        elif endpoint == "getCoverArt":
            self.send_response(404)
            self.end_headers()
        elif endpoint == "getPlaylists":
            self.send_json({"playlists": {"playlist": [{"id": "p1", "name": "Mix"}]}})
        elif endpoint == "getPlaylist":
            self.send_json({"playlist": {"entry": [SONGS[3], SONGS[0]]}})
        else:
            self.send_json({})


if __name__ == "__main__":
    http.server.HTTPServer(("127.0.0.1", int(sys.argv[1])), Handler).serve_forever()
