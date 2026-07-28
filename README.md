# Smart-Water-Control-System
An untethered, cellular-enabled smart water distribution terminal built with an ESP32 microcontroller and SIM800L GSM modem that tracks real-time fluid flow, streams telemetry to a ThingSpeak cloud broker, and allows instant web dashboard control with automatic volumetric safety shutoffs.
Smart Water Distribution Terminal
An untethered, cellular-enabled IoT water terminal for real-time telemetry, remote cloud control, and automated volumetric safety cutoffs.**

Overview

The **Smart Water Distribution Terminal** is a location-independent IoT solution designed for remote agricultural fields, industrial sites, or municipal water distribution grids where local Wi-Fi networks do not exist. 

Powered by an **ESP32 microcontroller** and a **SIM800L GSM/GPRS module**, the terminal measures real-time fluid flow metrics, transmits telemetry to a **ThingSpeak cloud broker**, and receives remote actuation commands via an **MQTT-over-WebSockets web dashboard**.

 Key Features

 Cellular Decoupling:** Operates over standard 2G/GPRS mobile network towers using AT command routines, eliminating Wi-Fi dependency.
 Dual-Domain Power Supply:** Integrated AC-DC SMPS and an LM2596 buck converter tuned to **4.1V** to absorb 2A cellular transmission spikes without processor resets.
 Real-Time Telemetry:** Tracks live **Flow Rate (L/min) and  Accumulated Volume (L) using hardware interrupt loops on a Hall-Effect flow sensor.
 Bidirectional Cloud Control:** Utilizes ThingSpeak as a central data hub, mapping telemetry and control switches across designated field channels.
  High-Speed Web Dashboard:** A responsive HTML/CSS/JS interface (`work.html`) communicating over an **MQTT WebSocket tunnel** with latency under $50\text{ ms}$.
  Automatic Safety Shutoff:** Onboard local edge logic automatically triggers a **2-channel relay interlock** to snap the motorized valve shut when a target volume limit is reached.

---

## 📐 System Architecture

```text
[ High-Voltage AC Mains ]
           │
           ▼
  [ 12V DC SMPS Supply ] ───► [ 12V Motorized Ball Valve ]
           │                               ▲
           ▼                               │ (Open/Close Coils)
  [ LM2596 Buck Converter (4.1V) ]  [ 2-Channel Relay Module ]
           │                               ▲
           ▼                               │ (GPIO 12 / GPIO 14)
  [ SIM800L GSM Modem ] ◄──UART──► [ ESP32 Core Controller ] ◄──Interrupt── [ YF-S201 Flow Sensor ]
           │                               │
     (GPRS Network)                  (GPRS Network)
           │                               │
           └───────────────┬───────────────┘
                           │
                           ▼
              [ ThingSpeak Cloud Broker ]
                           ▲
                  (MQTT / WebSockets)
                           │
                           ▼
             [ Web Dashboard (work.html) ]


we have made an website first then we shifted towards app making integrate an fully functional integrated with proper structured workflow firebase , multiple pages and everything
