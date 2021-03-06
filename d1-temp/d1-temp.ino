//
//  Temperature/Humidity Reporter - https://steve.fi/Hardware/
//
// This program will connect to your WiFi network, serving as
// an access-port if no valid credentials are found.
//
//  Once connected the current humidity and temperature values
// will be submitted to a central MQ topic once every minute.
// the values will also be available in real-time via a built-in
// HTTP-server.
//
//   Steve
//   --
//

#include <ESP8266WiFi.h>
#include <ArduinoOTA.h>
#include <ESP8266HTTPClient.h>

//
// We read/write to SPIFSS.
//
#include <FS.h>


//
// DHT11 / DHT22 sensor interface
//
#include "dht.h"


//
// WiFi setup.
//
#include "WiFiManager.h"


//
// Include the MQQ library and our info-dump class
//
#include "PubSubClient.h"
#include "info.h"


//
// Debug messages over the serial console.
//
#include "debug.h"


//
// The pin we're connecting the sensor to
//
#define DHTPIN D4


//
// Address of our MQ queue
//
char mqtt_server[64] = { '\0' };


//
// The default value if nothing is configured.
//
#define DEFAULT_MQ_SERVER  "10.0.0.10"


//
// Create an MQ client.
//
WiFiClient espClient;
PubSubClient client(espClient);


//
// Helper to dump our details.
//
info board_info;


//
// Last temperature & humidity values which have
// been read.
//
static double last_humidity    = -1;
static double last_temperature = -1;


//
// The HTTP-server we present runs on port 80.
//
WiFiServer server(80);


//
// The name of this project.
//
// Used for:
//   Access-Point name, in config-mode
//   OTA name.
//
#define PROJECT_NAME "D1-TEMP"



//
// Measure the temperature + humidity, then post that to the "temperature"
// topic in MQ.
//
void measureDHT()
{

    //
    // Make a reading
    //
    dht DHT;
    int chk = DHT.read22(DHTPIN);

    //
    // If the reading succeeded
    //
    if (chk == DHTLIB_OK)
    {
        // DISPLAY DATA
        DEBUG_LOG("Humidity: %02d - Temperature %02d\n",
                  DHT.humidity, DHT.temperature);

        // Format it.
        String payload = "{\"temperature\":" + String(DHT.temperature) +
                         ",\"humidity\":" + String(DHT.humidity) +
                         ",\"mac\":\"" + board_info.mac() + "\"}";

        // Publish it
        client.publish("temperature", payload.c_str());

        // Record so that the HTTP-server can serve it.
        last_temperature = DHT.temperature;
        last_humidity = DHT.temperature;

        return;

    }
    else
    {
        //
        // Failure to read the details.
        //

        //
        // Try to work out what kind of failure it was.
        //
        switch (chk)
        {
        case DHTLIB_OK:
            break;

        case DHTLIB_ERROR_CHECKSUM:
            DEBUG_LOG("Checksum error.\n");
            break;

        case DHTLIB_ERROR_TIMEOUT:
            DEBUG_LOG("Time out error.\n");
            break;

        case DHTLIB_ERROR_CONNECT:
            DEBUG_LOG("Connect error.\n");
            break;

        case DHTLIB_ERROR_ACK_L:
            DEBUG_LOG("Ack Low error.\n");
            break;

        case DHTLIB_ERROR_ACK_H:
            DEBUG_LOG("Ack High error.\n");
            break;

        default:
            DEBUG_LOG("Unknown error - %02d\n", chk);
            break;
        }

        return;
    }

}


