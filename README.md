# embedded-apothekenfinder
displays local pharmacies which are on standby

hardware used:
- ESP8266
- serial text display

libraries used:
- ArduinoJson (https://github.com/bblanchon/ArduinoJson)

A request to the service "apothekenfinder.mobi" is sent regularly.
You must supply longitude and latitude. As a result the service returns a list of pharmacies which are on standby
The list of pharmacies can be filtered by distance.
If the display is located at a pharmacy and this one is on standby, you have the ability to put a custom message on the display instead of the address.
As the data is locally stored, short network issues shouldn't be a problem. Entries get removed automatically if they time out even if network connection gets lost.

