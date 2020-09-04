//********************************************************
// PROJECT: MAXMIX
// AUTHOR: Ruben Henares
// EMAIL: rhenares0@gmail.com
//
// DECRIPTION:
// 
//
//
// The AVR 328P chip has 2kB of SRAM. 
// Program uses 825 bytes, leaving 1223 bytes for items. Each item is 42 bytes.
// Therefore we can store a maximum (1223 / 42 bytes) of 29 items.
// We currently this to 8 maximum items for safety.
//********************************************************


//********************************************************
// *** INCLUDES
//********************************************************
#include <Arduino.h>
#include <avr/wdt.h>

// Custom
#include "Config.h"

// Third-party
#include "src/Adafruit_GFX/Adafruit_GFX.h"
#include "src/Adafruit_NeoPixel/Adafruit_NeoPixel.h"
#include "src/Adafruit_SSD1306/Adafruit_SSD1306.h"
#include "src/ButtonEvents/ButtonEvents.h"
#include "src/Rotary/Rotary.h"
#include "src/TimerOne/TimerOne.h"
#include "src/FixedPoints/FixedPoints.h"
#include "src/FixedPoints/FixedPointsCommon.h"

//********************************************************
// *** STRUCTS
//********************************************************
struct Item
{
  uint32_t id;                          // 4 Bytes (32 bit)
  char name[ITEM_BUFFER_NAME_SIZE];     // 36 Bytes (36 Chars)
  int8_t volume;                        // 1 Byte
  uint8_t isMuted;                      // 1 Byte
                                        // 43 Bytes TOTAL
};

struct Settings
{
  uint8_t displayNewItem = 1;
  uint8_t sleepWhenInactive = 1;
  uint8_t sleepAfterSeconds = 5;
  uint8_t continuousScroll = 1;
  uint8_t accelerationPercentage = 60;
};

//********************************************************
// *** VARIABLES
//*******************************************************
// Serial Communication
uint8_t receiveIndex = 0;
uint8_t sendIndex = 0;
uint8_t messageRevision = 0;
bool waitingAck = false;
uint32_t ackTimer = millis();
uint8_t receiveBuffer[RECEIVE_BUFFER_SIZE];
uint8_t decodeBuffer[RECEIVE_BUFFER_SIZE];
uint8_t sendBuffer[SEND_BUFFER_SIZE];
uint8_t encodeBuffer[SEND_BUFFER_SIZE];

// State
uint8_t mode = MODE_OUTPUT;
uint8_t stateOutput = STATE_OUTPUT_NAVIGATE;
uint8_t stateApplication = STATE_APPLICATION_NAVIGATE;
uint8_t stateGame = STATE_GAME_SELECT_A;
uint8_t stateDisplay = STATE_DISPLAY_AWAKE;
uint8_t isDirty = true;

struct Item devices[DEVICE_MAX_COUNT];
struct Item sessions[SESSION_MAX_COUNT];
uint8_t deviceCount = 0;
uint8_t sessionCount = 0;

int8_t itemIndexOutput = 0;
int8_t itemIndexApp = 0;
int8_t itemIndexGameA = 0;
int8_t itemIndexGameB = 0;

// Settings
struct Settings settings;

// Rotary Encoder
ButtonEvents encoderButton;
Rotary encoderRotary(PIN_ENCODER_OUTB, PIN_ENCODER_OUTA);
uint32_t encoderLastTransition = 0;
int8_t prevDir = 0;
volatile int8_t steps = 0;

// Time & Sleep
uint32_t now = 0;
uint32_t last = 0;
uint32_t lastActivityTime = 0;

// Lighting
Adafruit_NeoPixel* pixels;

// Display
Adafruit_SSD1306* display;

// Watchdog Trigger
void reboot() {
  wdt_disable();
  wdt_enable(WDTO_15MS);
  while(1) {}
}