//
// Called once, when the device boots.
//
void setup()
{
    //
    // Setup the serial-console.
    //
#ifdef DEBUG
    Serial.begin(115200);
#endif

    //
    // Enable access to the filesystem.
    //
    SPIFFS.begin();

    //
    // Configure a sane hostname.
    //
    String id = PROJECT_NAME;
    id += ".";
    id += board_info.mac();


    //
    // Show the ID for debugging purposes.
    //
    DEBUG_LOG("\nDevice ID: %s\n", id.c_str());


    //
    // Set the hostname, and configure
    // the WiFi manager library to
    // work as an access-point, or join
    // the previously saved network.
    //
    WiFi.hostname(id);
    WiFiManager wifiManager;
    wifiManager.autoConnect(id.c_str());

    //
    // Now we're connected show the local IP address.
    //
    DEBUG_LOG("\nWiFi Connected with IP %s\n",
              WiFi.localIP().toString().c_str());

    //
    // Allow over the air updates
    //
    // This is documented here:
    //     https://randomnerdtutorials.com/esp8266-ota-updates-with-arduino-ide-over-the-air/
    //
    // Hostname defaults to esp8266-[ChipID]
    //
    ArduinoOTA.setHostname(PROJECT_NAME);

    ArduinoOTA.onStart([]()
    {
        DEBUG_LOG("OTA Start\n");
    });
    ArduinoOTA.onEnd([]()
    {
        DEBUG_LOG("OTA End\n");
    });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total)
    {
        char buf[16];
        memset(buf, '\0', sizeof(buf));
        snprintf(buf, sizeof(buf) - 1, "Upgrade - %02u%%          ", (progress / (total / 100)));
        DEBUG_LOG(buf);
        DEBUG_LOG("\n");
    });
    ArduinoOTA.onError([](ota_error_t error)
    {
        if (error == OTA_AUTH_ERROR)
            DEBUG_LOG("Auth Failed\n");
        else if (error == OTA_BEGIN_ERROR)
            DEBUG_LOG("Begin Failed\n");
        else if (error == OTA_CONNECT_ERROR)
            DEBUG_LOG("Connect Failed\n");
        else if (error == OTA_RECEIVE_ERROR)
            DEBUG_LOG("Receive Failed\n");
        else if (error == OTA_END_ERROR)
            DEBUG_LOG("End Failed\n");
    });

    //
    // Ensure the OTA process is running & listening.
    //
    ArduinoOTA.begin();

    //
    // Start our HTTP server
    //
    server.begin();
    DEBUG_LOG("HTTP-Server started on http://%s/\n",
              WiFi.localIP().toString().c_str());

    //
    // Load the MQ address, if we can.
    //
    String mq_str = read_file("/mq.addr");

    //
    // If we did make it live, otherwise use the default value.
    //
    if (mq_str.length() > 0)
        strcpy(mqtt_server, mq_str.c_str());
    else
        strncpy(mqtt_server, DEFAULT_MQ_SERVER, sizeof(mqtt_server) - 1);


    //
    // Setup our pub-sub connection.
    //
    client.setServer(mqtt_server, 1883);
    client.setCallback(callback);
}


//
// Called constantly.
//
// Read the value of the temperature-sensor, and submit it, once a minute.
//
void loop()
{
    //
    // Handle any pending over the air updates.
    //
    ArduinoOTA.handle();

    //
    // The time we last read the temperature
    //
    static long last_read = 0;

    //
    // Ensure we're connected to our queue.
    //
    if (!client.connected())
        reconnect();

    //
    // Handle queue messages.
    //
    client.loop();

    // Get the current time.
    long now = millis();

    // If we've not updated, or it was >60 seconds, then update
    if ((last_read == 0) || (abs(now - last_read) > 60 * 1000))
    {
        measureDHT();
        last_read = now;
    }


    //
    // Check if a client has connected to our HTTP-server.
    //
    // If so handle it.
    //
    // (This allows changing the stop, timezone, backlight, etc.)
    //
    WiFiClient client = server.available();

    if (client)
        processHTTPRequest(client);

}


//
// This is called when messages are received.
//
// We only subscribe to the `meta`-topic, and we don't do
// anything with the messages.  It's just a nice example
// showing how we could if we wanted to.
//
//
void callback(char* topic, byte* payload, unsigned int length)
{
    Serial.print("Message arrived [Topic:");
    Serial.print(topic);
    Serial.print("] ");

    for (int i = 0; i < length; i++)
    {
        Serial.print((char)payload[i]);
    }

    Serial.println();
}


//
// Reconnect to the pub-sub-server, if we're dropped.
//
void reconnect()
{
    // Loop until we're reconnected
    while (!client.connected())
    {
        DEBUG_LOG("Attempting to connect to MQ ..");

        String id = PROJECT_NAME;
        id += board_info.mac();

        // Attempt to connect
        if (client.connect(id.c_str()))
        {
            // We've connected
            DEBUG_LOG("\tconnected\n");

            //
            // Dump all our local details to the meta-topic
            //
            client.publish("meta", board_info.to_JSON().c_str());

            //
            // Subscribe to the `meta`-topic.
            //
            client.subscribe("meta");
        }
        else
        {
            DEBUG_LOG("\tfailed, rc=%02d, will retry in 5 seconds.\n",
                      client.state());
            delay(5000);
        }
    }

}






//
// Process an incoming HTTP-request.
//
void processHTTPRequest(WiFiClient client)
{
    // Wait until the client sends some data
    while (client.connected() && !client.available())
        delay(1);

    // Read the first line of the request
    String request = client.readStringUntil('\r');
    client.flush();

    // Change the MQ server?
    if (request.indexOf("/?mq=") != -1)
    {
        char *pattern = "/?mq=";
        char *s = strstr(request.c_str(), pattern);

        if (s != NULL)
        {
            // Temporary holder for the value - as a string.
            char tmp[64] = { '\0' };

            // Skip past the pattern.
            s += strlen(pattern);

            // Add characters until we come to a terminating character.
            for (int i = 0; i < strlen(s); i++)
            {
                if ((s[i] != ' ') && (s[i] != '&'))
                    tmp[strlen(tmp)] = s[i];
                else
                    break;
            }

            // Write the data to a file.
            write_file("/mq.addr", tmp);

            // Update the queue address
            memset(mqtt_server, '\0', sizeof(mqtt_server));
            strncpy(mqtt_server, tmp, sizeof(mqtt_server) - 1);

            // TODO - force reconnect
        }

        // Redirect to the server-root
        redirectIndex(client);
        return;
    }


    // Return a simple response
    serveHTML(client);

}


