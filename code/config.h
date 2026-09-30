#pragma once

// true = runs immediately without Wi-Fi or API key using simulated prices.
// false = uses live Tankerkoenig data.
#define DEMO_MODE true

// Copy this file to config.h and enter your own values.
// IMPORTANT: Never publish your personal Tankerkoenig API key.

#define WIFI_SSID       "your-ssid"
#define WIFI_PASSWORD   "your-password"

// Fixed installation location of the AZ-Touch demo
#define LOCATION_LAT    52.00000
#define LOCATION_LNG    12.00000

// Get your personal key at https://creativecommons.tankerkoenig.de/
#define TANKERKOENIG_API_KEY "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"
