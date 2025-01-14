// Code for all one complete sensor node
#include <Particle.h>
#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <BH1750.h>	
#include <Adafruit_BME280.h>
#include <SparkFun_SCD30_Arduino_Library.h>
#include <Adafruit_PM25AQI.h>
#include <Adafruit_VEML6070.h>


SYSTEM_MODE(SEMI_AUTOMATIC);
SYSTEM_THREAD (ENABLED);

BH1750 bh;
#define SEALEVELPRESSURE_HPA (1013.25)
#define BME_ADDRESS 0x77
Adafruit_BME280 bme;
SCD30 airSensor;
Adafruit_PM25AQI aqi = Adafruit_PM25AQI();
Adafruit_VEML6070 uv = Adafruit_VEML6070();
#define COMMAND_GET_VALUE 0x05
#define COMMAND_NOTHING_NEW 0x99
const byte qwiicAddress = 0x30;
#define ONE_DAY_MILLIS (24 * 60 * 60 * 1000)
uint16_t ADC_VALUE = 0;
float dBnumber = 0.0;
unsigned long sensorInterval = 30000;
time_t timeNow;

void initializeSensors();
JSONBufferWriter getSensorReadings(JSONBufferWriter writerJson);
void qwiicTestForConnectivity();
void qwiicGetValue();
void goSleep();
void syncClock();

// setup() runs once, when the device is first turned on.
void setup() {
	Particle.connect();
	pinMode(D7,OUTPUT);
	Wire.begin();
	Serial.begin();
	initializeSensors();

	// Cloud sync initialization
	waitUntil(Particle.connected);
	Particle.setDisconnectOptions(CloudDisconnectOptions().graceful(true).timeout(5s));
	syncClock();

	// Turn off connectivity
	Particle.disconnect();
	waitUntil(Particle.disconnected);
	WiFi.off();
	delay(5s);
}

void loop() {
	// Start of JSON string
	char *dataJson = (char *) malloc(1100);
	JSONBufferWriter writerJson(dataJson, 1099);
	writerJson.beginObject();
	writerJson.name("deviceID").value(System.deviceID());
	for (int collateCount = 0; collateCount < 3; collateCount++){
		// Sleep between sensor readings
		if (collateCount != 0) goSleep();

		// Collate readings into 1 JSON string
		digitalWrite(D7,HIGH);
		timeNow = Time.now();
		writerJson.name(Time.format(timeNow, TIME_FORMAT_ISO8601_FULL)).beginObject();
		writerJson = getSensorReadings(writerJson);
		writerJson.endObject();
		digitalWrite(D7,LOW);
		Serial.println("Take Reading");
	}

	// End of JSON string
	writerJson.endObject();
	writerJson.buffer()[std::min(writerJson.bufferSize(), writerJson.dataSize())] = 0;

	// Wake up connectivity
	digitalWrite(D7,HIGH);
	WiFi.on();
	Serial.println("wifi on");
	Particle.connect();
	waitUntil(Particle.connected);
	Serial.println("particle connected");

	// Publish collated JSON string
	Serial.println("Collated:");
	Serial.println(writerJson.dataSize());
	Serial.println(dataJson);
	Serial.println("");
	if (!Particle.connected()) Particle.connect();
	waitUntil(Particle.connected);
	Particle.publish("sensor-readings", dataJson);

	// Sync device clock daily
  	syncClock();

	// Shut down connectivity
	Particle.disconnect();
	waitUntil(Particle.disconnected);
	WiFi.off();
	digitalWrite(D7,LOW);

	// Sleep until next sensor reading
	free(dataJson);
	goSleep();
	
}


/* Main program flow above */

void initializeSensors()
{
	while (!bh.begin())
	{
		delay(500);
		Serial.println("Trying to connect BH1750 Lux Sensor");
	}
	bh.set_sensor_mode(BH1750::forced_mode_low_res);

	while (!bme.begin())
	{
		delay(500);
		Serial.println("Trying to connect BME280 PTH Sensor");
	}

	while (!airSensor.begin())
	{
		delay(500);
		Serial.println("Trying to connect SCD30 CO2 Sensor");
	}
	airSensor.setMeasurementInterval(25);
  	airSensor.setAutoSelfCalibration(true);

	aqi.begin_I2C();	// Particulate sensor PM2.5

	qwiicTestForConnectivity();
	Serial.println("Zio Qwiic Loudness Sensor Master Awake");

	uv.begin(VEML6070_1_T);
}