//
// Serve a redirect to the server-root
//
void redirectIndex(WiFiClient client)
{
    client.println("HTTP/1.1 302 Found");
    client.print("Location: http://");
    client.print(WiFi.localIP().toString().c_str());
    client.println("/");
}


//
// This is a bit horrid.
//
// Serve a HTML-page to any clients who connect, via a browser.
//
void serveHTML(WiFiClient client)
{
    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: text/html");
    client.println("");

    client.println("<!DOCTYPE html>");
    client.println("<html lang=\"en\">");
    client.println("<head>");
    client.println("<title>Temperature &amp; Humidity</title>");
    client.println("<meta charset=\"utf-8\">");
    client.println("<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">");
    client.println("<link href=\"https://maxcdn.bootstrapcdn.com/bootstrap/3.3.7/css/bootstrap.min.css\" rel=\"stylesheet\" integrity=\"sha384-BVYiiSIFeK1dGmJRAkycuHAHRg32OmUcww7on3RYdg4Va+PmSTsz/K68vbdEjh4u\" crossorigin=\"anonymous\">");
    client.println("<script src=\"//code.jquery.com/jquery-1.12.4.min.js\"></script>");
    client.println("<script src=\"https://maxcdn.bootstrapcdn.com/bootstrap/3.3.7/js/bootstrap.min.js\" integrity=\"sha384-Tc5IQib027qvyjSMfHjOMaLkfuWVxZxUPnCJA7l2mCWNIpG9mGCD8wGNIcPD7Txa\" crossorigin=\"anonymous\"></script>");
    client.println("</head>");
    client.println("<body>");
    client.println("<nav id=\"nav\" class = \"navbar navbar-default\" style=\"padding-left:50px; padding-right:50px;\">");
    client.println("<div class = \"navbar-header\">");
    client.println("<h1 class=\"banner\"><a href=\"/\">Temperature &amp; Humidity</a> - <small>by Steve</small></h1>");
    client.println("</div>");
    client.println("<ul class=\"nav navbar-nav navbar-right\">");
    client.println("<li><a href=\"https://steve.fi/Hardware/\">Steve's Projects</a></li>");
    client.println("</ul>");
    client.println("</nav>");
    client.println("<div class=\"container-fluid\">");

    // Start of body
    // Row
    client.println("<div class=\"row\">");
    client.println("<div class=\"col-md-3\"></div>");
    client.println("<div class=\"col-md-9\"><h1>Temperature &amp; Humidity</h1><p>&nbsp;</p></div>");
    client.println("</div>");
    client.println("<div class=\"row\">");
    client.println("<div class=\"col-md-4\"></div>");
    client.println("<div class=\"col-md-4\">");
    client.println("<table class=\"table table-striped table-hover table-condensed table-bordered\">");

    client.println("<tr><td>Temperature</td><td> ");
    client.println(last_temperature);
    client.println("</td></tr>");

    client.println("<tr><td>Humidity</td><td> ");
    client.println(last_humidity);
    client.println("</td></tr>");

    client.println("</table>");
    client.println("</div>");
    client.println("<div class=\"col-md-4\"></div>");
    client.println("</div>");

    // Row
    client.println("<div class=\"row\">");
    client.println("<div class=\"col-md-3\"></div>");
    client.println("<div class=\"col-md-9\"> <h2>Network Details</h2></div>");
    client.println("</div>");
    client.println("<div class=\"row\">");
    client.println("<div class=\"col-md-4\"></div>");
    client.println("<div class=\"col-md-4\">");
    client.print("<p>This device has the IP address <code>");
    client.print(WiFi.localIP());
    client.println("</code>, and is configured to send data to the following MQ server:</p>");
    client.println("<form action=\"/\" method=\"GET\"><input type=\"text\" name=\"mq\" value=\"");
    client.print(mqtt_server);
    client.println("\"><input type=\"submit\" value=\"Update\"></form>");
    client.println("</div>");
    client.println("<div class=\"col-md-4\"></div>");
    client.println("</div>");

    // End of body
    client.println("</div>");
    client.println("</body>");
    client.println("</html>");

}





//
// Open the given file for writing, and write
// out the specified data.
//
void write_file(const char *path, const char *data)
{
    File f = SPIFFS.open(path, "w");

    if (f)
    {
        DEBUG_LOG("Writing data '%s' to file '%s'\n", data, path);

        f.println(data);
        f.close();
    }
}

//
// Read a single line from a given file, and return it.
//
// This function explicitly filters out `\r\n`, and only
// reads the first line of the text.
//
String read_file(const char *path)
{
    String line;

    File f = SPIFFS.open(path, "r");

    if (f)
    {
        line = f.readStringUntil('\n');
        f.close();
    }


    String result;

    for (int i = 0; i < line.length() ; i++)
    {
        if ((line[i] != '\n') &&
                (line[i] != '\r'))
            result += line[i];
    }

    return (result);
}
