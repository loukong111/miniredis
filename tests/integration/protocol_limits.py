#!/usr/bin/env python3
import json
import signal
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request
from pathlib import Path


def resp_frame(*parts):
    chunks = [f"*{len(parts)}\r\n".encode()]
    for part in parts:
        raw = part.encode()
        chunks.extend((f"${len(raw)}\r\n".encode(), raw, b"\r\n"))
    return b"".join(chunks)


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def connect(port, io_timeout=2):
    sock = socket.create_connection(("127.0.0.1", port), timeout=2)
    sock.settimeout(io_timeout)
    return sock


def recv_exact(sock, size):
    data = b""
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise AssertionError(f"connection closed after {len(data)} of {size} bytes")
        data += chunk
    return data


def recv_until_close(sock):
    chunks = []
    while True:
        chunk = sock.recv(65536)
        if not chunk:
            return b"".join(chunks)
        chunks.append(chunk)


def authenticate(sock, auth):
    sock.sendall(auth)
    assert recv_exact(sock, 5) == b"+OK\r\n"


def wait_ready(port, process):
    ping = resp_frame("PING")
    for _ in range(100):
        if process.poll() is not None:
            raise AssertionError(f"server exited early with code {process.returncode}")
        try:
            with connect(port) as sock:
                sock.sendall(ping)
                if b"NOAUTH" in sock.recv(256):
                    return
        except OSError:
            pass
        time.sleep(0.05)
    raise AssertionError("server did not become ready")


def fetch_stats(stats_port):
    with urllib.request.urlopen(f"http://127.0.0.1:{stats_port}/stats", timeout=2) as response:
        return json.load(response)


def wait_connected_clients(stats_port, expected):
    stats = None
    for _ in range(100):
        stats = fetch_stats(stats_port)
        if stats["connected_clients"] == expected:
            return stats
        time.sleep(0.02)
    raise AssertionError(f"connected_clients did not become {expected}: {stats}")


def http_exchange(stats_port, first, rest=b"", io_timeout=2):
    with connect(stats_port, io_timeout) as sock:
        sock.sendall(first)
        if rest:
            time.sleep(0.02)
            sock.sendall(rest)
        return recv_until_close(sock)


def run_protocol_tests(port, stats_port):
    auth = resp_frame("AUTH", "protocol-secret")

    with connect(port) as sock:
        sock.sendall(auth[:9])
        sock.settimeout(0.1)
        try:
            premature = sock.recv(1)
        except socket.timeout:
            premature = b""
        assert premature == b""
        sock.settimeout(2)
        sock.sendall(auth[9:])
        assert recv_exact(sock, 5) == b"+OK\r\n"

        sticky = (resp_frame("PING", "one") + resp_frame("PING", "two") +
                  resp_frame("SET", "net:key", "v"))
        expected = b"$3\r\none\r\n$3\r\ntwo\r\n+OK\r\n"
        sock.sendall(sticky)
        assert recv_exact(sock, len(expected)) == expected

        fragmented = resp_frame("GET", "net:key")
        for byte in fragmented:
            sock.sendall(bytes((byte,)))
        assert recv_exact(sock, 7) == b"$1\r\nv\r\n"

    with connect(port) as sock:
        authenticate(sock, auth)
        sock.sendall(b"?invalid\r\n")
        assert recv_until_close(sock) == b"-ERR Protocol error: unknown RESP type byte\r\n"

    with connect(port) as sock:
        authenticate(sock, auth)
        sock.sendall(resp_frame("PING") * 65)
        assert recv_until_close(sock) == b"-ERR pipeline command limit exceeded\r\n"

    with connect(port) as sock:
        authenticate(sock, auth)
        sock.sendall(b"*2\r\n$3\r\nGET\r\n$9000\r\n" + b"x" * 8300)
        assert recv_until_close(sock) == b"-ERR request too large\r\n"

    with connect(port) as sock:
        authenticate(sock, auth)
        sock.sendall(resp_frame("SET", "net:big", "v" * 4096))
        assert recv_exact(sock, 5) == b"+OK\r\n"
        sock.sendall(resp_frame("GET", "net:big") * 5)
        assert recv_until_close(sock) == b"-ERR client output buffer limit exceeded\r\n"

    request = b"GET /healthz HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"
    response = http_exchange(stats_port, request[:13], request[13:])
    assert b"HTTP/1.1 200 OK" in response and b'"status":"ok"' in response

    response = http_exchange(
        stats_port,
        b"POST /stats HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
    assert b"HTTP/1.1 405 Method Not Allowed" in response
    assert b"Allow: GET\r\n" in response

    response = http_exchange(
        stats_port, b"GET /healthz HTTP/1.1\r\nHost: localhost", io_timeout=4)
    assert b"HTTP/1.1 400 Bad Request" in response

    header_prefix = b"GET /healthz HTTP/1.1\r\nX-Test: "
    oversized_header = header_prefix + b"x" * (8192 - len(header_prefix))
    response = http_exchange(stats_port, oversized_header)
    assert b"HTTP/1.1 431 Request Header Fields Too Large" in response

    response = http_exchange(
        stats_port,
        b"GET /missing HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
    assert b"HTTP/1.1 404 Not Found" in response

    wait_connected_clients(stats_port, 0)
    first = connect(port)
    second = connect(port)
    try:
        wait_connected_clients(stats_port, 2)
        with connect(port) as third:
            assert recv_exact(third, 36) == b"-ERR max number of clients reached\r\n"
            assert third.recv(1) == b""
        stats = fetch_stats(stats_port)
        assert stats["rejected_connections"] >= 1
    finally:
        first.close()
        second.close()
    wait_connected_clients(stats_port, 0)


def main():
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} /path/to/miniredis", file=sys.stderr)
        return 2

    server = str(Path(sys.argv[1]).resolve())
    port = free_port()
    stats_port = free_port()
    while stats_port == port:
        stats_port = free_port()

    with tempfile.TemporaryDirectory(prefix="miniredis_protocol_") as workdir:
        log_path = Path(workdir) / "server.log"
        snapshot = Path(workdir) / "snapshot.dat"
        with log_path.open("w") as log:
            process = subprocess.Popen(
                [server,
                 "--bind", "127.0.0.1", "--port", str(port),
                 "--stats-bind", "127.0.0.1", "--stats-port", str(stats_port),
                 "--snapshot-file", str(snapshot), "--snapshot-interval", "300",
                 "--requirepass", "protocol-secret",
                 "--max-clients", "2",
                 "--max-request-bytes", "8192",
                 "--max-value-bytes", "4096",
                 "--max-pipeline-commands", "64",
                 "--client-output-buffer-limit", "16384",
                 "--log-level", "warn"],
                stdout=log, stderr=subprocess.STDOUT)
            try:
                wait_ready(port, process)
                run_protocol_tests(port, stats_port)
            except Exception:
                log.flush()
                print(log_path.read_text(), file=sys.stderr)
                raise
            finally:
                if process.poll() is None:
                    process.send_signal(signal.SIGINT)
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=5)

    print("protocol and resource boundary tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
