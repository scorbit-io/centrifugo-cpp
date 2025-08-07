import os
import jwt
import time
from flask import Flask, request

app = Flask(__name__)
SECRET = os.getenv("JWT_SECRET", "my-secret-key")


@app.route("/")
def index():
    return "<h2>JWT Token Generator</h2><p>Usage: /token/username?days=7</p><p>Includes server-side subscriptions for channels: testchan, otherchan</p>"


@app.route("/token/<user>")
def get_token(user):
    days = int(request.args.get("days", 7))
    exp_time = int(time.time()) + (60 * 60 * 24 * days)

    channels = ["testchan", "otherchan"]
    token = jwt.encode(
        {"sub": user, "exp": exp_time, "channels": channels}, SECRET, algorithm="HS256"
    )
    return token


if __name__ == "__main__":
    print("JWT Generator running on http://0.0.0.0:3001")
    print("Usage: http://localhost:3001/token/test-user?days=7")
    app.run(host="0.0.0.0", port=3001)