//********************************************************
// *** INTERRUPTS
//********************************************************
void timerIsr()
{
  uint8_t encoderDir = encoderRotary.process();
  if(encoderDir == DIR_CW)
    steps++;
  else if(encoderDir == DIR_CCW)
    steps--;
}

//********************************************************
// *** MAIN
//********************************************************
//---------------------------------------------------------
//---------------------------------------------------------
void setup()
{
  // Clear Watchdog Flags
  MCUSR = 0;
  
  // --- Comms
  Serial.begin(BAUD_RATE);

  //--- Pixels
  pixels = new Adafruit_NeoPixel(PIXELS_NUM, PIN_PIXELS, NEO_GRB + NEO_KHZ800);
  pixels->begin();

  // --- Display
  display = InitializeDisplay();
  DisplaySplashScreen(display);

  // --- Encoder
  pinMode(PIN_ENCODER_SWITCH, INPUT_PULLUP);
  encoderButton.attach(PIN_ENCODER_SWITCH);
  encoderRotary.begin(true);
  Timer1.initialize(1000);
  Timer1.attachInterrupt(timerIsr);
}

//---------------------------------------------------------
//---------------------------------------------------------
void loop()
{
  last = now;
  now = millis();

  if(ReceiveData(receiveBuffer, &receiveIndex, MSG_PACKET_DELIMITER, RECEIVE_BUFFER_SIZE))
  {
    if(DecodePackage(receiveBuffer, receiveIndex, decodeBuffer))
    {
      // Immediately send ACK
      uint8_t command = GetCommandFromPackage(decodeBuffer);
      if(command != MSG_COMMAND_ACKNOWLEDGMENT)
      {
        uint8_t revision = GetRevisionFromPackage(decodeBuffer);
        SendAcknowledgment(sendBuffer, encodeBuffer, revision);
      }

      if(ProcessPackage())
      {
        UpdateActivityTime();
        isDirty = true;
      }
    }      
    ClearReceive();
  }

  // Trigger WatchDog if ack timed-out
  if(waitingAck)
  {
    if(now - ackTimer > ACK_TIMEOUT)
      reboot();
  }

  if(ProcessEncoderRotation() || ProcessEncoderButton())
  {
      UpdateActivityTime();
      isDirty = true;
  }

  if(ProcessSleep())
    isDirty = true;

  // Check for buffer overflow
  if(receiveIndex == RECEIVE_BUFFER_SIZE)
    ClearReceive();

  ClearSend();
  encoderButton.update();


  if(isDirty || ProcessDisplayScroll())
    UpdateDisplay();

  if(isDirty)
    UpdateLighting();  

  TimerDisplayUpdate(now - last);
  isDirty = false;
}

//********************************************************
// *** FUNCTIONS
//********************************************************
//---------------------------------------------------------
//---------------------------------------------------------
void ClearReceive()
{
  receiveIndex = 0;  
}

//---------------------------------------------------------
//---------------------------------------------------------
void ClearSend()
{
  sendIndex = 0;
}

//---------------------------------------------------------
//---------------------------------------------------------
void ResetState()
{
  mode = MODE_OUTPUT;
  stateOutput = STATE_OUTPUT_NAVIGATE;
  stateApplication = STATE_APPLICATION_NAVIGATE;
  stateGame = STATE_GAME_SELECT_A;
  stateDisplay = STATE_DISPLAY_AWAKE;

  itemIndexOutput = 0;
  itemIndexApp = 0;
  itemIndexGameA = 0;
  itemIndexGameB = 0;
  sessionCount = 0;
  deviceCount = 0;
}


