// © Kay Sievers <kay@versioduo.com>, 2020-2022
// SPDX-License-Identifier: Apache-2.0

#include <V2Device.h>
#include <V2LED.h>
#include <V2Link.h>
#include <V2MIDI.h>
#include <V2Potentiometer.h>

V2DEVICE_METADATA("com.versioduo.express", 22, "versioduo:samd:express");

static constexpr struct { uint8_t count{16}; } Ports;
static V2LED::WS2812 LED(Ports.count, PIN_LED_WS2812, &sercom2, SPI_PAD_0_SCK_1, PIO_SERCOM);
static V2Link::Port Plug(&SerialPlug);
static V2Link::Port Socket(&SerialSocket);
static V2Base::Analog::ADC ADC[]{
  V2Base::Analog::ADC(0),
  V2Base::Analog::ADC(1),
};

static class Device : public V2Device {
public:
  Device() : V2Device() {
    metadata.vendor      = "Versio Duo";
    metadata.product     = "V2 express";
    metadata.description = "16 channel Analog Expression Controller";
    metadata.home        = "https://versioduo.com/#express";

    system.download  = "https://versioduo.com/download";
    system.configure = "https://versioduo.com/configure";

    configuration = {.size{sizeof(config)}, .data{&config}};
  }

  // Config, written to EEPROM
  struct {
    uint8_t channel{};
  } config;

  void play(int8_t note, int8_t velocity) {
    if (note < V2MIDI::C(3))
      return;

    note -= V2MIDI::C(3);
    if (note >= Ports.count)
      return;

    float fraction = (float)velocity / 127;

    if (velocity > 0)
      LED.setHSV(note, 120, 1, fraction);

    else
      LED.setBrightness(note, 0);
  }

private:
  const struct V2Potentiometer::Config _config { .nSteps{128}, .min{0.05}, .max{0.7}, .alpha{0.3}, .lag{0.007}, };

  V2Potentiometer _potis[Ports.count]{
    V2Potentiometer(&_config),
    V2Potentiometer(&_config),
    V2Potentiometer(&_config),
    V2Potentiometer(&_config),
    V2Potentiometer(&_config),
    V2Potentiometer(&_config),
    V2Potentiometer(&_config),
    V2Potentiometer(&_config),
    V2Potentiometer(&_config),
    V2Potentiometer(&_config),
    V2Potentiometer(&_config),
    V2Potentiometer(&_config),
    V2Potentiometer(&_config),
    V2Potentiometer(&_config),
    V2Potentiometer(&_config),
    V2Potentiometer(&_config),
  };

  uint8_t _steps[Ports.count]{};
  uint32_t _measureUsec{};
  uint32_t _eventsUsec{};
  V2MIDI::Packet _midi{};

  void handleReset() override {
    LED.reset();

    for (uint8_t i = 0; i < Ports.count; i++)
      _potis[i].reset();

    memset(_steps, 0, sizeof(_steps));
    _measureUsec = 0;
    _eventsUsec  = V2Base::getUsec();
    _midi        = {};
  }

  void allNotesOff() {
    sendEvents(true);
  }

  void handleLoop() override {
    if (V2Base::getUsecSince(_measureUsec) > 10 * 1000) {
      for (uint8_t i = 0; i < Ports.count; i++) {
        const uint8_t id      = V2Base::Analog::ADC::getID(PIN_CHANNEL_SENSE + i);
        const uint8_t channel = V2Base::Analog::ADC::getChannel(PIN_CHANNEL_SENSE + i);
        _potis[i].measure(ADC[id].readChannel(channel));
      }

      _measureUsec = V2Base::getUsec();
    }

    if (V2Base::getUsecSince(_eventsUsec) > 50 * 1000) {
      sendEvents();
      _eventsUsec = V2Base::getUsec();
    }
  }

  bool handleSend(V2MIDI::Packet *midi) override {
    usb.midi.send(midi);
    Plug.send(midi);
    return true;
  }

  void sendEvents(bool force = false) {
    for (uint8_t i = 0; i < Ports.count; i++) {
      if (!force && _steps[i] == _potis[i].getStep())
        continue;

      LED.setBrightness(i, (float)_potis[i].getFraction());
      send(_midi.setControlChange(config.channel, V2MIDI::CC::GeneralPurpose1 + i, _potis[i].getStep()));
      _steps[i] = _potis[i].getStep();
    }
  }

  void handleNote(uint8_t channel, uint8_t note, uint8_t velocity) override {
    play(note, velocity);
  }

  void handleNoteOff(uint8_t channel, uint8_t note, uint8_t velocity) override {
    play(note, 0);
  }

