# Running the Project in Wokwi (no physical hardware needed)

[Wokwi](https://wokwi.com) lets you simulate the whole ESP32 + sensor rig in the browser.
This is useful for your project demo/viva if hardware isn't assembled yet, or to debug
firmware logic in isolation.

## Option A — Wokwi web IDE (fastest)
1. Go to https://wokwi.com/projects/new/esp32 and create a new ESP32 project.
2. Replace the default `sketch.ino` with the contents of
   `/firmware/cold_chain_esp32/cold_chain_esp32.ino`.
3. Open `diagram.json` (Wokwi's "diagram" tab) and paste in this folder's `diagram.json`.
4. In the sketch, temporarily set:
   ```cpp
   const char* WIFI_SSID     = "Wokwi-GUEST";
   const char* WIFI_PASSWORD = "";
   ```
   Wokwi's virtual network (`Wokwi-GUEST`) gives simulated devices real internet access.
5. To reach your local Flask server from the simulator, expose it publicly with a tunnel,
   e.g. `ngrok http 5000`, and set `SERVER_HOST` to the ngrok hostname (drop the port,
   use 80/443 and adjust `SERVER_PATH`/`SERVER_PORT` accordingly), **or** simply run the
   Flask server on a small cloud VM and point `SERVER_HOST` at its public IP.
6. Click ▶ Start simulation. Click on the DHT22/DS18B20 parts to change simulated
   temperature/humidity live and watch alerts, OLED, and Flask dashboard react.

## Option B — VS Code + Wokwi extension (recommended for offline dev)
1. Install the **Wokwi Simulator** extension in VS Code + the **Arduino** extension.
2. Open this project folder in VS Code, keep `wokwi.toml` and `diagram.json` at the root
   of the folder you open the simulator from.
3. Compile the sketch with `arduino-cli compile --fqbn esp32:esp32:esp32 firmware/cold_chain_esp32`
   (produces the `.bin`/`.elf` referenced in `wokwi.toml`).
4. Press F1 → "Wokwi: Start Simulator".

## SD card in Wokwi
Wokwi's microSD part simulates a real FAT filesystem backed by a folder on your computer —
you can inspect `backlog.csv` after a simulated offline period directly in that folder,
which is a nice way to *prove* the offline-logging requirement works without needing a
physical SD card and reader for your report/demo.
