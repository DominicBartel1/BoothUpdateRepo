#include <iostream>
#include <Fedm.h>
#include <glib.h>
#include <gio/gio.h>
#ifdef G_OS_UNIX
#include <gio/gunixfdlist.h>
#include <glib-unix.h>
#endif
#include <json-glib/json-glib.h>
#include <mosquitto.h>
#include <sqlite3.h>
#include <unistd.h>

using namespace FEDM;
using namespace std;

#define CONFIG_FILE "/usr/fcos/projects/sunless/conf/consumablesd.conf"
#define MQTT_SERVER "localhost"
#define MQTT_PORT 1883
#define MQTT_USERNAME "sunless"
#define MQTT_PASSWORD "BDf@v3JsryLn25YX"
#define SUNLESS "5F53756E6C657373696E632E"
#define MICROSECOND 1000000
#define TAGRESET 15

enum drawer { NONE, BOTTOM, TOP };

typedef struct _sunlessTag
{
	guint64 key;
	guint64 scanTime;
	guint drawer;
	guint position;
	gchar *sku;
	gchar *serialNumber;
	gchar *data;
	gchar *batchInfo;
	guint batchMonth;
	guint batchYear;
  guint readCount;
	gboolean isProduct;
	gboolean isPosition;
  string tagString;
} sunlessTag;

GSList *removedTags = NULL;
vector<map<string, sunlessTag>> positionData(6);
/*
 * NOTE: 0-3 are bottom drawer.  4-9 are top drawer.
 */
sunlessTag *installedTags[10] = { NULL, NULL, NULL, NULL, NULL,
	NULL, NULL, NULL, NULL, NULL };
static gboolean verbose = FALSE;
static gboolean top = FALSE;
static gboolean bottom = FALSE;
static gboolean useMqtt = FALSE;
static gboolean fullyExtended = FALSE;

static GError *err = NULL;
static gint currentDrawer = NONE;
static ReaderModule *reader = new ReaderModule(RequestMode::UniDirectional);
struct mosquitto *mosq;

static GOptionEntry entries[] =
{
	{ "verbose", 'v', 0, G_OPTION_ARG_NONE, &verbose, "Be verbose", NULL },
	{ "top", 't', 0, G_OPTION_ARG_NONE, &top, "Read from top drawer", NULL },
	{ "bottom", 'b', 0, G_OPTION_ARG_NONE, &bottom, "Read from bottom drawer", NULL },
	{ "mqtt", 'm', 0, G_OPTION_ARG_NONE, &useMqtt, "Send results over MQTT", NULL },
	{ NULL }
};
volatile sig_atomic_t listen = TRUE;

int myStrlen(char *s)
{
    char *p = s;
    while (*p != '\0')
        p++;
    return p - s;
}

int valueOf(char symbol)
{
	switch (symbol)
	{
		case '0': return 0;
  		case '1': return 1;
  		case '2': return 2;
  		case '3': return 3;
 		case '4': return 4;
  		case '5': return 5;
 		case '6': return 6;
  		case '7': return 7;
  		case '8': return 8;
  		case '9': return 9;
  		case 'A': return 10;
		case 'B': return 11;
		case 'C': return 12;
		case 'D': return 13;
		case 'E': return 14;
		case 'F': return 15;
		default:
				  return -1;
	}
}

char *hexToAscii(char hex[])
{
	int hexLength = myStrlen(hex);
	char* text = NULL;

	if (hexLength > 0)
	{
		int symbolCount;

		symbolCount = hexLength / 2;
		text = (char *)malloc(symbolCount + 1);
		int lastIndex = hexLength - 1;
		for (int i = lastIndex; i >= 0; --i)
		{
			if (((lastIndex - i) % 2 != 0))
			{
				int dec = 16 * valueOf(hex[i]) + valueOf(hex[i+1]);
				text[i/2] = dec;
			}
			else if(i == 0)
			{
				int dec = valueOf(hex[0]);
				text[0] = dec;
			}
		}
		text[symbolCount] = '\0';
	}
	return text;
}

