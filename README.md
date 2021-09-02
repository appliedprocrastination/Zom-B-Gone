# Zom-B-Gone

Code for a very basic DIY Wake-up Light. 

This code uses an Arduino Nano (Uno would also work)
It assumes that an Adafruit DS3231 RTC module is connected to the following pins:
 * SDA = A4
 * SCL = A5
 * SQW = D3

The code further assumes a rotary encoder connected to the following pins:
 * CLK = A0
 * DT = A1
 * SW = A2

It is finally assumed that an ULN2803N is connected to pin D6, and that there is a button connected to pin D2.
The button used in the original code includes an LED that is connected to pin D6,
and connecting an LED to this pin will indicate whenever the WakeUpLight is in manual override mode.

To change the alarm settings, modify the `AlarmSettings` struct with your preffered values. Current behavior of the wake-up light is to dim up for 30 minutes towards the START alarm and then down for 30 minutes towards the END alarm. This is inteded to be synced with an external alarm clock (eg. cell phone alarm).

Before first use: set the correct time on the RTC module and load it with a coin cell battery to retain this value through power cycles.

by:
[Applied Procrastination](https://www.youtube.com/AppliedProcrastination)
