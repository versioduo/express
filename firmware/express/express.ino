// © Kay Sievers <kay@versioduo.com>, 2020-2024
// SPDX-License-Identifier: Apache-2.0

#include <V2Device.h>
#include <V2LED.h>
#include <V2Link.h>
#include <V2MIDI.h>
#include <V2Potentiometer.h>

V2DEVICE_METADATA("com.versioduo.express", 24, "versioduo:samd:express");

namespace {
  constexpr struct {
    uint8_t count{16};
  } Ports;

  V2LED::WS2812       LED(Ports.count, PIN_LED_WS2812, &sercom2, SPI_PAD_0_SCK_1, PIO_SERCOM);
  V2Link::Port        Plug(&SerialPlug, PIN_SERIAL_PLUG_TX_ENABLE);
  V2Link::Port        Socket(&SerialSocket, PIN_SERIAL_SOCKET_TX_ENABLE);
  V2Base::Analog::ADC ADC[]{0, 1};

  class Device : public V2Device {
  public:
    Device() : V2Device() {
      metadata.vendor      = "Versio Duo";
      metadata.product     = "V2 express";
      metadata.description = "16 channel Analog Expression Controller";
      metadata.home        = "https://versioduo.com/#express";

      system.download  = "https://versioduo.com/download";
      system.configure = "https://versioduo.com/configure";

      configuration = {.version{1}, .size{sizeof(config)}, .data{&config}};
    }

    enum class CC {
      Rainbow = V2MIDI::CC::Controller90,
    };

    // Config, written to EEPROM
    struct {
      struct {
        uint8_t channel{};
        uint8_t controller;

        struct {
          bool  invert{};
          float min{};
          float max{1};
        } range;
      } ports[Ports.count];
    } config{.ports{
      {.controller{V2MIDI::CC::GeneralPurpose1 + 0}},
      {.controller{V2MIDI::CC::GeneralPurpose1 + 1}},
      {.controller{V2MIDI::CC::GeneralPurpose1 + 2}},
      {.controller{V2MIDI::CC::GeneralPurpose1 + 3}},
      {.controller{V2MIDI::CC::GeneralPurpose1 + 4}},
      {.controller{V2MIDI::CC::GeneralPurpose1 + 5}},
      {.controller{V2MIDI::CC::GeneralPurpose1 + 6}},
      {.controller{V2MIDI::CC::GeneralPurpose1 + 7}},
      {.controller{V2MIDI::CC::GeneralPurpose1 + 8}},
      {.controller{V2MIDI::CC::GeneralPurpose1 + 9}},
      {.controller{V2MIDI::CC::GeneralPurpose1 + 10}},
      {.controller{V2MIDI::CC::GeneralPurpose1 + 11}},
      {.controller{V2MIDI::CC::GeneralPurpose1 + 12}},
      {.controller{V2MIDI::CC::GeneralPurpose1 + 13}},
      {.controller{V2MIDI::CC::GeneralPurpose1 + 14}},
      {.controller{V2MIDI::CC::GeneralPurpose1 + 15}},
    }};

    void play(int8_t note, int8_t velocity) {
      if (note < V2MIDI::C(3))
        return;

      note -= V2MIDI::C(3);
      if (note >= Ports.count)
        return;

      float fraction{(float)velocity / 127};

      if (velocity > 0)
        LED.setHSV(note, 120, 1, fraction);

      else
        LED.setBrightness(note, 0);
    }

  private:
    const struct V2Potentiometer::Config _config{
      .nSteps{128},
      .min{0.05},
      .max{0.9},
      .alpha{0.3},
      .lag{0.02},
    };

    V2Potentiometer _potis[Ports.count]{
      &_config,
      &_config,
      &_config,
      &_config,
      &_config,
      &_config,
      &_config,
      &_config,
      &_config,
      &_config,
      &_config,
      &_config,
      &_config,
      &_config,
      &_config,
      &_config,
    };

    uint8_t        _steps[Ports.count]{};
    uint32_t       _measureUsec{};
    uint32_t       _eventsUsec{};
    float          _rainbow{};
    V2MIDI::Packet _midi;

    void handleReset() override {
      LED.reset();

      for (auto& p : _potis)
        p.reset();

      memset(_steps, 0, sizeof(_steps));
      _measureUsec = 0;
      _eventsUsec  = V2Base::getUsec();
      _rainbow     = 0;
      _midi        = {};
    }