//This should probably be at least two functions with one being recursive
void parseTagData(){
  g_debug("Parsing tag data...");
  guint offset = top ? 0 : 4;
  //Iterate drawers
  for(guint i = 0; i < 6; i++){
    g_debug("-------------------");
    //Iterate drawer tags seen
    guint secondMostScans = 0;
    string secondKey = "";
    guint mostScans = 0;
    string key = "";
    map<string, sunlessTag>::iterator it;
    for (it = positionData[i].begin(); it != positionData[i].end(); it++)
    {
      //update most scanned tag
      if(positionData[i][it->first].readCount > mostScans){
        //if most scanned tag exists move to second most scanned
        if(mostScans > 0){
          secondMostScans = mostScans;
          secondKey = key;
        }
        mostScans = positionData[i][it->first].readCount;
        key = it->first;
      }
      g_debug("Serial: %s", positionData[i][it->first].serialNumber);
      g_debug("Sku: %s", positionData[i][it->first].sku);
      g_debug("ReadCount: %d", positionData[i][it->first].readCount);
      g_debug("Position: %d", positionData[i][it->first].position);
    }
    // find duplicate
    sunlessTag *duplicateTag = NULL;
    int duplicatePosition = -1;
    g_debug("Checking duplicates");
    for(guint j = 0; j < 10; j++){
      if(installedTags[j] && mostScans > 0){
        if(key.compare(installedTags[j]->tagString) == 0){
          g_debug("Found duplicate tag: pos %d", j);
          duplicateTag = installedTags[j];
          duplicatePosition = j;
        }
      }
    }

    if(mostScans > 0){
      //check for duplicate
      if(!duplicateTag){
        installedTags[i + offset] = &positionData[i][key];
      }else{
        // If current tag belongs to this location
        guint reMost = 0;
        string reKey = "";
        guint reSecondMost = 0;
        string reSecondKey = "";
        map<string, sunlessTag>::iterator reIt;
        if(positionData[i][key].readCount > duplicateTag->readCount){
          g_debug("Current tag more scanned than duplicate: pos %d", duplicatePosition);
          installedTags[i + offset] = &positionData[i][key];
          // re-check previous location for other tags
          for (reIt = positionData[duplicatePosition].begin(); reIt != positionData[duplicatePosition].end(); reIt++)
          {
            if(positionData[duplicatePosition][reIt->first].readCount > reMost){
              if(reMost > 0){
                reSecondMost = reMost;
                reSecondKey = reKey;
              }
              reMost = positionData[duplicatePosition][reIt->first].readCount;
              reKey = positionData[duplicatePosition][reIt->first].key;
            }
          }
          if(reSecondMost > 0 && reSecondKey.compare(duplicateTag->tagString) != 0){
            installedTags[duplicatePosition] = &positionData[duplicatePosition][reSecondKey];
            g_debug("Second most scanned tag at position %d found", duplicatePosition );
          }else{
            installedTags[duplicatePosition] = NULL;
          }
        }else{
          //Use second most scanned tag, if exists
          if(secondMostScans > 0){
            installedTags[i + offset] = &positionData[i][secondKey];
            g_debug("Second most scanned tag at position %d used", i + offset );
          }else{
            // removedTags = g_slist_append(removedTags, installedTags[i + offset]);
            installedTags[i + offset] = NULL;
          }
        }
      }
    }else{
      // removedTags = g_slist_append(removedTags, installedTags[i + offset]);
      installedTags[i + offset] = NULL;
    }
  }
}

gint comparePosition(gpointer a, gpointer b)
{
	sunlessTag *alpha = (sunlessTag *)a;
	sunlessTag *beta = (sunlessTag *)b;

	if ((alpha) && (beta))
		return (gint)(alpha->position - beta->position);
	if ((alpha) && (!beta))
		return 1;
	if ((!alpha) && (beta))
		return -1;
	return 0;
}

