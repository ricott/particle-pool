// Blynk token kept in file listed in .gitignore
#include "BLYNK_SETTINGS.h"

#include <DS18B20.h>
#include <math.h>
#include <blynk.h>
#include <RelayShield.h>

char authToken[] = BLYNK_AUTH_TOKEN;

STARTUP(WiFi.selectAntenna(ANT_EXTERNAL));

const int BLYNK_TERMINAL = V0;
const int relay_pump = 1;             // Black
const int relay_circulation_pool = 2; // Brown
// Simulate the heater flow protection, proper one is broken :(
const int relay_heater_flow_protection = 3;  // Yellow
const int relay_circulation_heater_only = 4; // Red
const int MAXRETRY = 4;
const String event_prefix = "garden/pool/";
// RMS voltage
const double vRMS = 234.0; // Assumed or measured
// Parameters for measuring RMS current
const double offset = 1.65;  // Half the ADC max voltage
const int numTurns = 2000;   // 1:2000 transformer turns
const int rBurden = 100;     // Burden resistor value
const int numSamples = 1000; // Number of samples before calculating RMS

DS18B20 ds18b20_water(D1, true); // White
DS18B20 ds18b20_out(D7, true);   // Grey
DS18B20 ds18b20_in(D2, true);    // Green

char szInfo[64];
double water_out_temp;
double water_in_temp;
double water_temp;
int desiredWaterTemp;
double efficiency;
double amp;
double watt;
double desiredTempOffset = 1.0;
long startPumpSecond;
long stopPumpSecond;
bool heaterValveOpen = false;
bool isFirstConnect = true;
bool valveMaxOpenMsg = false;
bool heatingDisabled = false;
int valve_max_seconds = 32;
bool valve_moving = false;
int hv_moved_seconds = 0;
bool heaterOnNotification = false;
RelayShield myRelays;
BlynkTimer blynkTimer;
WidgetTerminal terminal(BLYNK_TERMINAL);
WidgetLED pump_led(V6);
WidgetLED heater_led(V7);

BLYNK_CONNECTED()
{
    Blynk.syncVirtual(V8, V9);
}

BLYNK_WRITE(V8)
{
    TimeInputParam t(param);
    long start = (t.getStartHour() * 3600) + (t.getStartMinute() * 60);
    long stop = (t.getStopHour() * 3600) + (t.getStopMinute() * 60);
    if (startPumpSecond == start && stopPumpSecond == stop)
    {
        // No change, just a reconnect event
        return;
    }

    terminal.println(Time.format(Time.now(), "%F %R ") +
                     "New pump schedule: " + padZeros(t.getStartHour()) + ":" + padZeros(t.getStartMinute()) +
                     "-" + padZeros(t.getStopHour()) + ":" + padZeros(t.getStopMinute()));
    terminal.flush();
    startPumpSecond = start;
    stopPumpSecond = stop;
}

char *padZeros(int val)
{
    char *buf = (char *)malloc(2);
    sprintf(buf, "%02d", val);
    return buf;
}

BLYNK_WRITE(V9)
{
    if (desiredWaterTemp == param.asInt())
    {
        // No change, just a reconnect event
        return;
    }

    desiredWaterTemp = param.asInt();
    if (desiredWaterTemp == 20)
    {
        terminal.println(Time.format(Time.now(), "%F %R ") + "Heater disabled!");
        terminal.flush();
        heatingDisabled = true;
    }
    else
    {
        terminal.println(Time.format(Time.now(), "%F %R ") + "Heater target temp: " + String(desiredWaterTemp));
        terminal.flush();
        heatingDisabled = false;
    }
}

void setup()
{
    Particle.publish(event_prefix + "setup", "starting", PRIVATE);
    Time.zone(+2);
    Particle.variable("water-out", water_out_temp);
    Particle.variable("water-in", water_in_temp);
    Particle.variable("water", water_temp);
    Particle.variable("watt", watt);
    Particle.variable("desWaterTmp", desiredWaterTemp);

    // ensure on power on the relay is in a predictable state
    myRelays.begin();
    myRelays.allOff();

    Blynk.begin(authToken);

    // Makse sure valve is in known position
    // force pool only circulation
    closeHeaterValve(true);

    // Schedule automatic on/off of pump
    blynkTimer.setInterval(60000L, operatePumpSchedule);
    blynkTimer.setInterval(120000L, operateHeaterValve);
    // Publish data to blynk
    blynkTimer.setInterval(30000L, publishData);
    // Read sensor values
    blynkTimer.setInterval(10000L, readSensors);
}

