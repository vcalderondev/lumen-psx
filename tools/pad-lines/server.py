"""Local helper for the turntable renders: serves this folder and the models, and saves the
frames the page posts (POST /frame/<name>/<n>, body = PNG). Exits on POST /done."""
import http.server, os, sys, threading
ROOT = os.path.dirname(os.path.abspath(__file__)); MODELS = sys.argv[1]; OUT = sys.argv[2]
class H(http.server.SimpleHTTPRequestHandler):
    def translate_path(self, path):
        p = path.split("?")[0]
        if p.startswith("/models/"): return os.path.join(MODELS, os.path.normpath(p[8:]).lstrip("/"))
        return os.path.join(ROOT, os.path.normpath(p).lstrip("/"))
    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0)); body = self.rfile.read(n)
        if self.path.startswith("/frame/"):
            _, _, name, idx = self.path.split("/")
            if name.replace("_", "").isalnum() and idx.isdigit():
                os.makedirs(os.path.join(OUT, name), exist_ok=True)
                open(os.path.join(OUT, name, "%03d.png" % int(idx)), "wb").write(body)
        self.send_response(200); self.end_headers()
        if self.path == "/done": threading.Thread(target=self.server.shutdown).start()
    def log_message(self, *a): pass
http.server.ThreadingHTTPServer(("127.0.0.1", 8791), H).serve_forever()