gint compareKey(gpointer a, gpointer b)
{
	sunlessTag *alpha = (sunlessTag *)a;
	sunlessTag *beta = (sunlessTag *)b;

	if ((alpha) && (beta))
		return (gint)(alpha->key - beta->key);
	if ((alpha) && (!beta))
		return 1;
	if ((!alpha) && (beta))
		return -1;
	return 0;
}

sunlessTag *findTag(guint64 key)
{
	GSList *foo = NULL;
	sunlessTag temp;

	temp.key = key;
	foo = g_slist_find_custom(removedTags, &temp,
			(GCompareFunc)compareKey);
	return (foo ? (sunlessTag *)foo->data : NULL);
}

void freeTag(sunlessTag *tag)
{
	if (tag == NULL)
		return;
	g_free(tag->sku);
	g_free(tag->serialNumber);
	g_free(tag->data);
}

sunlessTag *newTag(gchar *dataString)
{
	sunlessTag *tag = NULL;

	tag = g_new0(sunlessTag, 1);
	tag->drawer = currentDrawer;
	tag->data = g_strdup(dataString);
  tag->tagString = dataString;
	if ((dataString[0] == '5') && (dataString[1] == 'F') &&
		(dataString[14] == '5') && (dataString[15] == 'F'))
	{
		tag->isProduct = true;
		tag->isPosition = false;
	}
	else
	{
		tag->isProduct = false;
		tag->isPosition = true;
	}
	if (tag->isProduct)
	{
		gulong sN = 0;
		guint sku = 0;
		tag->sku = hexToAscii(g_strndup(&dataString[2], 12));
		tag->serialNumber = hexToAscii(g_strndup(&dataString[40], 32));
		tag->batchInfo = hexToAscii(g_strndup(&dataString[72], 8));
		tag->batchMonth = atol(hexToAscii(g_strndup(&dataString[80], 4)));
		tag->batchYear = atol(hexToAscii(g_strndup(&dataString[84], 4)));
		sku = atol(tag->sku);
		sN = atol(tag->serialNumber);
		tag->key = (10000000000000000 * (sku - 202200)) + sN;
	}
	else
	{
		tag->position = atol(hexToAscii(g_strndup(dataString, 8)));
	}
	tag->scanTime = g_get_monotonic_time();

	g_debug("readConsumables: Drawer: %u", tag->drawer);
	g_debug("readConsumables: Position: %u", tag->position);
	g_debug("readConsumables: SKU: %s", tag->sku);
	g_debug("readConsumables: Serial Number: %s", tag->serialNumber);
	g_debug("readConsumables: Key: %" G_GUINT64_FORMAT "", tag->key);
	g_debug("readConsumables: Data: %s", tag->data);
	return tag;
}

void dumpTag(sunlessTag *tag)
{
	if (tag != NULL)
	{
		g_message("Drawer: %u", tag->drawer);
		g_message("Position: %u", (tag->drawer == TOP) ? tag->position - 4 : tag->position);
		g_message("SKU: %s", tag->sku);
		g_message("Serial Number: %s", tag->serialNumber);
		g_message("Key: %" G_GUINT64_FORMAT "", tag->key);
		g_message("Data: %s", tag->data);
	}
	else
	{
		g_message("NULL");
	}
}

gchar *dumpTagAsJSON(sunlessTag *tag)
{
	gchar *foo = NULL;
	if (tag)
	{
		foo = g_strdup_printf("{ \"id\": %" G_GUINT64_FORMAT ", \"sku\": \"%s\", \"serial_num\": \"%s\", \"tag_data\": \"%s\", \"batch_tag\": \"%s\", \"batch_month\": %d, \"batch_year\": %d, \"position\": %d, \"drawer\": %d }",
				tag->key,
				tag->sku,
				tag->serialNumber,
				tag->data,
				tag->batchInfo,
				tag->batchMonth,
				tag->batchYear,
				(tag->drawer == TOP) ? tag->position - 4 : tag->position,
				tag->drawer);
	}
	return foo;
}

