# BGG LED Locator
Supports the https://github.com/sillyfrog/BGGManager to locate specific games in a BoxThrone.

This using the following hardware (anything compatible should be fine, but this is what I'm using):
 - WeMos ESP8266: https://wiki.wemos.cc/products:d1:d1_mini_pro
 - Strips of RGB LED's, such as this: https://www.aliexpress.com/item/1m-2m-3m-4m-5m-ws2812b-ws2812-led-strip-individually-addressable-smart-led-strip-black-white/32682015405.html - I get the "5m 30 IP30" - but there are a *lot* of options, they just need to be based on the "WS2812B" protocol.

This is based on the https://github.com/homieiot/homie-esp8266/ project.

## Flashing the device

I use PlatformIO in VS Code to compile and flash the device. There are 2 parts, the actual code, and the "file system".

### Initial filesystem
To flash the initial filesystem, once PlatformIO is configured, run: `platformio run --target uploadfs` - this will flash it onto the chip.

### Initial program
Next flash the chip with the actual code (in VS Code, run the "PlatfromIO: Upload" command, accessed under Cmd+Shift+P).

Once flashed, and the chip is booted, it will present an AP that you can connect to and configure. By default, name the Homie service `games-leds`, and leave the rest of the options on default where possible (configure WiFi etc as required). Once this is done, it'll connect to your WiFi network.

Now it's on the network, connect to the web interface by browsing to http://<IP address>:88/, here, right at the bottom of the page, enter the number of LED's per strip. Each line in the text area is a new LED strip, up to a maximum of 6 strips.

Click "Save", and then when done, reboot the device. When you reload the web page, it'll have a list of buttons that you can click to turn on the LED to test and calibrate things in the config.json in the BGG Manager. I have found this web service to not be super reliable, so if you have an issue, reboot and try again.

(More updates to come - just getting this live so people can see it.)