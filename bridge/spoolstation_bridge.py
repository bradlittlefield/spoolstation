#!/usr/bin/env python3
"""
SpoolStation Bridge - Raspberry Pi
Reads ESP32 station events → logs to Spoolman API → syncs to Snapmaker U1 via Moonraker

INSTALL:
    pip install pyserial requests flask flask-cors

RUN:
    python3 spoolstation_bridge.py

CONFIG:
    Edit the CONFIG block below.
"""

import serial
import json
import math
import time
import threading
import logging
import requests
from datetime import datetime, timezone
from flask import Flask, request, jsonify
from flask_cors import CORS

# see full file in repo
