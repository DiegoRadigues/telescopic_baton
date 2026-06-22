import serial
import time
import pandas as pd
import numpy as np
import matplotlib.pyplot as plt
import os

# === cfg ===
PORT = 'COM8'          # update port
BAUD_RATE = 115200
NUM_FLICKS = 20        # total rec mvts
RECORD_TIME = 1.5      # rec dur per mvt (s)
EXPORT_DIR = "flicks_data"

# init export dir
if not os.path.exists(EXPORT_DIR):
    os.makedirs(EXPORT_DIR)

# === conn ===
print(f"Connecting to {PORT} at {BAUD_RATE} baud...")
try:
    ser = serial.Serial(PORT, BAUD_RATE, timeout=1)
    time.sleep(2) # wait arduino rst
except Exception as e:
    print(f"Serial error: {e}")
    exit()

all_flicks = []

print("\n" + "="*50)
print(" START OF CHARACTERIZATION CAMPAIGN")
print(f" Target: {NUM_FLICKS} Flicks")
print("="*50)

# --- 1. cap phase ---
for i in range(1, NUM_FLICKS + 1):
    input(f"\n[Flick {i}/{NUM_FLICKS}] Hold the baton. Press ENTER, then perform your Flick motion...")
    
    # clr buf to skip old data
    ser.reset_input_buffer() 
    
    print("Recording (1.5s)... GO!")
    start_time = time.time()
    data = []
    
    # fast rec loop
    while (time.time() - start_time) < RECORD_TIME:
        if ser.in_waiting:
            try:
                line = ser.readline().decode('utf-8').strip()
                parts = line.split(',')
                if len(parts) == 4:
                    data.append([int(parts[0]), float(parts[1]), float(parts[2]), float(parts[3])])
            except:
                pass # skip rd err

    # wr to pandas df
    df = pd.DataFrame(data, columns=['Timestamp', 'AccelX', 'AccelY', 'AccelZ'])
    if not df.empty:
        # rel time ms
        df['Time_ms'] = df['Timestamp'] - df['Timestamp'].iloc[0]
        
        # save df file
        df.to_csv(f"{EXPORT_DIR}/flick_{i}.csv", index=False)
        all_flicks.append(df)
        print(f"-> Done! ({len(df)} measurements captured)")
    else:
        print("-> ERROR: No data received.")

ser.close()

# --- 2. analysis & sync ---
print("\n" + "="*50)
print(" ANALYZING DATA...")
print("="*50)

stats = []
plt.figure(figsize=(14, 10))

for i, df in enumerate(all_flicks):
    # a. find x axis peak (max abs val)
    peak_idx = df['AccelX'].abs().idxmax()
    peak_time_ms = df.loc[peak_idx, 'Time_ms']
    peak_accel_x = df.loc[peak_idx, 'AccelX']
    
    # b. calc jerk (accel deriv)
    # dt in s
    df['dt_s'] = df['Time_ms'].diff() / 1000.0 
    # jerk = dA / dt
    df['JerkX'] = df['AccelX'].diff() / df['dt_s']
    
    max_jerk_x = df['JerkX'].abs().max()
    
    # c. time sync (peak at t=0)
    df['Aligned_Time_ms'] = df['Time_ms'] - peak_time_ms
    
    # d. save stats
    stats.append({
        'Flick': i + 1,
        'Max_Accel_X (g)': abs(peak_accel_x),
        'Max_Jerk_X (g/s)': max_jerk_x
    })
    
    # e. plt graphs
    # graph 1: x axis (main mvt)
    plt.subplot(3, 1, 1)
    plt.plot(df['Aligned_Time_ms'], df['AccelX'], label=f"Flick {i+1}")
    
    # graph 2: y/z axis (parasitic mvt)
    plt.subplot(3, 1, 2)
    plt.plot(df['Aligned_Time_ms'], df['AccelY'], color='green', alpha=0.3)
    plt.plot(df['Aligned_Time_ms'], df['AccelZ'], color='purple', alpha=0.3)
    
    # graph 3: jerk x
    plt.subplot(3, 1, 3)
    plt.plot(df['Aligned_Time_ms'], df['JerkX'].abs(), color='red', alpha=0.5)

# --- 3. graph format/param ---
plt.subplot(3, 1, 1)
plt.title("Longitudinal Acceleration (X Axis) - Synchronized on impact (T=0)")
plt.ylabel("Acceleration (g)")
plt.grid(True)

# adjust leg for multi items (prevent overflow)
plt.legend(bbox_to_anchor=(1.01, 1), loc='upper left', fontsize='x-small', ncol=2)

plt.subplot(3, 1, 2)
plt.title("Superposition of Parasitic Axes (Y and Z)")
plt.ylabel("Acceleration (g)")
plt.grid(True)

plt.subplot(3, 1, 3)
plt.title("Jerk on X Axis (Strike violence in g/s)")
plt.xlabel("Time (ms) [T=0 is the impact peak]")
plt.ylabel("Jerk |g/s|")
plt.grid(True)

plt.tight_layout()
plot_filename = f"{EXPORT_DIR}/analyse_flicks.png"
plt.savefig(plot_filename)
print(f"\nGraph generated and saved: {plot_filename}")

# --- 4. stat summary ---
stats_df = pd.DataFrame(stats)
print("\n--- STATISTICAL SUMMARY OF FLICKS ---")
print(stats_df.to_string(index=False))

mean_accel = stats_df['Max_Accel_X (g)'].mean()
min_accel = stats_df['Max_Accel_X (g)'].min()
mean_jerk = stats_df['Max_Jerk_X (g/s)'].mean()
min_jerk = stats_df['Max_Jerk_X (g/s)'].min()

print("\n--- THRESHOLD SUGGESTIONS FOR C++ CODE (VR) ---")
print("-> To ensure detection even for a 'weak' Flick:")
print(f"   X Acceleration Threshold: > {min_accel * 0.8:.1f} g")
print(f"   X Jerk Threshold        : > {min_jerk * 0.8:.0f} g/s")
print("(These suggestions include a 20% error margin below your weakest Flick).")

plt.show()