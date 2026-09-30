# Fuel price tracker for AZ-Touch MOD

This program determines the lowest prices for diesel, regular gasoline, and premium gasoline within a radius of 3, 10, or 20 km and displays the three cheapest providers on an AZ-Touch MOD (an ili9341-based touchscreen). You can set an audible alarm that sounds automatically when the price falls below the set threshold. 

![Fuel price tracker gui](images/AZ-TOUCH-LCARS.jpg)

## Hardware
The demo was written for the AZ-Touch. The [AZ-Touch MOD (for ESP32 DEV KIT C)](https://www.hwhardsoft.de/english/projects/arduitouch-esp/) or the [AZ-Touch Feather](https://www.hwhardsoft.de/english/projects/az-touch-feather/) can be used as hardware. Of course you can also simply connect an ILI9341 based touch screen with jumper wires to a microcontroller board and adapt the code accordingly. 

## Tanker Koenig
To determine current fuel prices, we use Tanker Koenig's API. To take full advantage of the program, you'll need an free API key, which you can request here: 

[Request an API Key](https://onboarding.tankerkoenig.de/)


## Configuration
To take full advantage of the program, you must enter your Wi-Fi password and SSID, as well as the API key mentioned above, in the config.h file.
In addition, you must enter the latitude and longitude of your location.
Once you have updated these values, you can set the DEMO_MODE parameter to false.


## Demo Mode
The app comes with a demo mode enabled by default, which uses randomly generated prices and gas stations. This allows you to test the features without needing an API key or setting up Wi-Fi. 


## License
This library is free software; you can redistribute it and/or modify it under the terms of the GNU Lesser General Public License as published by the Free Software Foundation; either version 2.1 of the License, or (at your option) any later version.

