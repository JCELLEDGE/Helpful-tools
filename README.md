# Helpful-tools
Designing helpful tools
JoystickHAT Remote (M5StickC Plus2)

JoystickHAT Remote for ProPresenter
A full‑featured wireless remote for ProPresenter 7, designed for the **M5StickC Plus2** paired with the **M5Stack Joystick HAT**.  
This version includes thumbnails, background media compositing, slide number bar, heartbeat, Wi‑Fi status, ProPresenter reachability, and a polished UI.

Features
- Joystick‑based NEXT / PREV slide control  
- Thumbnail + background compositing  
- Slide number bar with flash feedback  
- Title marquee with auto‑scroll  
- Status bar with Wi‑Fi, ProPresenter, and BKG indicators  
- Heartbeat indicator  
- Multi‑profile support (Wi‑Fi + ProPresenter host/port)  
- Auto‑polling of slide index and transport state  
- Clean, responsive UI optimized for StickC Plus2  

## Hardware Required
- M5StickC Plus2**  
- M5Stack Joystick HAT (U073)**  
- USB‑C cable  
- Wi‑Fi network  
- ProPresenter 7 running on the same network  
Installation Instructions

1. Install Arduino IDE
Download from: https://www.arduino.cc/en/software

2. Install ESP32 Board Support
1. Open Arduino IDE  
2. Go to **File → Preferences**  
3. Add this URL to *Additional Boards Manager URLs*:  https://espressif.github.io/arduino-esp32/package_esp32_index.json
4. Go to **Tools → Board → Boards Manager**  
5. Search **ESP32**  
6. Install version **3.3.5**

3. Install Required Libraries
Open **Tools → Manage Libraries**, then install:

- M5StickCPlus2** (1.0.1)  
- M5Unified** (0.2.11)  
- M5GFX** (0.2.18)  
- ArduinoJson** (7.4.2)  
- MultiButton** (1.3.0)  
- TJpg_Decoder** (1.1.0)

4. Select the Correct Board
Tools → Board → ESP32 → M5Stack M5StickC Plus2

5. Open the Project
Open the folder: JoystickHAT_Remote/JoystickHAT_Remote.ino

6. Upload to the Device
Click **Upload**.

---

Editing Profiles
Profiles are defined at the top of the `.ino` file:
{ "Home PC", "MySSID", "MyPassword", "192.168.1.10", 5005 },
Each profile includes:
•	Display name
•	Wi Fi SSID
•	Wi Fi password
•	ProPresenter host IP
•	ProPresenter port
Usage
•	Use the joystick to navigate slides
•	Press the joystick button or M5 button to dismiss info screens
•	Status bar shows Wi Fi, ProPresenter, and background media state
•	Slide number bar flashes on successful commands
Notes
•	ProPresenter must have the Network API enabled
•	Device must be on the same network as ProPresenter
•	If thumbnails fail to load, check firewall settings
License
This project is provided as is for personal and educational use.


