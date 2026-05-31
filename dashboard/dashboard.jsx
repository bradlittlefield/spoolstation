import { useState, useEffect, useCallback } from "react";

// CONFIG
const SPOOLMAN_BASE = "http://raspberrypi.local:7912";  // change to your RPi IP
const BRIDGE_BASE   = "http://raspberrypi.local:8765";  // spooldesk_bridge.py