void printReaderInfo(ReaderModule &reader)
{
	g_debug("readConsumables: deviceId: %s", (reader.info().deviceIdToHexString()).c_str());
	g_debug("readConsumables: readerType: %s", (reader.info().readerTypeToString()).c_str());
	g_debug("readConsumables: readerReport: %s", (reader.info().getReport()).c_str());
}

gboolean selectMux(ReaderModule &reader, int muxNumber)
{
	string requestProtocol;
	string respProtocol = "";
	int returnCode;

	switch (muxNumber)
	{
		case 1:
			requestProtocol.assign("BF010020021F02DDFE000100");
			break;
		case 2:
			requestProtocol.assign("BF010020021F02DDFE000200");
			break;
		case 3:
			requestProtocol.assign("BF010020021F02DDFE000300");		// Unused in Sunless
			break;
		case 4:
			requestProtocol.assign("BF010020021F02DDFE000400");		// Unused in Sunless
			break;
		default:
			requestProtocol.assign("BF010020021F02DDFE000400");		// Unused in Sunless
	}
	returnCode = reader.transceive(requestProtocol, respProtocol);
	if (returnCode != ErrorCode::Ok)
	{
		g_debug("readConsumables: error setting mux %d active", muxNumber);
		return false;
	}
	g_message("readConsumables: mux %d selected asd", muxNumber);
	return true;
}

void rfOn(ReaderModule &reader, int antennaNumber)
{
	bool maintainHostMode = false;
	bool dcOn = false;
	int state = reader.rf().on(antennaNumber, maintainHostMode, dcOn);
	if (state != ErrorCode::Ok)
	{
		cerr << "Error in rfOn: " << ErrorCode::toString(state) << endl;
	}
	else
		cout << antennaNumber << " enabled!" << endl;
}

void rfOff(ReaderModule &reader)
{
	// Turns off ALL antennas
	int state = reader.rf().off();
	if (state != ErrorCode::Ok)
	{
		cerr << "Error in rfOff: " << ErrorCode::toString(state) << endl;
	}
	else
		cout << "Antennas off" << endl;
}

void loadConf(ReaderModule &reader)
{
	int state = reader.config().transferXmlFileToReaderCfg(CONFIG_FILE);
	if (state != ErrorCode::Ok)
	{
		cerr << "Error in loadConf: " << ErrorCode::toString(state) << endl;
	}
}

void mqttConnect(struct mosquitto *mosq, void *obj, int reasonCode)
{
	int rc;

	if (reasonCode != 0)
	{
		g_warning("consumablesd: Couldn't connect to MQTT broker: %s", mosquitto_connack_string(reasonCode));
		mosquitto_disconnect(mosq);
	}
	rc = mosquitto_subscribe(mosq, NULL, "sunless/lock_drawer", 1);
	if (rc != MOSQ_ERR_SUCCESS)
	{
		g_message("consumablesd: Couldn't subscribe to 'lock_drawer' topic: %s", mosquitto_strerror(rc));
		mosquitto_disconnect(mosq);
	}
}

void mqttSubscribe(struct mosquitto *mosq, void *obj, int mid, int qosCount, const int *grantedQos)
{
	int i;

	g_debug("consumablesd: Subscribed (mid: %d): %d", mid, grantedQos[0]);
	for (i = 1; i < qosCount; i++)
	{
		g_debug("consumablesd: %d:granted qos = %d\n", i, grantedQos[i]);
	}
}

