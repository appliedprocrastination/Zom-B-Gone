/*********************************************************
 * This code uses an Arduino Nano (Uno would also work)
 * It assumes that an Adafruit DS3231 RTC module is connected to the following pins:
 * SDA = A4
 * SCL = A5
 * SQW = D3
 * The code further assumes a rotary encoder connected to the following pins:
 * CLK = A0
 * DT = A1
 * SW = A2
 * It is finally assumed that an ULN2803N is connected to pin D6, and that there is a button connected to pin D2.
 * The button used in the original code includes an LED that is connected to pin D6,
 * and connecting an LED to this pin will indicate whenever the WakeUpLight is in manual override mode.
 * ******************************************************/

#include <Arduino.h>
#include <ClickEncoder.h>
#include <TimerOne.h>
#include <RTClib.h>
#include <Wire.h>

volatile boolean MANUAL_OVERRIDE, OVERRIDE_SENSE_FLAG;

static const uint8_t ENC_CLK_PIN = A0;
static const uint8_t ENC_DT_PIN = A1;
static const uint8_t ENC_SW_PIN = A2;

static const uint8_t OVERRIDE_SENSE_PIN = 2;
static const uint8_t CLOCK_INTERRUPT_PIN = 3;
static const uint8_t BUTTON_LED_PIN = 5;
static const uint8_t LED_ENABLE_PIN = 6;

static const int BUTTON_LED_PWM_VALUE = 255; //TODO: Set this value lower if the button is too bright.
static const int MAX_ENCODER_VALUE = 50;   //This value is mapped down to 255 before being used. TODO: Tune this value in case the encoder feels too slow.

static const unsigned long IRQ_TIMEOUT = 500; //Timeout that applies universally to all button based ISRs.

unsigned long interrupt_timeout = 0; //Timekeeping-variable to handle the IRQ_TIMEOUT

ClickEncoder *encoder;
int16_t last_enc_value, enc_value, manual_pwm_value, last_manual_pwm_value;

RTC_DS3231 rtc;
RTC_Millis rtc_millis;

struct AlarmSettings
{
  // The days that the WakeUpLight shall start
  // Setting a value to false disables the alarm on the corresponding day of the week.
  boolean ALARM_DAYS[7] = {false,  //Sunday
                           true,  //Monday
                           true,  //Tuesday
                           true,  //Wednesday
                           true,  //Thursday
                           true,  //Friday
                           false}; //Saturday
  // The time of the day when the dimming shall start increasing (24 hour clock)
  uint8_t START_HOUR = 5;
  uint8_t START_MINUTE = 30;
  uint8_t START_SECOND = 0;
  // The time of the day when the dimming shall start decreasing (24 hour clock)
  uint8_t END_HOUR = 6;
  uint8_t END_MINUTE = 30;
  uint8_t END_SECOND = 0;
  // The duration of the dimming process (both from start time to 100% and from end time to 0%)
  uint8_t DURATION_HOURS = 0;
  uint8_t DURATION_MINUTES = 30;
  uint8_t DURATION_SECONDS = 0;
  //Dimming settings
  uint8_t MIN_PWM = 0;
  uint8_t MAX_PWM = 255;
};

const struct AlarmSettings ALARM_SETTINGS;
int auto_pwm_value, pwm_dimming_delta;
DateTime timestamp_prev_pwm_change;
TimeSpan s_between_pwm_increments;
boolean ALARM1_FLAG, ALARM2_FLAG, dimming_up, dimming_down;

void timerIsr()
{
  //This ISR is used to call the rotary encoder service routine every 1ms
  encoder->service();
}

void onAlarmIsr()
{
  //This typical content of this ISR is basically covered by the RTCLib library.
}

void overrideSenseIsr(){
  // The Override Sense button is for forcing the light ON as a normal lamp. 
  // When the override sense is ON, the LED Enable pin must be set high (to turn on light in button)
  // When the override sense is ON, the rotary encoder must be programmed as a dimmer so it can tune the 
  // brightness of the light.
  if (millis() >= interrupt_timeout)
  {
    MANUAL_OVERRIDE = !MANUAL_OVERRIDE;
    OVERRIDE_SENSE_FLAG = true;
    interrupt_timeout = millis() + IRQ_TIMEOUT;
  }
}