void operateHeaterValve()
{
    if (isHeaterSupposedToBeOn())
    {
        // Lets heat that water
        openHeaterValve();
    }
    else
    {
        closeHeaterValve(false);
    }
}

void openHeaterValve()
{
    if (!isHeaterValveOpen())
    {
        heaterValveOpen = true;
        sprintf(szInfo, "%2.2f", water_temp);
        terminal.println(Time.format(Time.now(), "%F %R ") + "Water->Heater, temp: " + String(szInfo));
        terminal.flush();

        if (!isValveMoving())
        {
            turnOnRelay("", relay_circulation_heater_only);
            blynkTimer.setTimer(60000L, closeHVRelay, 1);

            valve_moving = true;
        }
        else
        {
            terminal.println(Time.format(Time.now(), "%F %R ") + "Valve is already moving!");
            terminal.flush();
        }
    }
}
void closeHVRelay()
{
    turnOffRelay("", relay_circulation_heater_only);
    valve_moving = false;
    // Water is flowing through the heater, the valve is fully open
    // lets turn it on
    turnOnRelay("", relay_heater_flow_protection);
}

void closeHeaterValve(bool force)
{
    if (isHeaterValveOpen() || force)
    {
        heaterValveOpen = false;
        valveMaxOpenMsg = false;
        heaterOnNotification = false;
        hv_moved_seconds = 0;
        if (!force)
        {
            sprintf(szInfo, "%2.2f", water_temp);
            terminal.println(Time.format(Time.now(), "%F %R ") + "Water->Pool, temp: " + String(szInfo));
            terminal.flush();
        }

        // Water is not flowing through the heater, below we are closing the valve
        turnOffRelay("", relay_heater_flow_protection);

        if (!isValveMoving())
        {
            turnOnRelay("", relay_circulation_pool);
            blynkTimer.setTimer(60000L, closePVRelay, 1);
            valve_moving = true;
        }
        else
        {
            terminal.println(Time.format(Time.now(), "%F %R ") + "Valve is already moving!");
            terminal.flush();
        }
    }
}
void closePVRelay()
{
    turnOffRelay("", relay_circulation_pool);
    valve_moving = false;
}

bool isHeaterValveOpen()
{
    return heaterValveOpen;
}
bool isHeatingDisabled()
{
    return heatingDisabled;
}
bool isValveMoving()
{
    return valve_moving;
}

bool isHeaterSupposedToBeOn()
{
    // Pump needs to be running, and water temp lower than desired - offset
    // and heating not disabled
    if (isPumpOn() && !isHeatingDisabled())
    {
        // If heater is not on, only start after temp is less than desired - offset
        if (!isHeaterOn() && water_temp < (desiredWaterTemp - desiredTempOffset))
        {
            return true;
            // If heater is on, run it until water reaches desired temp
        }
        else if (isHeaterOn() && water_temp < desiredWaterTemp)
        {
            return true;
        }
        else
        {
            return false;
        }
    }
    else
    {
        return false;
    }
}

void operatePumpSchedule()
{
    long now = (Time.hour() * 3600) + (Time.minute() * 60);

    if (now > startPumpSecond)
    {
        // We are after start time, lets check if we are before end time
        if (now < stopPumpSecond)
        {
            // Pump should be running, if not start it
            if (!isPumpOn())
            {
                Particle.publish(event_prefix + "schedule", "Start pump by schedule", PRIVATE);
                pumpStartBySchedule();
            }
        }
        else
        {
            // We are past end time, stop pump
            if (isPumpOn())
            {
                Particle.publish(event_prefix + "schedule", "Stop pump by schedule", PRIVATE);
                pumpStopBySchedule();
                closeHeaterValve(false);
            }
        }
    }
    else
    {
        // We are before start time, stop it if running
        if (isPumpOn())
        {
            Particle.publish(event_prefix + "schedule", "Stop pump by schedule", PRIVATE);
            pumpStopBySchedule();
            closeHeaterValve(false);
        }
    }
}

void loop()
{
    Blynk.run();
    blynkTimer.run();
}

void readSensors()
{
    getWaterInTemp();
    getWaterOutTemp();
    getWaterTemp();
    readCurrent();
    toggleBlynkLeds();
}