//---------------------------------------------------------
// \brief Handles incoming commands.
// \returns true if screen update is required.
//---------------------------------------------------------
bool ProcessPackage()
{
  uint8_t command = GetCommandFromPackage(decodeBuffer);
  
  if(command == MSG_COMMAND_ACKNOWLEDGMENT)
  {
    if (ProcessAcknowledgment(decodeBuffer, messageRevision))
      waitingAck = false;
    return false;
  }
  else if(command == MSG_COMMAND_HANDSHAKE_REQUEST)
  {
    ResetState();
    return true;
  }
  else if(command == MSG_COMMAND_ADD)
  {
    uint32_t id = GetIdFromPackage(decodeBuffer);
    bool isDevice = GetIsDeviceFromAddPackage(decodeBuffer);    

    int8_t index;  
    if(isDevice)
    {
      if(deviceCount == DEVICE_MAX_COUNT)
        return false;

      index = FindItem(id, devices, deviceCount);
      if(index == -1)
      {
        AddItemCommand(decodeBuffer, devices, &deviceCount);
        index = deviceCount - 1;   
      }
      else
        UpdateItemCommand(decodeBuffer, devices, index);

      if(settings.displayNewItem)
      {
        itemIndexOutput = index;
        if(mode == MODE_OUTPUT)
          stateOutput = STATE_OUTPUT_NAVIGATE;

        return true;
      }
    }
    else
    {
      if(sessionCount == SESSION_MAX_COUNT)
        return false;

      index = FindItem(id, sessions, sessionCount);
      if(index == -1)
      {
        AddItemCommand(decodeBuffer, sessions, &sessionCount);
        index = sessionCount - 1;
      }
      else
        UpdateItemCommand(decodeBuffer, sessions, index);

      if(settings.displayNewItem)
      {
        itemIndexApp = index;
        if(mode == MODE_APPLICATION)
          stateApplication = STATE_APPLICATION_NAVIGATE;

        return true;
      }
    }    
  }
  else if(command == MSG_COMMAND_REMOVE)
  {
    uint32_t id = GetIdFromPackage(decodeBuffer);
    bool isDevice = GetIsDeviceFromRemovePackage(decodeBuffer);

    int8_t index;
    if(isDevice)
    {
      if(deviceCount == 0)
        return false;

      index = FindItem(id, devices, deviceCount);
      if(index == -1)
        return false;

      RemoveItemCommand(decodeBuffer, devices, &deviceCount, index);
      
      bool isItemActive = IsItemActive(index);
      itemIndexOutput = GetNextIndex(itemIndexOutput, deviceCount, 0, settings.continuousScroll);
      if(isItemActive)
      {
        if(mode == MODE_OUTPUT)
          stateOutput = STATE_OUTPUT_NAVIGATE;
        return true;      
      }
    }
    else
    {
      if(sessionCount == 0)
        return false;

      index = FindItem(id, sessions, sessionCount);
      if(index == -1)
        return false;

      RemoveItemCommand(decodeBuffer, sessions, &sessionCount, index);
      
      bool isItemActive = IsItemActive(index);
      itemIndexApp = GetNextIndex(itemIndexApp, sessionCount, 0, settings.continuousScroll);
      if(isItemActive)
      {
        if(mode == MODE_APPLICATION)
          stateApplication = STATE_APPLICATION_NAVIGATE;
        return true;      
      }    
    }
  }
  else if(command == MSG_COMMAND_UPDATE_VOLUME)
  {
    uint32_t id = GetIdFromPackage(decodeBuffer);
    bool isDevice = GetIsDeviceFromUpdatePackage(decodeBuffer);

    int8_t index;
    if(isDevice)
    {
      index = FindItem(id, devices, deviceCount);
      if(index == -1)
        return false;

      UpdateItemVolumeCommand(decodeBuffer, devices, index);

      if(IsItemActive(index))
        return true;

      return false;
    }
    else
    {
      index = FindItem(id, sessions, sessionCount);
      if(index == -1)
        return false;

      UpdateItemVolumeCommand(decodeBuffer, sessions, index);

      if(IsItemActive(index))
      {
        if(mode == MODE_GAME)
        {
          if(index == itemIndexGameA && index != itemIndexGameB)
            RebalanceGameVolume(sessions[itemIndexGameA].volume, itemIndexGameB);

          if(index == itemIndexGameB && index != itemIndexGameA)
            RebalanceGameVolume(sessions[itemIndexGameB].volume, itemIndexGameA);
        }                
        return true;
      }
      return false;
    }    
  }
  else if(command == MSG_COMMAND_SETTINGS)
  {
    UpdateSettingsCommand(decodeBuffer, &settings);
    stateDisplay = STATE_DISPLAY_AWAKE;
    return true;
  }

  return false;
} 


