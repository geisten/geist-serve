"""geistd.py — client for geistd, stdlib only.

    import geistd
    c = geistd.Client()                       # Unix socket, default path
    c = geistd.Client(host="pi", port=7433, token="...")
    s = c.open(temperature=0)                 # or c.resume("1a2b...")
    ids = c.tokenize("Human: Capital of France?\\n\\nAssistant:")
    print(s.prefill(ids))                     # {'prefilled': n, 'reused': 0, 'n': n}
    print(s.peek(ids=c.tokenize(" Paris") + c.tokenize(" London")))
    for piece in s.generate(max=16): print(piece, end="")

Frames: u32 header_len, u32 body_len (little-endian), JSON header, raw body.
Token ids are int32 arrays, logits float32 arrays.

geistd serves one connection at a time, so this client connects per call
and closes right after the reply (a Unix socket connect is microseconds);
only `generate` holds the connection for the stream. Never keep an idle
connection open to geistd: everyone else waits behind it.
"""
import array, json, os, socket, struct, sys

class GeistdError(RuntimeError):
    pass

def default_socket_path():
    rt = os.environ.get("XDG_RUNTIME_DIR")
    return f"{rt}/geistd.sock" if rt else f"/tmp/geistd-{os.getuid()}.sock"

class Client:
    def __init__(self, path=None, host=None, port=None, token=None, timeout=600):
        self.path, self.host, self.port, self.token, self.timeout = path, host, port, token, timeout
        self.sock = None
        self.info_cache = None

    # -- connection -----------------------------------------------------------
    def _connect(self):
        if self.sock is not None:
            return
        if self.host:
            self.sock = socket.create_connection((self.host, self.port), timeout=self.timeout)
        else:
            self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            self.sock.settimeout(self.timeout)
            self.sock.connect(self.path or default_socket_path())
        if self.token is not None:  # handshake on this very connection
            self._send({"op": "hello", "token": self.token})
            h, _ = self._recv()
            if not h.get("ok", False):
                self.close()
                raise GeistdError(h.get("error", "hello refused"))

    def close(self):
        if self.sock is not None:
            self.sock.close()
            self.sock = None

    def _read(self, n):
        buf = b""
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                self.close()
                raise GeistdError("connection closed by geistd")
            buf += chunk
        return buf

    def _send(self, header, body=b""):
        h = json.dumps(header).encode()
        self.sock.sendall(struct.pack("<II", len(h), len(body)) + h + body)

    def _recv(self):
        hl, bl = struct.unpack("<II", self._read(8))
        header = json.loads(self._read(hl))
        body = self._read(bl) if bl else b""
        return header, body

    def _call(self, header, body=b""):
        self._connect()
        try:
            self._send(header, body)
            h, b = self._recv()
        finally:
            self.close()
        if not h.get("ok", False):
            raise GeistdError(h.get("error", "unknown error"))
        return h, b

    # -- ops -------------------------------------------------------------------
    def info(self):
        h, _ = self._call({"op": "info"})
        self.info_cache = h
        return h

    def tokenize(self, text):
        _, b = self._call({"op": "tokenize", "text": text})
        return list(array.array("i", b))

    def open(self, temperature=0.0, top_p=1.0, top_k=0, seed=0):
        h, _ = self._call({"op": "open", "temperature": temperature, "top_p": top_p, "top_k": top_k, "seed": seed})
        return Session(self, h["session"])

    def resume(self, session_id):
        return Session(self, session_id)

class Session:
    def __init__(self, client, sid):
        self.client, self.id = client, sid

    def _call(self, op, body=b"", **fields):
        return self.client._call({"op": op, "session": self.id, **fields}, body)

    def prefill(self, ids):
        h, _ = self._call("prefill", array.array("i", ids).tobytes())
        return {"prefilled": h["prefilled"], "reused": h["reused"], "n": h["n"]}

    def step(self, topk=0):
        h, _ = self._call("step", topk=topk)
        return h

    def peek(self, ids=None, topk=0, full=False):
        h, b = self._call("peek", array.array("i", ids or []).tobytes(), topk=topk, full=full)
        if full:
            h["full"] = list(array.array("f", b))
        return h

    def strs(self, ids):
        h, _ = self._call("str", array.array("i", ids).tobytes())
        return h["pieces"]

    def generate(self, max=256, stop_ids=None, stop_strings=None):
        """Yields pieces; the final frame is available as .last after the loop."""
        self.client._connect()
        self.client._send({"op": "generate", "session": self.id, "max": max,
                           "stop_ids": stop_ids or [], "stop_strings": stop_strings or []})
        try:
            while True:
                h, _ = self.client._recv()
                if not h.get("ok", False):
                    raise GeistdError(h.get("error", "unknown error"))
                if h.get("done"):
                    self.last = h
                    return
                yield h["piece"] or ""
        finally:
            self.client.close()

    def reset(self):
        self._call("reset")

    def close(self):
        self._call("close")

if __name__ == "__main__":
    c = Client()
    print(json.dumps(c.info(), indent=1))