void publishData()
{
    Blynk.virtualWrite(V1, water_temp);
    Blynk.virtualWrite(V2, water_out_temp);
    Blynk.virtualWrite(V3, water_in_temp);
    Blynk.virtualWrite(V4, watt);
    calculateEfficiency();
    Blynk.virtualWrite(V5, efficiency);
}

void calculateEfficiency()
{
    // Only relevant to show a value if heater is on
    if (isHeaterOn())
    {
        double temp_diff = water_in_temp - water_out_temp;
        efficiency = (temp_diff / watt) * 1000;
        if (efficiency < 0.0)
        {
            efficiency = 0.0;
        }
    }
    else
    {
        efficiency = 0.0;
    }
}

void toggleBlynkLeds()
{
    if (isHeaterOn())
    {
        heater_led.on();
    }
    else
    {
        heater_led.off();
    }

    if (isPumpOn())
    {
        pump_led.on();
    }
    else
    {
        pump_led.off();
    }
}

void pumpStartBySchedule()
{
    terminal.println(Time.format(Time.now(), "%F %R Pump started by schedule"));
    terminal.flush();
    startPump();
}
void pumpStopBySchedule()
{
    terminal.println(Time.format(Time.now(), "%F %R Pump stopped by schedule"));
    terminal.flush();
    stopPump();
}

void startPump()
{
    turnOnRelay("Pump", relay_pump);
}
void stopPump()
{
    turnOffRelay("Pump", relay_pump);
}
bool isPumpOn()
{
    return isRelayOn(relay_pump);
}

bool isHeaterOn()
{
    if (watt > 100.0)
    {
        return true;
    }
    else
    {
        return false;
    }
}

bool isHeaterStartingStopping()
{
    if (watt > 10.0)
    {
        return true;
    }
    else
    {
        return false;
    }
}

bool isRelayOn(int relay)
{
    if (myRelays.isOn(relay))
    {
        return true;
    }
    else
    {
        return false;
    }
}
void turnOnRelay(String relayName, int relay)
{
    if (relayName != "")
    {
        Particle.publish(event_prefix + relayName, "on", PRIVATE);
        terminal.println(Time.format(Time.now(), "%F %R " + relayName + " on"));
        terminal.flush();
    }
    myRelays.on(relay);
}

void turnOffRelay(String relayName, int relay)
{
    if (relayName != "")
    {
        Particle.publish(event_prefix + relayName, "off", PRIVATE);
        terminal.println(Time.format(Time.now(), "%F %R " + relayName + " off"));
        terminal.flush();
    }
    myRelays.off(relay);
}

void getWaterTemp()
{
    float _temp;
    int i = 0;

    do
    {
        _temp = ds18b20_water.getTemperature();
    } while (!ds18b20_water.crcCheck() && MAXRETRY > i++);

    if (i < MAXRETRY && !isnan(_temp))
    {
        water_temp = _temp;
    }
    else
    {
        water_temp = 0.0;
    }
}

void getWaterInTemp()
{
    float _temp;
    int i = 0;
    do
    {
        _temp = ds18b20_in.getTemperature();
    } while (!ds18b20_in.crcCheck() && MAXRETRY > i++);

    if (i < MAXRETRY)
    {
        water_in_temp = _temp;
    }
    else
    {
        water_in_temp = 0.0;
    }
}

void getWaterOutTemp()
{
    float _temp;
    int i = 0;
    do
    {
        _temp = ds18b20_out.getTemperature();
    } while (!ds18b20_out.crcCheck() && MAXRETRY > i++);

    if (i < MAXRETRY)
    {
        water_out_temp = _temp;
    }
    else
    {
        water_out_temp = 0.0;
    }
}

void readCurrent()
{
    int sample;
    double voltage;
    double iPrimary;
    double acc = 0;

    // Take a number of samples and calculate RMS current
    for (int i = 0; i < numSamples; i++)
    {
        // Read ADC, convert to voltage, remove offset
        sample = analogRead(A0);
        voltage = (sample * 3.3) / 4096;
        voltage = voltage - offset;

        // Calculate the sensed current
        iPrimary = (voltage / rBurden) * numTurns;

        // Square current and add to accumulator
        acc += pow(iPrimary, 2);
    }

    // Calculate RMS from accumulated values
    amp = sqrt(acc / numSamples);
    // Calculate apparent power and publish it
    watt = vRMS * amp;
}