//---------------------------------------------------------
// \brief Encoder acceleration algorithm (Exponential - speed squared)
// \param encoderDelta - step difference since last check
// \param deltaTime - time difference since last check (ms)
// \param volume - curent volume
// \returns new adjusted volume
//---------------------------------------------------------
int8_t ComputeAcceleratedVolume(int8_t encoderDelta, uint32_t deltaTime, int16_t volume)
{
  if (!encoderDelta)
    return volume;

  bool dirChanged = ((prevDir > 0) && (encoderDelta < 0)) || ((prevDir < 0) && (encoderDelta > 0));

  uint32_t step;
  if (dirChanged)
  {
     step = 1;
  }
  else
  {
    // Compute acceleration using fixed point maths.
    SQ15x16 speed = (SQ15x16)encoderDelta*1000/deltaTime;
    SQ15x16 accelerationDivisor = max((1-(SQ15x16)settings.accelerationPercentage/100)*ROTARY_ACCELERATION_DIVISOR_MAX, 1);
    SQ15x16 fstep = 1 + absFixed(speed*speed/accelerationDivisor);
    step = fstep.getInteger();
  }

  prevDir = encoderDelta;

  if(encoderDelta > 0)
  {
    volume += step;
  }
  else if(encoderDelta < 0)
  {
    volume -= step;
  }
  
  return constrain(volume, 0, 100);
}

//---------------------------------------------------------
//---------------------------------------------------------
bool ProcessEncoderRotation()
{
  int8_t encoderDelta;
  cli();
  encoderDelta = steps;
  if(encoderDelta !=0)
    steps = 0;
  sei();
  
  if(encoderDelta == 0)
    return false;

  uint32_t deltaTime = now - encoderLastTransition;
  encoderLastTransition = now;

  if(stateDisplay == STATE_DISPLAY_SLEEP)
    return true;

  if(mode == MODE_OUTPUT)
  {
    if(stateOutput == STATE_OUTPUT_NAVIGATE)
    {
      itemIndexOutput = GetNextIndex(itemIndexOutput, deviceCount, encoderDelta, settings.continuousScroll);
      TimerDisplayReset();
    }

    else if(stateOutput == STATE_OUTPUT_EDIT)
    {
      devices[itemIndexOutput].volume = ComputeAcceleratedVolume(encoderDelta, deltaTime, devices[itemIndexOutput].volume);
      SendItemVolumeCommand(&devices[itemIndexOutput], sendBuffer, encodeBuffer, &waitingAck, &ackTimer, now);
    }
  }
  else if(mode == MODE_APPLICATION)
  {
    if(stateApplication == STATE_APPLICATION_NAVIGATE)
    {
      itemIndexApp = GetNextIndex(itemIndexApp, sessionCount, encoderDelta, settings.continuousScroll);
      TimerDisplayReset();
    }
    else if(stateApplication == STATE_APPLICATION_EDIT)
    {
      sessions[itemIndexApp].volume = ComputeAcceleratedVolume(encoderDelta, deltaTime, sessions[itemIndexApp].volume);
      SendItemVolumeCommand(&sessions[itemIndexApp], sendBuffer, encodeBuffer, &waitingAck, &ackTimer, now);
    }
  }
  else if(mode == MODE_GAME)
  {
    if(stateGame == STATE_GAME_SELECT_A)
    {
      itemIndexGameA = GetNextIndex(itemIndexGameA, sessionCount, encoderDelta, settings.continuousScroll);
      TimerDisplayReset();
    }
    else if(stateGame == STATE_GAME_SELECT_B)
    {
      itemIndexGameB = GetNextIndex(itemIndexGameB, sessionCount, encoderDelta, settings.continuousScroll);
      TimerDisplayReset();
    }

    else if(stateGame == STATE_GAME_EDIT)
    {
      sessions[itemIndexGameA].volume = ComputeAcceleratedVolume(encoderDelta, deltaTime, sessions[itemIndexGameA].volume);
      SendItemVolumeCommand(&sessions[itemIndexGameA], sendBuffer, encodeBuffer, &waitingAck, &ackTimer, now);

      if(itemIndexGameA != itemIndexGameB)
        RebalanceGameVolume(sessions[itemIndexGameA].volume, itemIndexGameB);
    } 
  }
  
  return true;
}

