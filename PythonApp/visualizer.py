#!/usr/bin/env python3
"""
EchoDriver Visualizer Server

Serves the HTML frontend and provides API endpoints for:
- Reading/writing reverb parameters via sysfs
- Uploading WAV files and processing them through the driver
- Serving the resulting WAV files for playback

Usage:
    python3 visualizer.py

Then open http://<vm-ip>:8080 in your browser.
"""

import http.server
import os
import sys
from urllib.parse import urlparse, parse_qs
import json
import subprocess
import tempfile
import shutil
import threading

PORT = 8080
DIRECTORY = os.path.dirname(os.path.abspath(__file__))
OUTPUT_DIR = os.path.join(DIRECTORY, 'output')
UPLOAD_DIR = os.path.join(DIRECTORY, 'uploads')
VENV_PYTHON = os.path.join(DIRECTORY, 'venv', 'bin', 'python3')

os.makedirs(OUTPUT_DIR, exist_ok=True)
os.makedirs(UPLOAD_DIR, exist_ok=True)

# sysfs paths for reverb parameters
SYSFS_BASE = '/sys/module/my_audio/parameters'
PARAM_NAMES = ['reverb_delay_ms', 'reverb_decay', 'reverb_wet']

# Processing state
processing_lock = threading.Lock()
processing_status = {'running': False, 'progress': '', 'error': None}


def read_sysfs_params():
    """Read current reverb parameters from sysfs."""
    params = {}
    for name in PARAM_NAMES:
        path = os.path.join(SYSFS_BASE, name)
        try:
            with open(path, 'r') as f:
                params[name] = int(f.read().strip())
        except (FileNotFoundError, PermissionError, ValueError) as e:
            params[name] = None
            params[f'{name}_error'] = str(e)
    return params


def write_sysfs_param(name, value):
    """Write a reverb parameter to sysfs."""
    if name not in PARAM_NAMES:
        return False, f'Unknown parameter: {name}'
    path = os.path.join(SYSFS_BASE, name)
    try:
        # Use sudo tee to write (needs passwordless sudo or root)
        proc = subprocess.run(
            ['sudo', 'tee', path],
            input=str(value).encode(),
            capture_output=True, timeout=5
        )
        if proc.returncode != 0:
            return False, proc.stderr.decode().strip()
        return True, 'OK'
    except Exception as e:
        return False, str(e)


def run_processing(input_wav_path=None):
    """Run test_echo.py in a subprocess to process audio through the driver."""
    global processing_status

    with processing_lock:
        if processing_status['running']:
            return

        processing_status = {'running': True, 'progress': 'Starting...', 'error': None}

    try:
        cmd = [VENV_PYTHON, os.path.join(DIRECTORY, 'test_echo.py')]

        # Calculate timeout dynamically from audio duration
        timeout_s = 60  # default for generated beeps
        if input_wav_path:
            cmd.extend(['--input', input_wav_path])
            try:
                import wave
                with wave.open(input_wav_path, 'r') as wf:
                    duration = wf.getnframes() / wf.getframerate()
                    timeout_s = int(duration * 2) + 30  # 2x audio length + buffer
                    processing_status['progress'] = f'Processing {duration:.0f}s of audio...'
            except Exception:
                timeout_s = 600  # fallback if we can't read the file

        proc = subprocess.run(
            cmd, capture_output=True, text=True,
            timeout=timeout_s,
            cwd=DIRECTORY
        )

        if proc.returncode != 0:
            processing_status['error'] = proc.stderr or proc.stdout
            processing_status['progress'] = 'Failed'
        else:
            processing_status['progress'] = 'Complete'
            processing_status['output'] = proc.stdout

    except subprocess.TimeoutExpired:
        processing_status['error'] = 'Processing timed out (>120s)'
        processing_status['progress'] = 'Timeout'
    except Exception as e:
        processing_status['error'] = str(e)
        processing_status['progress'] = 'Error'
    finally:
        processing_status['running'] = False