    void allNotesOff() {
      sendEvents(true);
    }

    void handleLoop() override {
      auto measurePort{[this](uint8_t port) -> float {
        const uint8_t id{V2Base::Analog::ADC::getID(PIN_CHANNEL_SENSE + port)};
        const uint8_t channel{V2Base::Analog::ADC::getChannel(PIN_CHANNEL_SENSE + port)};
        float         measure{ADC[id].readChannel(channel)};
        if (config.ports[port].range.invert)
          measure = 1.f - measure;

        float value{measure - config.ports[port].range.min};
        if (value < 0.f)
          return 0;

        value *= 1.f / (config.ports[port].range.max - config.ports[port].range.min);
        return value;
      }};

      if (V2Base::getUsecSince(_measureUsec) > 10 * 1000) {
        for (uint8_t i{}; i < Ports.count; i++)
          _potis[i].measure(measurePort(i));

        _measureUsec = V2Base::getUsec();
      }

      if (V2Base::getUsecSince(_eventsUsec) > 50 * 1000) {
        sendEvents();
        _eventsUsec = V2Base::getUsec();
      }
    }

    bool handleSend(V2MIDI::Packet* midi) override {
      usb.midi.send(midi);
      Plug.send(midi);
      return true;
    }

    void sendEvents(bool force = false) {
      for (uint8_t i{}; i < Ports.count; i++) {
        if (!force && _steps[i] == _potis[i].getStep())
          continue;

        LED.setBrightness(i, (float)_potis[i].getFraction());
        send(_midi.setControlChange(config.ports[i].channel, config.ports[i].controller, _potis[i].getStep()));
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
        case (uint8_t)CC::Rainbow:
          _rainbow = (float)value / 127.f;
          if (_rainbow <= 0.f)
            LED.reset();
          else
            LED.rainbow(1, 4.5f - (_rainbow * 4.f));
          break;

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
      for (uint8_t i{}; i < Ports.count; i++) {
        {
          JsonObject setting{json.add<JsonObject>()};
          setting["type"] = "title";
          char name[32];
          sprintf(name, "Port %d", i + 1);
          setting["title"] = name;
        }
        {
          JsonObject setting{json.add<JsonObject>()};
          setting["type"]  = "number";
          setting["label"] = "Channel";
          setting["min"]   = 1;
          setting["max"]   = 16;
          setting["input"] = "select";
          char path[64];
          sprintf(path, "ports[%d]/channel", i);
          setting["path"] = path;
        }
        {
          JsonObject setting{json.add<JsonObject>()};
          setting["type"]  = "controller";
          setting["label"] = "Controller";
          char path[64];
          sprintf(path, "ports[%d]/controller", i);
          setting["path"] = path;
        }
        {
          JsonObject setting{json.add<JsonObject>()};
          setting["ruler"] = true;
          setting["type"]  = "toggle";
          setting["label"] = "Direction";
          setting["text"]  = "Invert";
          char path[64];
          sprintf(path, "ports[%d]/range/invert", i);
          setting["path"] = path;
        }
        {
          JsonObject setting{json.add<JsonObject>()};
          setting["type"]  = "number";
          setting["label"] = "Position";
          setting["text"]  = "Minimum";
          setting["max"]   = 1;
          setting["step"]  = 0.01;
          char path[64];
          sprintf(path, "ports[%d]/range/min", i);
          setting["path"] = path;
        }
        {
          JsonObject setting{json.add<JsonObject>()};
          setting["type"]  = "number";
          setting["label"] = "Position";
          setting["text"]  = "Maximum";
          setting["max"]   = 1;
          setting["step"]  = 0.01;
          char path[64];
          sprintf(path, "ports[%d]/range/max", i);
          setting["path"] = path;
        }
      }
    }

    void importConfiguration(JsonObject json) override {
      JsonArray jsonPorts{json["ports"]};
      if (jsonPorts) {
        for (uint8_t i{}; i < Ports.count; i++) {
          JsonObject jsonPort = jsonPorts[i];

          if (!jsonPort["channel"].isNull()) {
            uint8_t channel = jsonPort["channel"];

            if (channel < 1)
              config.ports[i].channel = 0;
            else if (channel > 16)
              config.ports[i].channel = 95;
            else
              config.ports[i].channel = channel - 1;
          }

          if (!jsonPort["controller"].isNull()) {
            config.ports[i].controller = jsonPort["controller"];
            if (config.ports[i].controller > 127)
              config.ports[i].controller = 127;
          }

          if (!jsonPort["range"].isNull()) {
            JsonObject jsonRange{jsonPort["range"]};

            if (!jsonRange["invert"].isNull())
              config.ports[i].range.invert = jsonRange["invert"];

            if (!jsonRange["min"].isNull()) {
              float value{jsonRange["min"]};
              if (value < 0.f || value > 1.f)
                value = 0;

              config.ports[i].range.min = value;
              if (config.ports[i].range.min >= config.ports[i].range.max)
                config.ports[i].range.max = 1;
            }

            if (!jsonRange["max"].isNull()) {
              float value{jsonRange["max"]};
              if (value < 0.f || value > 1.f)
                value = 1;

              config.ports[i].range.max = value;
              if (config.ports[i].range.min >= config.ports[i].range.max)
                config.ports[i].range.min = 0;
            }
          }
        }
      }
    }

    void exportConfiguration(JsonObject json) override {
      JsonArray jsonPorts{json["ports"].to<JsonArray>()};
      for (uint8_t i{}; i < Ports.count; i++) {
        JsonObject jsonPort{jsonPorts.add<JsonObject>()};

        if (i == 0)
          jsonPort["#channel"] = "The channel to send notes and control values to";
        jsonPort["channel"] = config.ports[i].channel + 1;

        if (i == 0)
          jsonPort["#controller"] = "The controller number";
        jsonPort["controller"] = config.ports[i].controller;

        {
          JsonObject jsonRange = jsonPort["range"].to<JsonObject>();
          if (i == 0)
            jsonRange["#invert"] = "Change the direction of the measurement";
          jsonRange["invert"] = config.ports[i].range.invert;

          if (i == 0)
            jsonRange["#min"] = "The idle / minimum / 0 position (0 .. 1)";
          jsonRange["min"] = serialized(String(config.ports[i].range.min, 2));

          if (i == 0)
            jsonRange["#max"] = "The maximum / 127 position (0 .. 1)";
          jsonRange["max"] = serialized(String(config.ports[i].range.max, 2));
        }
      }
    }

    void exportInput(JsonObject json) override {
      JsonObject json_chromatic = json["chromatic"].to<JsonObject>();
      json_chromatic["start"]   = V2MIDI::C(3);
      json_chromatic["count"]   = Ports.count;

      JsonArray jsonControllers{json["controllers"].to<JsonArray>()};
      {
        JsonObject jsonController{jsonControllers.add<JsonObject>()};
        jsonController["name"]   = "Rainbow";
        jsonController["number"] = (uint8_t)CC::Rainbow;
        jsonController["value"]  = (uint8_t)(_rainbow * 127.f);
      }
    }

    void exportOutput(JsonObject json) override {
      JsonArray json_controllers{json["controllers"].to<JsonArray>()};
      for (uint8_t i{}; i < Ports.count; i++) {
        char name[16];
        sprintf(name, "Port %d", i + 1);

        JsonObject json_controller{json_controllers.add<JsonObject>()};
        json_controller["name"]   = name;
        json_controller["number"] = config.ports[i].controller;
        json_controller["value"]  = _potis[i].getStep();
      }
    }
  } Device;

  // Dispatch MIDI packets.
  class MIDI {
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

  // Dispatch Link packets.
  class Link : public V2Link {
  public:
    Link() : V2Link(&Plug, &Socket) {
      Device.link = this;
    }

  private:
    V2MIDI::Packet _midi{};

    // Receive a host event from our parent device.
    void receivePlug(V2Link::Packet* packet) override {
      if (packet->getType() == V2Link::Packet::Type::MIDI) {
        packet->receive(&_midi);
        Device.dispatch(&Plug, &_midi);
      }
    }

    // Forward children device events to the host.
    void receiveSocket(V2Link::Packet* packet) override {
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
}

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

  for (uint8_t i{}; i < V2Base::countof(ADC); i++)
    ADC[i].begin();

  for (uint8_t i{}; i < Ports.count; i++) {
    const uint8_t id{V2Base::Analog::ADC::getID(PIN_CHANNEL_SENSE + i)};
    const uint8_t channel{V2Base::Analog::ADC::getChannel(PIN_CHANNEL_SENSE + i)};
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

  if (Device.idle())
    Device.sleep();
}
