# WiFiPowerMeter

Karl Hagström did this great [Power Plug Energy Meter Hack](http://gizmosnack.blogspot.co.uk/2014/10/power-plug-energy-meter-hack.html). This extends that with an ESP8266 to add WiFi capability to send the power readings to the Internet. 

You need:

![A Power Meter](https://github.com/HarringayMakerSpace/WiFiPowerMeter/blob/master/docs/meter1.png)
You should be able to find one for around [£7.50 in places like EBay](http://www.ebay.co.uk/itm/262034181013?_trksid=p2057872.m2749.l2649).

An ESP8266, any model should work, i've used an [ESP-12 with adapter plate](http://www.aliexpress.com/item/Serial-WIFI-ESP8266-module-adapter-plate-Full-IO-port-leads-you-can-choose-ESP-07-ESP/32581237017.html).

[A 3.3v DC power supply.](http://www.ebay.co.uk/itm/231569839422?_trksid=p2057872.m2749.l2649)

Optionally, [a momentary on push button switch.](http://www.ebay.co.uk/itm/Quality-Momentary-Tactile-Push-Button-Switch-SPST-Miniature-Mini-Micro-Small-PCB-/180732232689) I used one desoldered from an old BT Access Point i'd pulled apart for scavenged bits.
 
Open the case of the Power Meter and there is plenty of space inside. On the main board of the Power Meter connect wires from GND, CLK, and MISO to GND, GPIO4, GPIO5 on the ESP, and on the Power Meter connect wires from live and neutral to the 3.3v power supply input, and the power supply output to the ESP Vin and GND. Optionally, I've also added a miniature push switch connected to GPIO-12 and GND on the ESP which when held down at power on has the ESP enter a config mode with Over-The-Air support enabled so you can update the sketch without having to open up the case.

![A Power Meter](https://github.com/HarringayMakerSpace/WiFiPowerMeter/blob/master/docs/meter2.png)