//---------------------------------------------------------
//---------------------------------------------------------
bool ProcessEncoderButton()
{
  if(encoderButton.tapped())
  {
    if(stateDisplay == STATE_DISPLAY_SLEEP)
      return true;

    if(mode == MODE_OUTPUT)
    {
      stateOutput = CycleState(stateOutput, STATE_OUTPUT_COUNT);
      TimerDisplayReset();
    }
    else if(mode == MODE_APPLICATION)
    {
      stateApplication = CycleState(stateApplication, STATE_APPLICATION_COUNT);
      TimerDisplayReset();
    }
    else if(mode == MODE_GAME)
    {
      stateGame = CycleState(stateGame, STATE_GAME_COUNT);
      TimerDisplayReset();
    }

    return true;
  }
  else if(encoderButton.doubleTapped())
  {
    if(mode == MODE_OUTPUT)
      ToggleMute(devices, itemIndexOutput);

    else if(mode == MODE_APPLICATION)
      ToggleMute(sessions, itemIndexApp);

    else if(mode == MODE_GAME && stateGame == STATE_GAME_EDIT)
      ResetGameVolume();

    return true;
  }
  else if(encoderButton.held())
  {
    if(stateDisplay == STATE_DISPLAY_SLEEP)
      return true;
      
    CycleMode();
    TimerDisplayReset();

    return true;
  }

  return false;
}

//---------------------------------------------------------
//---------------------------------------------------------
bool ProcessSleep()
{
  if(settings.sleepWhenInactive == 0)
    return false;

  uint32_t activityTimeDelta = now - lastActivityTime;

  if(stateDisplay == STATE_DISPLAY_AWAKE)
  {
    if(activityTimeDelta > settings.sleepAfterSeconds * 1000)
    {
      stateDisplay = STATE_DISPLAY_SLEEP;
      return true;
    }
  }
  else if(stateDisplay == STATE_DISPLAY_SLEEP)
  {
    if(activityTimeDelta < settings.sleepAfterSeconds * 1000)
    {
      stateDisplay = STATE_DISPLAY_AWAKE;
      return true;
    }
  }

  return false;
}

bool ProcessDisplayScroll()
{
  bool result = false;

  if(mode == MODE_OUTPUT)
    result = strlen(devices[itemIndexOutput].name) > DISPLAY_CHAR_MAX_X2;
  else if(mode == MODE_APPLICATION)
    result = strlen(sessions[itemIndexApp].name) > DISPLAY_CHAR_MAX_X2;
  else if(mode == MODE_GAME)
  {
    if(stateGame == STATE_GAME_SELECT_A)
      result = strlen(sessions[itemIndexGameA].name) > DISPLAY_CHAR_MAX_X2;
    else if(stateGame == STATE_GAME_SELECT_B)
      result = strlen(sessions[itemIndexGameB].name) > DISPLAY_CHAR_MAX_X2;
    else if(stateGame == STATE_GAME_EDIT)
    {
      result = strlen(sessions[itemIndexGameA].name) > DISPLAY_GAME_EDIT_CHAR_MAX ||
               strlen(sessions[itemIndexGameB].name) > DISPLAY_GAME_EDIT_CHAR_MAX;
    }
  }

  return result;
}