void acquireRotaryEncoderPos(void){
  enc_value += encoder->getValue();

  if (enc_value != last_enc_value)
  {
    last_enc_value = enc_value;
    Serial.print("Encoder Value: ");
    Serial.println(enc_value);
  }

  ClickEncoder::Button b = encoder->getButton();
  if (b != ClickEncoder::Open)
  {
    Serial.print("Button: ");
#define VERBOSECASE(label)  \
  case label:               \
    Serial.println(#label); \
    break;
    switch (b)
    {
      VERBOSECASE(ClickEncoder::Pressed);
      VERBOSECASE(ClickEncoder::Held)
      VERBOSECASE(ClickEncoder::Released)
      VERBOSECASE(ClickEncoder::Clicked)
    case ClickEncoder::DoubleClicked:
      Serial.println("ClickEncoder::DoubleClicked");
      encoder->setAccelerationEnabled(!encoder->getAccelerationEnabled());
      Serial.print("  Acceleration is ");
      Serial.println((encoder->getAccelerationEnabled()) ? "enabled" : "disabled");
      break;
    default:
      break;
    }
  }
}

void initRotaryEncoder(){
  Timer1.initialize(1000); // The timer will call its ISR every 1000 us (1ms)
  Timer1.attachInterrupt(timerIsr);

  last_enc_value = -1;
}

void setNextWakeUpLightAlarm(int alarm_num){
  TimeSpan oneDay = TimeSpan(1, 0, 0, 0);
  DateTime t = rtc.now();
  DateTime newAlarmDay = t + oneDay;

  int daysUntilNextAlarm = 1;
  while (daysUntilNextAlarm < 8) //The alarm system is based on weekdays, so if there are no alarms witin the first week there will never be any alarms
  {
    uint8_t day_idx = newAlarmDay.dayOfTheWeek();
    if (ALARM_SETTINGS.ALARM_DAYS[day_idx])
    {
      // The wakeup light is supposed to start on the day with index day_idx
      if (alarm_num == 1)
      {
        // A "start"-type alarm
        DateTime alarmTime = DateTime(newAlarmDay.year(), newAlarmDay.month(), newAlarmDay.day(), ALARM_SETTINGS.START_HOUR, ALARM_SETTINGS.START_MINUTE, ALARM_SETTINGS.START_SECOND);
        if (!rtc.setAlarm1(alarmTime,DS3231_A1_Day // this mode triggers the alarm when the day (day of week), hours, minutes and seconds match. See Doxygen for other options
                ))
        {
          Serial.println("Error, alarm wasn't set!");
        }
        else
        {
          Serial.print("A START alarm will happen at ");
          char buf[] = "hh:mm:ss, DDD MMM DD. YYYY";
          alarmTime.toString(buf);
          Serial.println(buf);
        }

      }else if (alarm_num == 2)
      {
        // An "end"-type alarm
        DateTime alarmTime = DateTime(newAlarmDay.year(), newAlarmDay.month(), newAlarmDay.day(), ALARM_SETTINGS.END_HOUR, ALARM_SETTINGS.END_MINUTE, ALARM_SETTINGS.END_SECOND);
        if (!rtc.setAlarm2(alarmTime,DS3231_A2_Day))
        {
          Serial.println("Error, alarm wasn't set!");
        }
        else
        {
          char buf[] = "hh:mm:ss, DDD MMM DD. YYYY";
          alarmTime.toString(buf);
          Serial.print("An END alarm will happen at ");
          Serial.println(buf);
        }
      }else
      {
        //Invalid alarm number
        Serial.println("ERROR: Invalid alarm number to function 'setNextWakeUpLightAlarm(int alarm_num)'. Valid values for alarm_num are 1 and 2");
      }
      break;
    }else
    {
      //Increment the tested variables
      newAlarmDay = newAlarmDay + oneDay;
      daysUntilNextAlarm++;
    }    
  }
  
}

void initRTC(){
  // initializing the rtc
  if (!rtc.begin())
  {
    Serial.println("Couldn't find RTC!");
    Serial.flush();
    abort();
  }


  if (rtc.lostPower())
  {
    // this will adjust to the date and time at compilation
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    // Uncomment next line to manually set an explicit time instead (June 2nd 2021, 20:55:20 PM)
    rtc.adjust(DateTime(2021, 06, 02, 20, 55, 20));
  }
  rtc_millis.begin(rtc.now());

  //we don't need the 32K Pin, so disable it
  rtc.disable32K();

  // Making it so, that the alarm will trigger an interrupt
  pinMode(CLOCK_INTERRUPT_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(CLOCK_INTERRUPT_PIN), onAlarmIsr, FALLING);

  // set alarm 1, 2 flag to false (so alarm 1, 2 didn't happen so far)
  // if not done, this easily leads to problems, as both register aren't reset on reboot/recompile
  rtc.clearAlarm(1);
  rtc.clearAlarm(2);

  // stop oscillating signals at SQW Pin
  // otherwise setAlarm1 will fail
  rtc.writeSqwPinMode(DS3231_OFF);
  char buf1[] = "RTC Time: DD MM YYYY-hh:mm:ss";
  Serial.println(rtc.now().toString(buf1));
  setNextWakeUpLightAlarm(1);
  setNextWakeUpLightAlarm(2);
  
  // schedule an alarm 10 seconds in the future to verify that things are working as expected
/*   if (!rtc.setAlarm1(
          rtc.now() + TimeSpan(10), 
          DS3231_A1_Day // this mode triggers the alarm when the day (day of week), hours, minutes and seconds match. See Doxygen for other options
          ))
  {
    Serial.println("Error, alarm wasn't set!");
  }
  else
  {
    Serial.println("Alarm 1 will happen in 10 seconds!");
    char buf2[] = "RTC Time in 10: DD MM YYYY-hh:mm:ss";
    Serial.println((rtc.now() + TimeSpan(10)).toString(buf2));
  }

  if (!rtc.setAlarm2(
          rtc.now() + TimeSpan(60),
          DS3231_A2_Day))
  {
    Serial.println("Error, alarm wasn't set!");
  }
  else
  {
    Serial.println("Alarm 2 will happen in 60 seconds!");
    char buf3[] = "RTC Time in 60: DD MM YYYY-hh:mm:ss";
    Serial.println((rtc.now() + TimeSpan(60)).toString(buf3));
  } */
  
}

void setup()
{
  Serial.begin(9600);
  // Set up encoder
  encoder = new ClickEncoder(ENC_DT_PIN, ENC_CLK_PIN, ENC_SW_PIN, 2);
  initRotaryEncoder();
  Timer1.stop(); //Stop the rotary encoder readings until the manual override mode is activated in order to save power
  // Set up RTC
  initRTC();
  // Set up inputs
  pinMode(OVERRIDE_SENSE_PIN, INPUT_PULLUP);
  // Set up interrupt on input
  attachInterrupt(digitalPinToInterrupt(OVERRIDE_SENSE_PIN), overrideSenseIsr, FALLING);
  interrupts();
  // Set up outputs
  pinMode(BUTTON_LED_PIN, OUTPUT);
  pinMode(LED_ENABLE_PIN, OUTPUT);
  analogWrite(BUTTON_LED_PIN, 0);
  analogWrite(LED_ENABLE_PIN, 0);
  //Init variables
  MANUAL_OVERRIDE = false;
  OVERRIDE_SENSE_FLAG = false;
  last_enc_value = 0;
  enc_value = MAX_ENCODER_VALUE;
  last_manual_pwm_value = 0;
  manual_pwm_value = BUTTON_LED_PWM_VALUE;
  auto_pwm_value = ALARM_SETTINGS.MIN_PWM;
  ALARM1_FLAG = false;
  ALARM2_FLAG = false;
  dimming_up = false;
  dimming_down = false;

  //Calculate the delay (in ms) between each increment when dimming:
  int hr_to_min = ALARM_SETTINGS.DURATION_HOURS * 60;
  int min_to_s = (ALARM_SETTINGS.DURATION_MINUTES + hr_to_min) * 60  + ALARM_SETTINGS.DURATION_SECONDS;
  pwm_dimming_delta = constrain((ALARM_SETTINGS.MAX_PWM - ALARM_SETTINGS.MIN_PWM) / min_to_s, 1, 255);
  //Dropping ms level resolution to avoid having to deal with overflow from millis() function.
  //int s_to_ms = (ALARM_SETTINGS.DURATION_SECONDS + min_to_s ) * 1000;
  //ms_between_pwm_increments = s_to_ms / (ALARM_SETTINGS.MAX_PWM - ALARM_SETTINGS.MIN_PWM);
  s_between_pwm_increments = TimeSpan(max(min_to_s / (ALARM_SETTINGS.MAX_PWM - ALARM_SETTINGS.MIN_PWM),1));
  timestamp_prev_pwm_change = rtc_millis.now();
}

void loop()
{
  

  if (!MANUAL_OVERRIDE)
  {
    //Normal operations. Wait for RTC alarms to occur.
    if(OVERRIDE_SENSE_FLAG){
      //The override button has been pushed to turn OFF the override functionality (seems backwards because OVERRIDE_SENSE_FLAG and MANUAL OVERRIDE are chaged in the same interrupt handler)
      Serial.println("Override button released, light OFF! (Alarms are active)");
      Timer1.stop(); //Stop the rotary encoder readings to save power
      OVERRIDE_SENSE_FLAG = false;
      analogWrite(BUTTON_LED_PIN, 0);
      Serial.print("Writing PWM ");
      Serial.print(0);
      Serial.println(" to LED driver.");
      analogWrite(LED_ENABLE_PIN, 0);


    }
    if (rtc.alarmFired(1))
    {
      Serial.println("Alarm 1 has gone off. Dimming UP!\n");
      //reset flag
      rtc.clearAlarm(1);
      //Start dimming up
      dimming_up = true;
      //Set time of next alarm
      setNextWakeUpLightAlarm(1);
    }
    else if (rtc.alarmFired(2))
    {
      Serial.println("Alarm 2 has gone off. Dimming DOWN!\n");
      //reset flag
      rtc.clearAlarm(2);
      //Start dimming down (NOTE: If this alarm occurs before the dimming-up phase is done, the dimming-up phase will complete before dimming down begins!)
      dimming_down = true;
      //Set time of next alarm
      setNextWakeUpLightAlarm(2);
    }

    if (dimming_up || dimming_down)
    {
      DateTime timenow = rtc_millis.now();
      if (timenow > timestamp_prev_pwm_change + s_between_pwm_increments)
      {
        if (dimming_up)
        {
          auto_pwm_value = constrain(auto_pwm_value + pwm_dimming_delta, ALARM_SETTINGS.MIN_PWM, ALARM_SETTINGS.MAX_PWM);
          Serial.print("Auto-dimming PWM ");
          Serial.print(auto_pwm_value);
          Serial.println(" to LED driver.");
          if (auto_pwm_value == ALARM_SETTINGS.MAX_PWM)
          {
            dimming_up = false;
            Serial.println("Done dimming up.");
          }
          analogWrite(LED_ENABLE_PIN,auto_pwm_value);
        }
        else if (dimming_down){
          auto_pwm_value = constrain(auto_pwm_value - pwm_dimming_delta, ALARM_SETTINGS.MIN_PWM, ALARM_SETTINGS.MAX_PWM);
          Serial.print("Auto-dimming PWM ");
          Serial.print(auto_pwm_value);
          Serial.println(" to LED driver.");
          if (auto_pwm_value == ALARM_SETTINGS.MIN_PWM)
          {
            Serial.println("Done dimming down.");
            dimming_down = false;
          }
          analogWrite(LED_ENABLE_PIN, auto_pwm_value);
        }
        timestamp_prev_pwm_change = timenow;
      }
      
    }
    
    
    
  }
  else
  {
    //Manual override is initiated. The WakeUpLight functions as a normal lamp (ON) and the rotary encoder functions as a dimmer.
    if(OVERRIDE_SENSE_FLAG){
      //The override button is pushed. Activate the LED and use the rotary encoder as a PWM dimmer. (seems backwards because OVERRIDE_SENSE_FLAG and MANUAL OVERRIDE are chaged in the same interrupt handler)
      last_enc_value = 0;
      last_manual_pwm_value = 0;
      
      Serial.println("Overriding alarms, light ON!");
      Timer1.start();
      analogWrite(BUTTON_LED_PIN,BUTTON_LED_PWM_VALUE);
      OVERRIDE_SENSE_FLAG = false;
    }
    acquireRotaryEncoderPos();
    //"enc_value" is altered with a delta value: "enc_value += encoder->getValue();".
    enc_value = constrain(enc_value, 0, MAX_ENCODER_VALUE);
    //map(value, fromLow, fromHigh, toLow, toHigh)
    manual_pwm_value = map(enc_value, 0, MAX_ENCODER_VALUE, 6, 255); //Always leave some light on when in "override mode" so that the user doesn't forget to turn it off. Forgetting to switch off override mode would leave alarms inactive.
    if (manual_pwm_value != last_manual_pwm_value){
      Serial.print("Writing PWM ");
      Serial.print(manual_pwm_value);
      Serial.println(" to LED driver.");
      analogWrite(LED_ENABLE_PIN,manual_pwm_value);
      last_manual_pwm_value = manual_pwm_value;
    }
  }
  
  
}