JSONBufferWriter getSensorReadings(JSONBufferWriter writerJson)
{
	/*
	Planned JSON Structure:
	{
		"deviceID": xxxxxxx
		"DateTime1": 
		{
			"Sensor1":
			{
				"Measurement1": Value1
				"Measurement2": Value2
			}
		}
	}
	*/

	// LUX Sensor (BH1750)
	bh.make_forced_measurement();
	writerJson.name("BH1750").beginObject();
	writerJson.name("lux").value(bh.get_light_level());
	writerJson.endObject();

	// CO2 Sensor (SCD30)
	if (airSensor.dataAvailable()) delay(500);
	if (airSensor.dataAvailable()) {
		writerJson.name("SCD30").beginObject();
		writerJson.name("CO2-ppm").value(airSensor.getCO2());
		writerJson.name("TempC").value(airSensor.getTemperature());
		writerJson.name("RH%").value(airSensor.getHumidity());
		writerJson.endObject();
	}
	
	// Particulate Sensor (PMSA003I)
	PM25_AQI_Data data;
	writerJson.name("PMSA003I").beginObject();
	writerJson.name("StdPM1.0").value(data.pm10_standard);
	writerJson.name("StdPM2.5").value(data.pm25_standard);
	writerJson.name("StdPM10").value(data.pm100_standard);
	writerJson.name("EnvPM1.0").value(data.pm10_env);
	writerJson.name("EnvPM2.5").value(data.pm25_env);
	writerJson.name("EnvPM10").value(data.pm100_env);
	writerJson.endObject();

	// Peak Sound Sensor (SPARKFUN SEN-15892)
	qwiicGetValue();
	writerJson.name("qwiic").beginObject();
	writerJson.name("ADC").value(ADC_VALUE);
	writerJson.name("dB").value(dBnumber);
	writerJson.endObject();

	// UV Sensor (VEML 6070)
	writerJson.name("VEML6070").beginObject();
	writerJson.name("UV").value(uv.readUV());
	writerJson.endObject();

	// Pressure, Temperature, Humidity Sensor (BME280)
	writerJson.name("BME280").beginObject();
	writerJson.name("P-mbar").value(bme.readPressure()/100.0F);
	writerJson.name("RH%").value(bme.readHumidity());
	writerJson.name("TempC").value(bme.readTemperature());
	writerJson.endObject();

	return writerJson;
}


void qwiicGetValue()
{
	Wire.beginTransmission(qwiicAddress);
	Wire.write(COMMAND_GET_VALUE); // command for status
	Wire.endTransmission(); // stop transmitting //this looks like it was essential.
	Wire.requestFrom(qwiicAddress, 2); // request 1 bytes from slave device qwiicAddress

	while (Wire.available())
	{ // slave may send less than requested
		uint8_t ADC_VALUE_L = Wire.read();
		uint8_t ADC_VALUE_H = Wire.read();
		ADC_VALUE=ADC_VALUE_H;
		ADC_VALUE<<=8;
		ADC_VALUE|=ADC_VALUE_L;
		dBnumber = (ADC_VALUE+83.2073) / 11.003; //emprical formula to convert ADC value to dB
	}
	return;
}

// qwiicTestForConnectivity() checks for an ACK from an Sensor. If no ACK
// program freezes and notifies user.
void qwiicTestForConnectivity()
{
	Wire.beginTransmission(qwiicAddress);
	//check here for an ACK from the slave, if no ACK don't allow change?
	if (Wire.endTransmission() != 0)
	{
		Serial.println("Check connections. No slave attached.");
		while (1);
	}
	return;
}

void goSleep()
{
	// SystemSleepConfiguration sleepConfig;
	// sleepConfig.mode(SystemSleepMode::ULTRA_LOW_POWER).duration(1min);
	// System.sleep(sleepConfig);
	delay(sensorInterval);
	return;
}

void syncClock()
{
	unsigned long lastSync = Particle.timeSyncedLast();
	if (millis() - lastSync > ONE_DAY_MILLIS){
		Particle.syncTime();
		waitUntil(Particle.syncTimeDone);
	}
	return;
}