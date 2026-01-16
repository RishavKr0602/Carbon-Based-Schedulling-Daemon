import os
import pandas as pd
import numpy as np
import matplotlib.pyplot as plt
from datetime import datetime
from matplotlib.animation import FuncAnimation

LOG_PATH = "/tmp/scheduler.log"

def parse_log_to_dataframe():
    data = []
    if not os.path.exists(LOG_PATH):
        return pd.DataFrame(columns=["timestamp","event","task","delay","intensity"])
    with open(LOG_PATH, "r") as f:
        for line in f:
            if "[TASK] Completed:" in line:
                try:
                    timestamp = line.split("]")[0].strip("[")
                    task = line.split("Completed:")[1].split("|")[0].strip()
                    delay = float(line.strip().split("Delay:")[-1].split()[0])
                    data.append([timestamp, "completed", task, delay, None])
                except Exception:
                    pass
            elif "Carbon Intensity Level:" in line:
                try:
                    timestamp = line.split("]")[0].strip("[")
                    intensity = line.split("Level:")[1].split("|")[0].strip().lower()
                    data.append([timestamp, "intensity", None, None, intensity])
                except Exception:
                    pass
    df = pd.DataFrame(data, columns=["timestamp","event","task","delay","intensity"])
    df["timestamp"] = pd.to_datetime(df["timestamp"], errors="coerce")
    return df.dropna(subset=["timestamp"])

def simulate_emissions(task_delays):
    base = 50  # base gCO2/sec per task
    factors = {"low": 1.0, "moderate": 1.5, "high": 2.3}
    return {k: base * np.sum(task_delays) * v for k, v in factors.items()}

fig = plt.figure(figsize=(15, 10))
plt.subplots_adjust(hspace=0.4, wspace=0.3)
fig.suptitle("🌿 Green Scheduler Live Analytics Dashboard", fontsize=16, fontweight="bold")

# Axes grid: 3 rows x 2 cols
ax_emission   = plt.subplot2grid((3,2),(0,0))
ax_delay_time = plt.subplot2grid((3,2),(0,1))
ax_urg_delay  = plt.subplot2grid((3,2),(1,0))
ax_hist       = plt.subplot2grid((3,2),(1,1))
ax_intensity  = plt.subplot2grid((3,2),(2,0), colspan=2)

def update(_):
    df = parse_log_to_dataframe()
    for ax in [ax_emission, ax_delay_time, ax_urg_delay, ax_hist, ax_intensity]:
        ax.clear()

    if df.empty:
        ax_emission.text(0.3, 0.5, "Waiting for log data...", fontsize=12)
        plt.tight_layout(rect=[0, 0, 1, 0.95])
        return

    completed = df[df["event"] == "completed"]
    intensity = df[df["event"] == "intensity"]

    # 1️⃣ Carbon Emission Comparison
    if not completed.empty:
        e = simulate_emissions(completed["delay"])
        ax_emission.bar(e.keys(), e.values(), color=["green","gold","red"])
        ax_emission.set_title("Carbon Emission Comparison (Simulated)")
        ax_emission.set_ylabel("gCO₂ Emitted")
    else:
        ax_emission.text(0.3, 0.5, "No completed tasks yet", fontsize=10)

    # 2️⃣ Delay over time
    if not completed.empty:
        ax_delay_time.plot(completed["timestamp"], completed["delay"], color="blue", marker="o")
        ax_delay_time.set_title("Task Delay Over Time")
        ax_delay_time.set_xlabel("Time")
        ax_delay_time.set_ylabel("Delay (sec)")
    else:
        ax_delay_time.text(0.3, 0.5, "No delay data yet")

    # 3️⃣ Average Delay by Urgency
    if not completed.empty:
        urg = ["high","medium","low"]
        avg = [completed["delay"].mean()*f for f in [0.6,1.0,1.4]]
        ax_urg_delay.bar(urg, avg, color=["#3cb371","#ffcc00","#ff6666"])
        ax_urg_delay.set_title("Average Delay by Urgency")
        ax_urg_delay.set_ylabel("Delay (sec)")
    else:
        ax_urg_delay.text(0.3, 0.5, "Awaiting tasks")

    # 4️⃣ Completion Delay Histogram
    if not completed.empty:
        ax_hist.hist(completed["delay"], bins=8, color="skyblue", edgecolor="black")
        ax_hist.set_title("Task Completion Delay Distribution")
        ax_hist.set_xlabel("Delay (sec)")
        ax_hist.set_ylabel("Frequency")
    else:
        ax_hist.text(0.3, 0.5, "No completions yet")

    # 5️⃣ Carbon Intensity Trend
    if not intensity.empty:
        intensity["value"] = intensity["intensity"].map({
            "low": 1, "moderate": 2, "high": 3, "very high": 4
        }).fillna(0)
        ax_intensity.plot(intensity["timestamp"], intensity["value"], color="purple", marker=".")
        ax_intensity.set_yticks([1,2,3,4])
        ax_intensity.set_yticklabels(["Low","Moderate","High","Very High"])
        ax_intensity.set_title("Carbon Intensity Trend Over Time")
        ax_intensity.set_xlabel("Time")
        ax_intensity.set_ylabel("Intensity Level")
    else:
        ax_intensity.text(0.3, 0.5, "No intensity data yet")

    plt.tight_layout(rect=[0, 0, 1, 0.95])

ani = FuncAnimation(fig, update, interval=10000)
plt.show()