  void handleControlChange(uint8_t channel, uint8_t controller, uint8_t value) override {
    switch (controller) {
      case V2MIDI::CC::AllNotesOff:
      case V2MIDI::CC::AllSoundOff:
        allNotesOff();
        break;
    }
  }

  void handleSystemReset() override {
    reset();
  }

  void exportSettings(JsonArray json) override {
    {
      JsonObject setting = json.add<JsonObject>();
      setting["type"]    = "number";
      setting["title"]   = "MIDI";
      setting["label"]   = "Channel";
      setting["min"]     = 1;
      setting["max"]     = 16;
      setting["input"]   = "select";
      setting["path"]    = "midi/channel";
    }
  }

  void importConfiguration(JsonObject json) override {
    JsonObject json_midi = json["midi"];
    if (json_midi) {
      if (!json_midi["channel"].isNull()) {
        uint8_t channel = json_midi["channel"];

        if (channel < 1)
          config.channel = 0;
        else if (channel > 16)
          config.channel = 15;
        else
          config.channel = channel - 1;
      }
    }
  }

  void exportConfiguration(JsonObject json) override {
    json["#midi"]         = "The MIDI settings";
    JsonObject json_midi  = json["midi"].to<JsonObject>();
    json_midi["#channel"] = "The channel to send notes and control values to";
    json_midi["channel"]  = config.channel + 1;
  }

  void exportInput(JsonObject json) override {
    // The range of notes we receive to drive the LEDs.
    JsonObject json_chromatic = json["chromatic"].to<JsonObject>();
    json_chromatic["start"]   = V2MIDI::C(3);
    json_chromatic["count"]   = Ports.count;
  }

  void exportOutput(JsonObject json) override {
    json["channel"] = config.channel;

    // List of controllers we send out; generic CC values, one per channel.
    JsonArray json_controllers = json["controllers"].to<JsonArray>();
    for (uint8_t i = 0; i < Ports.count; i++) {
      char name[11];
      sprintf(name, "Channel %d", i + 1);

      JsonObject json_controller = json_controllers.add<JsonObject>();
      json_controller["name"]    = name;
      json_controller["number"]  = V2MIDI::CC::GeneralPurpose1 + i;
    }
  }
} Device;

// Dispatch MIDI packets
static class MIDI {
public:
  void loop() {
    if (!Device.usb.midi.receive(&_midi))
      return;

    if (_midi.getPort() == 0) {
      Device.dispatch(&Device.usb.midi, &_midi);

    } else {
      _midi.setPort(_midi.getPort() - 1);
      Socket.send(&_midi);
    }
  }

private:
  V2MIDI::Packet _midi{};
} MIDI;

// Dispatch Link packets
static class Link : public V2Link {
public:
  Link() : V2Link(&Plug, &Socket) {}

private:
  V2MIDI::Packet _midi{};

  // Receive a host event from our parent device
  void receivePlug(V2Link::Packet *packet) override {
    if (packet->getType() == V2Link::Packet::Type::MIDI) {
      packet->receive(&_midi);
      Device.dispatch(&Plug, &_midi);
    }
  }

  // Forward children device events to the host
  void receiveSocket(V2Link::Packet *packet) override {
    if (packet->getType() == V2Link::Packet::Type::MIDI) {
      uint8_t address = packet->getAddress();
      if (address == 0x0f)
        return;

      if (Device.usb.midi.connected()) {
        packet->receive(&_midi);
        _midi.setPort(address + 1);
        Device.usb.midi.send(&_midi);
      }
    }
  }
} Link;

void setup() {
  Serial.begin(9600);

  LED.begin();
  LED.setMaxBrightness(0.5);

  Plug.begin();
  Socket.begin();
  Device.link = &Link;

  // Set the SERCOM interrupt priority, it requires a stable ~300 kHz interrupt
  // frequency. This needs to be after begin().
  setSerialPriority(&SerialPlug, 2);
  setSerialPriority(&SerialSocket, 2);

  for (uint8_t i = 0; i < V2Base::countof(ADC); i++)
    ADC[i].begin();

  for (uint8_t i = 0; i < Ports.count; i++) {
    const uint8_t id      = V2Base::Analog::ADC::getID(PIN_CHANNEL_SENSE + i);
    const uint8_t channel = V2Base::Analog::ADC::getChannel(PIN_CHANNEL_SENSE + i);
    ADC[id].addChannel(channel);
  }

  Device.begin();
  Device.reset();
}

void loop() {
  LED.loop();
  MIDI.loop();
  Link.loop();
  Device.loop();

  if (Link.idle() && Device.idle())
    Device.sleep();
}
