# GHCPMeter InvertedCYD

This is a small ESP32 CYD client for the local `GHCPMeter` Node helper.

It is based on the hardware wiring and inverted-screen setup from:

- `/home/coreymillia/Documents/complete-projects/WhisplayGroqHat/CompanionCYD/INVERTEDdisplay`

But it is intentionally much smaller:

- stores Wi-Fi + helper host settings in Preferences
- opens a captive setup portal when BOOT is held at power-on
- polls `GET /api/usage`
- can trigger `POST /api/refresh` from the touchscreen
- renders the same core billing values as the browser helper
- supports multiple pages for summary, quota, models, and a 30-day usage graph

## Build

```bash
pio run -d /home/coreymillia/Documents/GHCPMeter/InvertedCYD
```

## Setup

1. Flash the firmware.
2. Hold **BOOT** during power-on to open the setup portal.
3. Join Wi-Fi network `GHCPMeterCYD-Setup`.
4. Open `http://192.168.4.1`.
5. Save:
   - your Wi-Fi SSID/password
   - the GHCPMeter helper host or IP
   - helper port (`8787` by default)
   - polling interval

## Touch controls

- **REFRESH**: fetch the current `/api/usage`
- **SYNC NOW**: tell the helper to refresh GitHub first, then reload usage
- **SETUP**: reopen the local setup portal
- **Tap left side above the bottom buttons**: go to the previous page
- **Tap right side above the bottom buttons**: go to the next page
