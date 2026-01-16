from flask import Flask, jsonify
import time

app = Flask(__name__)

# Four intensity levels to cycle through
levels = [
    {"index": "low", "forecast": 80},
    {"index": "moderate", "forecast": 150},
    {"index": "high", "forecast": 260},
    {"index": "very high", "forecast": 380}
]

@app.route("/intensity")
def intensity():
    # Change every 2 minutes (120 s)
    idx = int(time.time() / 120) % len(levels)
    level = levels[idx]
    return jsonify({
        "data": [{
            "from": time.strftime("%Y-%m-%dT%H:%MZ", time.gmtime()),
            "to":   time.strftime("%Y-%m-%dT%H:%MZ", time.gmtime(time.time()+600)),
            "intensity": {
                "forecast": level["forecast"],
                "actual":   level["forecast"],
                "index":    level["index"]
            }
        }]
    })

if __name__ == "__main__":
    app.run(host="127.0.0.1", port=5000)