//---------------------------------------------------------
//---------------------------------------------------------
void UpdateActivityTime()
{
  lastActivityTime = now;
}

//---------------------------------------------------------
//---------------------------------------------------------
void UpdateDisplay()
{
  if(stateDisplay == STATE_DISPLAY_SLEEP)
  {
    DisplaySleep(display);
    return;
  }

  // TODO: Update to include devices.
  if(sessionCount == 0)
  {
    DisplaySplashScreen(display);
    return;
  }
  
  if(mode == MODE_OUTPUT)
  {
    if(stateOutput == STATE_OUTPUT_NAVIGATE)
    {
      uint8_t scrollLeft = CanScrollLeft(itemIndexOutput, deviceCount, settings.continuousScroll);
      uint8_t scrollRight = CanScrollRight(itemIndexOutput, deviceCount, settings.continuousScroll);
      DisplayOutputSelectScreen(display, devices[itemIndexOutput].name, devices[itemIndexOutput].volume, devices[itemIndexOutput].isMuted, scrollLeft, scrollRight, mode, MODE_COUNT);
    }
    else if(stateOutput == STATE_OUTPUT_EDIT)
      DisplayOutputEditScreen(display, devices[itemIndexOutput].name, devices[itemIndexOutput].volume, devices[itemIndexOutput].isMuted, mode, MODE_COUNT);
  }
  else if(mode == MODE_APPLICATION)
  {
    if(stateApplication == STATE_APPLICATION_NAVIGATE)
    {
      uint8_t scrollLeft = CanScrollLeft(itemIndexApp, sessionCount, settings.continuousScroll);
      uint8_t scrollRight = CanScrollRight(itemIndexApp, sessionCount, settings.continuousScroll);
      DisplayApplicationSelectScreen(display, sessions[itemIndexApp].name, sessions[itemIndexApp].volume, sessions[itemIndexApp].isMuted, scrollLeft, scrollRight, mode, MODE_COUNT);
    }

    else if(stateApplication == STATE_APPLICATION_EDIT)
      DisplayApplicationEditScreen(display, sessions[itemIndexApp].name, sessions[itemIndexApp].volume, sessions[itemIndexApp].isMuted, mode, MODE_COUNT);
  }
  else if(mode == MODE_GAME)
  {
    if(stateGame == STATE_GAME_SELECT_A)
    {
      uint8_t scrollLeft = CanScrollLeft(itemIndexGameA, sessionCount, settings.continuousScroll);
      uint8_t scrollRight = CanScrollRight(itemIndexGameA, sessionCount, settings.continuousScroll);
      DisplayGameSelectScreen(display, sessions[itemIndexGameA].name, sessions[itemIndexGameA].volume, sessions[itemIndexGameA].isMuted, "A", scrollLeft, scrollRight, mode, MODE_COUNT);
    }
    else if(stateGame == STATE_GAME_SELECT_B)
    {
      uint8_t scrollLeft = CanScrollLeft(itemIndexGameB, sessionCount, settings.continuousScroll);
      uint8_t scrollRight = CanScrollRight(itemIndexGameB, sessionCount, settings.continuousScroll);
      DisplayGameSelectScreen(display, sessions[itemIndexGameB].name, sessions[itemIndexGameB].volume, sessions[itemIndexGameB].isMuted, "B", scrollLeft, scrollRight, mode, MODE_COUNT);
    }
    else if(stateGame == STATE_GAME_EDIT)
      DisplayGameEditScreen(display, sessions[itemIndexGameA].name, sessions[itemIndexGameB].name, sessions[itemIndexGameA].volume, sessions[itemIndexGameB].volume, sessions[itemIndexGameA].isMuted, sessions[itemIndexGameB].isMuted, mode, MODE_COUNT);
  }
}

