import serial
import time
import sys

port = '/dev/ttyUSB0'
baud = 115200

try:
    s = serial.Serial(port, baud, timeout=1)
    # Reset ESP32
    s.setDTR(False)
    s.setRTS(False)
    time.sleep(0.5)
    s.setDTR(True)
    s.setRTS(True)
    
    end = time.time() + 40
    while time.time() < end:
        line = s.readline()
        if line:
            print(line.decode('utf-8', 'ignore').strip())
            sys.stdout.flush()
except Exception as e:
    print(f"Error: {e}")
finally:
    if 's' in locals():
        s.close()