class VisualizerHandler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=DIRECTORY, **kwargs)

    def do_GET(self):
        # API: Get reverb parameters
        if self.path == '/api/params':
            params = read_sysfs_params()
            self._json_response(params)
            return

        # API: Get processing status
        if self.path == '/api/status':
            self._json_response(processing_status)
            return

        # API: Get audio metadata
        if self.path == '/api/metadata':
            meta_path = os.path.join(OUTPUT_DIR, 'metadata.json')
            if os.path.isfile(meta_path):
                with open(meta_path, 'r') as f:
                    self._json_response(json.load(f))
            else:
                self._json_response({'error': 'No metadata yet. Process audio first.'})
            return

        # Route /audio/* to the output/ directory
        if self.path.startswith('/audio/'):
            parsed = urlparse(self.path)
            filename = parsed.path.replace('/audio/', '')
            filepath = os.path.join(OUTPUT_DIR, filename)
            if os.path.isfile(filepath):
                self.send_response(200)
                self.send_header('Content-Type', 'audio/wav')
                self.send_header('Content-Length', str(os.path.getsize(filepath)))
                self.send_header('Access-Control-Allow-Origin', '*')
                self.end_headers()
                with open(filepath, 'rb') as f:
                    self.wfile.write(f.read())
                return
            else:
                self.send_error(404, f'Audio file not found: {filename}')
                return

        # Route /uploads/* to the uploads/ directory
        if self.path.startswith('/uploads/'):
            parsed = urlparse(self.path)
            filename = parsed.path.replace('/uploads/', '')
            filepath = os.path.join(UPLOAD_DIR, filename)
            if os.path.isfile(filepath):
                self.send_response(200)
                self.send_header('Content-Type', 'audio/wav')
                self.send_header('Content-Length', str(os.path.getsize(filepath)))
                self.send_header('Access-Control-Allow-Origin', '*')
                self.end_headers()
                with open(filepath, 'rb') as f:
                    self.wfile.write(f.read())
                return
            else:
                self.send_error(404, f'Upload file not found: {filename}')
                return

        # Route / to index.html
        if self.path == '/':
            self.path = '/index.html'

        return super().do_GET()

    def do_POST(self):
        # API: Set reverb parameters
        if self.path == '/api/params':
            content_length = int(self.headers.get('Content-Length', 0))
            body = self.rfile.read(content_length).decode('utf-8')
            try:
                data = json.loads(body)
            except json.JSONDecodeError:
                self._json_response({'error': 'Invalid JSON'}, 400)
                return

            results = {}
            for name, value in data.items():
                if name in PARAM_NAMES:
                    ok, msg = write_sysfs_param(name, int(value))
                    results[name] = {'ok': ok, 'message': msg}

            self._json_response(results)
            return

        # API: Upload WAV + process
        if self.path == '/api/upload':
            if processing_status['running']:
                self._json_response({'error': 'Processing already in progress'}, 409)
                return

            content_length = int(self.headers.get('Content-Length', 0))
            if content_length > 100 * 1024 * 1024:  # 100MB limit
                self._json_response({'error': 'File too large (max 100MB)'}, 413)
                return

            body = self.rfile.read(content_length)

            # Save uploaded file to uploads/ directory
            upload_path = os.path.join(UPLOAD_DIR, 'uploaded_input.wav')
            with open(upload_path, 'wb') as f:
                f.write(body)

            # Start processing in background
            t = threading.Thread(target=run_processing, args=(upload_path,))
            t.daemon = True
            t.start()

            self._json_response({'status': 'processing_started'})
            return

        # API: Process without upload (use beeps)
        if self.path == '/api/process':
            if processing_status['running']:
                self._json_response({'error': 'Processing already in progress'}, 409)
                return

            t = threading.Thread(target=run_processing)
            t.daemon = True
            t.start()

            self._json_response({'status': 'processing_started'})
            return

        self.send_error(404)

    def _json_response(self, data, status=200):
        body = json.dumps(data).encode('utf-8')
        self.send_response(status)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Access-Control-Allow-Origin', '*')
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, format, *args):
        print(f"  [{self.log_date_time_string()}] {format % args}")


def main():
    # Check prerequisites
    for fname in ['original_input.wav', 'reverb_output.wav']:
        fpath = os.path.join(OUTPUT_DIR, fname)
        if not os.path.isfile(fpath):
            print(f"[WARN] output/{fname} not found. Run test_echo.py first, or use the web UI to generate it.")

    if not os.path.isdir(SYSFS_BASE):
        print(f"[WARN] Sysfs not found at {SYSFS_BASE}. Is my_audio.ko loaded?")

    handler = VisualizerHandler
    with http.server.HTTPServer(('0.0.0.0', PORT), handler) as server:
        print(f"╔══════════════════════════════════════════════╗")
        print(f"║   EchoDriver Studio                         ║")
        print(f"║   http://0.0.0.0:{PORT}                       ║")
        print(f"║                                              ║")
        print(f"║   API Endpoints:                             ║")
        print(f"║     GET  /api/params  - Read reverb params   ║")
        print(f"║     POST /api/params  - Set reverb params    ║")
        print(f"║     POST /api/upload  - Upload & process WAV ║")
        print(f"║     POST /api/process - Process test beeps   ║")
        print(f"║     GET  /api/status  - Processing status    ║")
        print(f"║                                              ║")
        print(f"║   Press Ctrl+C to stop                       ║")
        print(f"╚══════════════════════════════════════════════╝")
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            print("\nServer stopped.")


if __name__ == '__main__':
    main()