void mqttMessage(struct mosquitto *mosq, void *obj, const struct mosquitto_message *message)
{
	g_debug("ConsumabelMessage");
	if (!g_strcmp0(message->topic, "sunless/lock_drawer"))
	{
		if (!g_ascii_strncasecmp((char *)message->payload, "BOTTOM", message->payloadlen) && (currentDrawer == BOTTOM))
		{
			listen = FALSE;
		}
		if (!g_ascii_strncasecmp((char *)message->payload, "TOP", message->payloadlen) && (currentDrawer == TOP))
		{
			listen = FALSE;
		}
	}
}

void mqttPublish(struct mosquitto *mosq, void *obj, int mid)
{
	g_message("consumablesd: Message with mid %d has been published.", mid);
}

int mqPublish(gchar *topic, gchar *payload) {
	int rc;
	int mid = 0;
	int len = payload == NULL ? 0 : strlen(payload);
	rc = mosquitto_publish(mosq, &mid, topic, len, payload, 2, false);
	g_debug("publishing to topic '%s' mid(%d) rc(%d) payload:'%s'", topic, mid, rc, payload);
	if (rc != MOSQ_ERR_SUCCESS) {
		g_warning("sunlessd: Couldn't publish MQTT message: %s", mosquitto_strerror(rc));
	}
	return rc;
}

void publishError(int errCode, gchar *errMessage) {
	if (!useMqtt) {
		return;
	}
	gchar *payload;
	payload = g_strdup_printf("{ \"code\":%d, \"message\": \"%s\" }", errCode, errMessage);
	mqPublish((gchar *)"sunless/error", payload);
	g_free(payload);
}

