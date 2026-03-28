#!/usr/bin/env python3
"""
NSX Simulator Local Server
Usage: python3 server.py
Then open: http://localhost:8080
"""
import http.server
import json
import os
import subprocess
import sys

SIM_DIR = os.path.dirname(os.path.abspath(__file__))
PORT = 8080

OUTPUT_FILES = [
    "sim_output.txt",
    "sim_trace_custom.txt",
    "sim_trace_a.txt",
    "sim_trace_b.txt",
    "sim_trace_c.txt",
]


class SimHandler(http.server.BaseHTTPRequestHandler):

    def _cors(self):
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")

    def do_OPTIONS(self):
        self.send_response(200)
        self._cors()
        self.end_headers()

    def do_GET(self):
        path = self.path.split("?")[0]
        if path in ("/", "/visualizer.html"):
            self._serve_file("visualizer.html", "text/html; charset=utf-8")
        elif path == "/health":
            self.send_response(200)
            self._cors()
            self.send_header("Content-Type", "text/plain")
            self.end_headers()
            self.wfile.write(b"ok")
        else:
            self.send_error(404)

    def do_POST(self):
        if self.path != "/run":
            self.send_error(404)
            return

        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length)
        try:
            config = json.loads(body)
        except json.JSONDecodeError as e:
            self._json_error(400, f"Invalid JSON: {e}")
            return

        # Write sim_config.json for the C++ binary to read
        config_path = os.path.join(SIM_DIR, "sim_config.json")
        with open(config_path, "w") as f:
            json.dump(config, f, indent=2)

        # Remove stale outputs so the response only contains files from this run.
        for fname in OUTPUT_FILES:
            fpath = os.path.join(SIM_DIR, fname)
            try:
                os.remove(fpath)
            except FileNotFoundError:
                pass

        # Run the simulator
        sim_bin = os.path.join(SIM_DIR, "nsx_sim")
        if not os.path.exists(sim_bin):
            self._json_error(500, "nsx_sim binary not found. Run: clang++ -std=c++20 -O2 -o nsx_sim main.cpp")
            return

        try:
            result = subprocess.run(
                [sim_bin],
                cwd=SIM_DIR,
                capture_output=True,
                text=True,
                timeout=300,
            )
        except subprocess.TimeoutExpired:
            self._json_error(500, "Simulation timed out (>5 min)")
            return
        except Exception as e:
            self._json_error(500, f"Failed to run simulator: {e}")
            return

        # Collect output files
        files = {}
        for fname in OUTPUT_FILES:
            fpath = os.path.join(SIM_DIR, fname)
            if os.path.exists(fpath):
                with open(fpath, "r", errors="replace") as f:
                    files[fname] = f.read()

        response = {
            "returncode": result.returncode,
            "stdout": result.stdout,
            "stderr": result.stderr,
            "files": files,
        }

        body_out = json.dumps(response).encode("utf-8")
        self.send_response(200)
        self._cors()
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body_out)))
        self.end_headers()
        self.wfile.write(body_out)

    def _serve_file(self, filename, content_type):
        path = os.path.join(SIM_DIR, filename)
        try:
            with open(path, "rb") as f:
                data = f.read()
            self.send_response(200)
            self._cors()
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
        except FileNotFoundError:
            self.send_error(404, f"{filename} not found")

    def _json_error(self, code, msg):
        body = json.dumps({"error": msg}).encode("utf-8")
        self.send_response(code)
        self._cors()
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        print(f"[SimServer] {fmt % args}", flush=True)


if __name__ == "__main__":
    os.chdir(SIM_DIR)
    try:
        srv = http.server.HTTPServer(("localhost", PORT), SimHandler)
    except OSError as e:
        print(f"Cannot bind to port {PORT}: {e}", file=sys.stderr)
        sys.exit(1)
    print(f"NSX Simulator Server  →  http://localhost:{PORT}", flush=True)
    print("Press Ctrl+C to stop.", flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\nServer stopped.")
