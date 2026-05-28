import serial
import wave
import os
import time
from datetime import datetime

PORT = "COM10"
BAUD = 921600
SAMPLE_RATE = 8000
RECORD_SECONDS = 5
TOTAL_BYTES = SAMPLE_RATE * RECORD_SECONDS * 2

os.makedirs("recordings", exist_ok=True)

timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
filename = os.path.join("recordings", f"{timestamp}_mic_recording.wav")

ser = serial.Serial(PORT, BAUD, timeout=5)
time.sleep(2)

ser.reset_input_buffer()

print("Waiting for ESP32-S3...")

ready_found = False
start_time = time.time()

while time.time() - start_time < 10:
    line = ser.readline().decode(errors="ignore").strip()
    if line:
        print("ESP:", line)
    if line == "READY":
        ready_found = True
        break

if not ready_found:
    print("READY message not found, but recording command will be sent.")

print("Start recording...")
ser.write(b"R")

start_found = False
start_time = time.time()

while time.time() - start_time < 10:
    line = ser.readline().decode(errors="ignore").strip()
    if line:
        print("ESP:", line)
    if line == "START":
        start_found = True
        break

if not start_found:
    ser.close()
    raise RuntimeError("START message not received from ESP32-S3.")

audio_data = bytearray()

while len(audio_data) < TOTAL_BYTES:
    chunk = ser.read(TOTAL_BYTES - len(audio_data))
    if not chunk:
        break
    audio_data.extend(chunk)
    print(f"Received {len(audio_data)} / {TOTAL_BYTES} bytes", end="\r")

print()
print("Recording received.")

if len(audio_data) != TOTAL_BYTES:
    print(f"Warning: expected {TOTAL_BYTES} bytes, received {len(audio_data)} bytes.")

with wave.open(filename, "wb") as wf:
    wf.setnchannels(1)
    wf.setsampwidth(2)
    wf.setframerate(SAMPLE_RATE)
    wf.writeframes(audio_data)

print(f"Saved: {filename}")

ser.close()