//---------------------------------------------------------
//---------------------------------------------------------
void UpdateLighting()
{
  if(stateDisplay == STATE_DISPLAY_SLEEP)
  {
    SetPixelsColor(pixels, 0,0,0);
    return;
  }

  if(sessionCount == 0)
  {
    SetPixelsColor(pixels, 128,128,128);
    return;
  }
  
  if(mode == MODE_OUTPUT)
  {
    uint8_t volumeColor = round(sessions[0].volume * 2.55f);
    SetPixelsColor(pixels, volumeColor, 255 - volumeColor, volumeColor);
  }
  else if(mode == MODE_APPLICATION)
  {
    uint8_t volumeColor = round(sessions[itemIndexApp].volume * 2.55f);
    SetPixelsColor(pixels, volumeColor, 255 - volumeColor, volumeColor);
  }
  else if(mode == MODE_GAME)
  {
    uint8_t volumeColor;
    if(stateGame == STATE_GAME_SELECT_A)
    {
      volumeColor = round(sessions[itemIndexGameA].volume * 2.55f);
      SetPixelsColor(pixels, volumeColor, 255 - volumeColor, volumeColor);
    }
    else if(stateGame == STATE_GAME_SELECT_B)
    {
      volumeColor = round(sessions[itemIndexGameB].volume * 2.55f);
      SetPixelsColor(pixels, volumeColor, 255 - volumeColor, volumeColor);
    }
    else
    {
      SetPixelsColor(pixels, 128, 128, 128);
    }
  }
}

//---------------------------------------------------------
//---------------------------------------------------------
void CycleMode()
{
    mode++;

    if(mode == MODE_COUNT)
      mode = MODE_OUTPUT;
}

//---------------------------------------------------------
//---------------------------------------------------------
uint8_t CycleState(uint8_t state, uint8_t count)
{
  state++;
  if(state == count)
    state = 0;

  return state;
}

//---------------------------------------------------------
//---------------------------------------------------------
void ToggleMute(Item* items, int8_t index)
{
  items[index].isMuted = !items[index].isMuted;
  SendItemVolumeCommand(&items[index], sendBuffer, encodeBuffer, &waitingAck, &ackTimer, now);
}

//---------------------------------------------------------
//---------------------------------------------------------
void ResetGameVolume()
{
  sessions[itemIndexGameA].volume = 50;
  sessions[itemIndexGameB].volume = 50;

  SendItemVolumeCommand(&sessions[itemIndexGameA], sendBuffer, encodeBuffer, &waitingAck, &ackTimer, now);
  SendItemVolumeCommand(&sessions[itemIndexGameB], sendBuffer, encodeBuffer, &waitingAck, &ackTimer, now);
}

void RebalanceGameVolume(uint8_t sourceVolume, uint8_t targetIndex)
{
  sessions[targetIndex].volume = 100 - sourceVolume;
  SendItemVolumeCommand(&sessions[targetIndex], sendBuffer, encodeBuffer, &waitingAck, &ackTimer, now);
}

//---------------------------------------------------------
// Finds the item with the given id.
// Returns the index of the item if found, -1 otherwise.
//---------------------------------------------------------
int8_t FindItem(uint32_t id, Item* items, uint8_t itemCount)
{  
  for(int8_t i = 0; i < itemCount; i++)
    if(items[i].id == id)
      return i;

  return -1;
}

//---------------------------------------------------------
// \brief Checks if target ID is the active application.
// \param index The index of the item to be checked.
// \returns true if ids match.
//---------------------------------------------------------
bool IsItemActive(int8_t index)
{
  if(mode == MODE_OUTPUT && itemIndexOutput == index)
    return true;

  else if(mode == MODE_APPLICATION && itemIndexApp == index)
    return true;

  else if(mode == MODE_GAME && (itemIndexGameA == index || itemIndexGameB == index))
    return true;

  return false;
}

