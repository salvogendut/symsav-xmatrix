#!/usr/bin/env python3
import http.server
import os

PORT = 9092
DIR  = os.path.dirname(os.path.abspath(__file__))

class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=DIR, **kwargs)

if __name__ == "__main__":
    with http.server.HTTPServer(("", PORT), Handler) as httpd:
        print(f"Serving {DIR} on http://0.0.0.0:{PORT}")
        httpd.serve_forever()