int main(int argc, char **argv)
{
	GOptionContext *context;
	InventoryParam inventoryParam;
	Connector usbConnector;
	int state;
	std::unique_ptr<TagHandler::ThBase> th;
	// gboolean badCombo = false;
	sunlessTag *tempTag = NULL;
	sunlessTag *positionTag = NULL;
	sunlessTag *productTag = NULL;
	int rc;
	gchar *tempString = NULL;

	context = g_option_context_new("- read NFC tags");
	g_option_context_add_main_entries(context, entries, NULL);
	if (!g_option_context_parse (context, &argc, &argv, &err))
	{
		g_print("readConsumables: option parsing failed: %s\n", err->message);
		g_free(context);
		g_free(err);
		exit(EXIT_FAILURE);
	}
	if (!top && !bottom)
	{
		g_print("readConsumables: Need a drawer specified\n");
		return(EXIT_FAILURE);
	}
	if (verbose)
		g_setenv("G_MESSAGES_DEBUG", "all", TRUE);
	else
		g_unsetenv("G_MESSAGES_DEBUG");

	if (useMqtt)
	{
		mosquitto_lib_init();
		mosq = mosquitto_new(NULL, true, NULL);
		if (mosq == NULL)
		{
			g_message("readConsumables: Unable to create mqtt instance");
			mosquitto_lib_cleanup();
			return(EXIT_FAILURE);
		}
		mosquitto_connect_callback_set(mosq, mqttConnect);
		mosquitto_subscribe_callback_set(mosq, mqttSubscribe);
		mosquitto_message_callback_set(mosq, mqttMessage);
		mosquitto_publish_callback_set(mosq, mqttPublish);
		if (mosquitto_username_pw_set(mosq, MQTT_USERNAME, MQTT_PASSWORD) != MOSQ_ERR_SUCCESS)
		{
			g_message("readConsumables: Unable to set mqtt username/password");
			mosquitto_destroy(mosq);
			mosquitto_lib_cleanup();
			return(EXIT_FAILURE);
		}
		if (mosquitto_connect(mosq, MQTT_SERVER, MQTT_PORT, 60) != MOSQ_ERR_SUCCESS)
		{
			g_message("readConsumables: Unable to connect to MQTT server");
			mosquitto_destroy(mosq);
			mosquitto_lib_cleanup();
			return(EXIT_FAILURE);
		}
		if (mosquitto_loop_start(mosq) != MOSQ_ERR_SUCCESS)
		{
			g_message("readConsumables: Unable to connect to start MQTT thread");
			mosquitto_destroy(mosq);
			mosquitto_lib_cleanup();
			return(EXIT_FAILURE);
		}
		g_message("readConsumables: Connected to MQTT broker");
	}

	g_debug("readConsumables: attempting to find and connect to usb FEIG device");
	int scan = UsbManager::startDiscover();
	if (scan <= 0)
	{
		g_message("readConsumables: NO SCANNER FOUND");
		publishError(7130, (gchar *)"readConsumables: NO SCANNER FOUND!");
		return(EXIT_FAILURE);
	}
	usbConnector = UsbManager::popDiscover().connector();
	reader->connect(usbConnector);
	if (reader->lastError() != ErrorCode::Ok)
	{
		g_debug("readConsumables: error connecting to device: %s", (reader->lastErrorText()).c_str());
		reader->disconnect();
		if (reader->lastError() == ErrorCode::Ok) {
			g_debug("readConsumables: %s disconnected.", (reader->info().readerTypeToString()).c_str());
		}
		publishError(7130, (gchar *)"readConsumables: error connecting to device!");
		return(EXIT_FAILURE);
	}
	if (verbose)
		printReaderInfo(*reader);

	loadConf(*reader);
	selectMux(*reader, top ? TOP : BOTTOM);
	if (top)
		currentDrawer = TOP;
	else
		currentDrawer = BOTTOM;

  // Maintain current position regardless of if tag is seen
  guint currentPosition = top ? 6 : 4;
  for(guint i = 0; i < currentPosition; i++){
    std::map<string, sunlessTag> m;
    positionData[i] = m;
  }

	while (listen)
	{
		state = reader->hm().inventory(true, inventoryParam);
		if (state == ErrorCode::Ok)
		{

			// We have tags!
			size_t count = reader->hm().itemCount();


			g_debug("readConsumables: %lu tag(s) found.", count);
      //iterate position first
      for (size_t i = 0; i < count; i++)
      {
        TagItem tagItem = reader->hm().tagItem(i);

        th = reader->hm().createTagHandler(tagItem);
        TagHandler::ThIso15693 *tag = (TagHandler::ThIso15693 *)th.get();
        g_debug("readConsumables: Manufacturer: %s", (tag->manufacturerName()).c_str());

        TagHandler::ThIso15693::SystemInfo si;
        state = tag->getSystemInformation(si, false);
        if (state == ErrorCode::Ok)
        {
          vector<uint8_t> data;
          string dataString;

          size_t blockSize;
          state = tag->readMultipleBlocks(0x00, 11, blockSize, data);
          string result;
          Utility::HexConvert::vectorToString(data, result, 0);
          if (g_ascii_strncasecmp(&result[16], SUNLESS, 24) == 0)
          {
            tempTag = newTag((gchar *)result.c_str());
            // Change positions when only one position tag is seen
            if (tempTag->isPosition && !positionTag)
            {
              g_debug("currentPosition: %d", currentPosition);
              g_debug("tempPosition: %d", tempTag->position);
              // if(true){
              if (tempTag->position != currentPosition)
              {
                // Alert for position change
                g_debug("RFID position change: %d", currentPosition);
                if (tempTag->position == 1)
                {
                    if (!fullyExtended)
                        rc = mosquitto_publish(mosq, NULL, "sunless/drawer_extended", 0, NULL, 2, false);
                    fullyExtended = TRUE;
                }
                gchar *message = g_strdup_printf("{ \"drawerPosition\": \"%d\" }", tempTag->position);
                rc = mosquitto_publish(mosq, NULL, "sunless/drawer_position", strlen(message), message, 2, false);
              }
              currentPosition = tempTag->position;
            }
          }
        }
      }
      //Iterate inventory tags
      for (size_t i = 0; i < count; i++)
      {
        TagItem tagItem = reader->hm().tagItem(i);

        th = reader->hm().createTagHandler(tagItem);
        TagHandler::ThIso15693 *tag = (TagHandler::ThIso15693 *)th.get();
        g_debug("readConsumables: Manufacturer: %s", (tag->manufacturerName()).c_str());

        TagHandler::ThIso15693::SystemInfo si;
        state = tag->getSystemInformation(si, false);
        if (state == ErrorCode::Ok)
        {
          vector<uint8_t> data;
          string dataString;

          size_t blockSize;
          state = tag->readMultipleBlocks(0x00, 11, blockSize, data);
          string result;
          Utility::HexConvert::vectorToString(data, result, 0);
          g_debug("readConsumables: afi: 0x%x", si.afi());
          g_debug("readConsumables: dsfId: 0x%x", si.dsfId());
          g_debug("readConsumables: Tag Data Block Size: %lu", blockSize);
          g_debug("readConsumables: Tag Data: %s", result.c_str());
          if (g_ascii_strncasecmp(&result[16], SUNLESS, 24) == 0)
          {
            g_debug("readConsumables: %zu Sunless Tags Found!", count);
            tempTag = newTag((gchar *)result.c_str());
            if (tempTag->isProduct && fullyExtended)
            {
              tempTag->position = (currentDrawer == BOTTOM) ? currentPosition : currentPosition + 4;
              tempTag->readCount = 1;
              if(positionData[currentPosition-1].find(result.c_str()) != positionData[currentPosition-1].end()){
                positionData[currentPosition-1][result.c_str()].readCount++;
              }else{
                positionData[currentPosition-1][result.c_str()] = *tempTag;
                // positionData[currentPosition].insert(std::map<string, sunlessTag>::value_type(result.c_str(), tempTag));
              }
            }
          }
        }
      }
		}
		// badCombo = false;
		tempTag = positionTag = productTag = NULL;
		usleep(50);
	}


	selectMux(*reader, NONE);
	if (useMqtt)
	{
		if (fullyExtended)
		{
      // Sort data before attempting storage.
      parseTagData();
			tempString = g_strdup("[ ");
			for (int i = 0; i < 10; i++)
			{
				if (installedTags[i])
				{
					tempString = g_strconcat(tempString, dumpTagAsJSON(installedTags[i]), NULL);
					tempString = g_strconcat(tempString, ",", NULL);
				}
			}
			tempString = g_strndup(tempString, myStrlen(tempString) - 1);
			tempString = g_strconcat(tempString, " ]", NULL);
			rc = mosquitto_publish(mosq, NULL, "sunless/consumables", strlen(tempString), tempString, 2, false);
			usleep(10000);
			if (rc != MOSQ_ERR_SUCCESS)
				g_warning("consumablesd: Couldn't publish MQTT message: %s", mosquitto_strerror(rc));
			g_free(tempString);
		}
		else
		{
			// Send error message that drawer wasn't fully opened
			publishError(7130, (gchar *)"readConsumables: Drawer not fully opened!");
		}
	}
	else
	{
		for (int i = 0; i < 10; i++)
		{
			if (installedTags[i])
				dumpTag(installedTags[i]);
		}
	}
	// Once main loop is terminated...
	reader->disconnect();
	g_slist_free_full(removedTags, (GDestroyNotify) freeTag);
	removedTags = NULL;
	if (reader->lastError() == ErrorCode::Ok)
		g_debug("readConsumables: %s disconnected.", (reader->info().readerTypeToString()).c_str());
	g_message("readConsumables: Exiting...");
	if (useMqtt)
	{
		mosquitto_disconnect(mosq);
		mosquitto_lib_cleanup();
	}
	if (err)
		g_error_free(err);
	g_free(context);
	unsetenv("G_MESSAGES_DEBUG");
	return(EXIT_SUCCESS);
}
