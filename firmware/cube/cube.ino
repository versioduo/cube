#include "MIDISong.h"
#include <V2Audio.h>
#include <V2Buttons.h>
#include <V2Device.h>
#include <V2LED.h>
#include <V2Link.h>
#include <V2MIDI.h>
#include <V2Music.h>
#include <V2PowerSupply.h>
#include <Wire.h>
#include <arm_math.h>

V2DEVICE_METADATA("com.versioduo.cube", 1, "versioduo:samd:wave");

static V2LED::WS2812       LED(2, PIN_LED_WS2812, &sercom5, SPI_PAD_3_SCK_1, PIO_SERCOM_ALT);
static V2LED::WS2812       LEDExt(64, PIN_LED_WS2812_EXT, &sercom4, SPI_PAD_0_SCK_1, PIO_SERCOM);
static V2Link::Port        Plug(&SerialPlug, PIN_SERIAL_PLUG_TX_ENABLE);
static V2Link::Port        Socket(&SerialSocket, PIN_SERIAL_SOCKET_TX_ENABLE);
static V2Base::Analog::ADC ADC(1);

static class Power : public V2PowerSupply {
public:
  constexpr Power() : V2PowerSupply({.min{6}, .max{26}}) {}

private:
  float handleMeasurement() override {
    return 36.f * ADC.readChannel(V2Base::Analog::ADC::getChannel(PIN_VOLTAGE_SENSE));
  }

  void handleNotify(float voltage) override {
    // Loss of power or requests to switch-on without a power connection show yellow LEDs.
    if (voltage < config.min) {
      LED.splashHSV(0.5, V2Colour::Yellow, 1, 0.5);
      return;
    }
  }
} Power;

static class MA12070P : public V2Audio::Codec {
public:
  MA12070P() : V2Audio::Codec(PIN_I2S_SCK, PIN_I2S_FS, PIN_I2S_SDO, PIN_I2S_MCK) {}

  void setGain(float gain) {
    reset();
    _gain = gain;
  }

private:
  enum class Address {
    Base = 0x20,
  };

  enum class RegisterAddress {
    AudioProcessor = 0x35, // Bit 3
    Limiter        = 0x36, // Bit 6
    VolumeMain     = 0x40,
  };

  float _gain{};

  bool handlePower(bool on) override {
    if (on) {
      bool continuous;

      // If we do not have enough power, shut down.
      if (!Power.on(continuous))
        return false;

    } else {
      Power.off();
    }

    return true;
  }

  bool handleEnable(bool on) override {
    if (on) {
      digitalWrite(PIN_CODEC_ENABLE, LOW);
      delay(1);

      if (!configure()) {
        digitalWrite(PIN_CODEC_ENABLE, HIGH);
        return false;
      }

      digitalWrite(PIN_CODEC_MUTE, HIGH);

    } else {
      digitalWrite(PIN_CODEC_MUTE, LOW);
      digitalWrite(PIN_CODEC_ENABLE, HIGH);
    }

    return true;
  }

  bool writeRegister(RegisterAddress address, uint8_t value) {
    Wire.beginTransmission((uint8_t)Address::Base);
    Wire.write((uint8_t)address);
    Wire.write(value);
    Wire.endTransmission();

    // Read back the values to make sure we never use the
    // amplifier without the configured volume control.
    Wire.beginTransmission((uint8_t)Address::Base);
    Wire.write((uint8_t)address);
    Wire.endTransmission();
    Wire.requestFrom((uint8_t)Address::Base, 1);
    if (!Wire.available())
      return false;

    return Wire.read() == value;
  }

  bool configure() {
    // Enable the audio processor / volume control.
    if (!writeRegister(RegisterAddress::AudioProcessor, 8 | 1))
      return false;

    // Enable the limiter.
    //
    // Errata – The Limiter is false active for a short period when the input signal is close
    // to silence and the total gain of the channel is equal to -64.25 dBFS or below.
    if (_gain > -64.f && !writeRegister(RegisterAddress::Limiter, 0x40 | 1))
      return false;

    if (!writeRegister(RegisterAddress::VolumeMain, -_gain + 24.f))
      return false;

    return true;
  }
} Codec;

static class Synth : private V2Audio::Codec::Channel {
public:
  enum class Form { Sine, Triangle, Sawtooth, Square, _count };

  struct {
    V2Audio::Fader sine{1};
    V2Audio::Fader triangle;
    V2Audio::Fader sawtooth;
    V2Audio::Fader square;
  } wave;

  struct {
    float attack{0.5};
    float decay{0.5};
    float release{0.5};
  } envelope;

  struct {
    float depth{0.5};
  } vibrato;

  // Pitch sliding from one note to another.
  float portamento{0.5};

  constexpr Synth(uint8_t channel = 0) : _channel(channel) {}

  void begin() {
    Codec.registerChannel(_channel, this);
  }

  void resetParameters() {
    if (_volume > 0.f) {
      _delayResetParameters = true;
      return;
    }

    _frequency = 0;

    wave.sine.reset();
    wave.triangle.reset();
    wave.sawtooth.reset();
    wave.square.reset();

    envelope.attack  = 0.5;
    envelope.decay   = 0.5;
    envelope.release = 0.5;

    _distortion.amount = 0;
    _distortion.phasor.reset();
    _distortion.active = false;

    portamento = 0.5;

    vibrato.depth = 0.5;
    _vibrato.rate = 0;
    _vibrato.phasor.reset();
    _vibrato.active = false;
  }

  void reset() {
    _delayResetParameters = false;
    resetParameters();
    _volume.reset();
    _phasor.reset();
  }

  void play(float frequency, float volume) {
    noInterrupts();

    _frequency   = frequency;
    _volume.base = volume;
    _volume.peak = powf(volume, 1.f - envelope.decay);
    _volume.setTarget(_volume.peak);

    if (!Codec.isChannelEnabled(_channel)) {
      _phasor.setFrequency(_frequency);

      if (!Codec.enableChannel(_channel)) {
        LED.splashHSV(1, V2Colour::Magenta, 1, 0.5);
        stop();
      }

    } else {
      _phasor.setFaderSpeed(0.1f + (1.f * powf(portamento, 3)), 20, 20000);
      _phasor.setFrequencyTarget(_frequency);
    }

    _volume.setFadeSec(0.01f + (0.5f * powf(envelope.attack, 3)));
    interrupts();
  }

  void stop() {
    _frequency = 0;

    _volume.base = 0;
    _volume.peak = 0;
    _volume.setTarget(0);
    _volume.setFadeSec(0.02f + (0.8f * powf(envelope.release, 3)));
  }

  float getDistortion() const {
    return _distortion.amount;
  }

  void setDistortion(float amount) {
    _distortion.amount = amount;
    if (_distortion.amount <= 0.f) {
      _distortion.active = false;
      _distortion.phasor.reset();
      return;
    }

    const float offset = 2000.f * powf(_distortion.amount, 3);
    _distortion.phasor.setFaderSpeed(0.05, 20, 20000);
    _distortion.phasor.setFrequencyTarget(_frequency + offset);

    if (!_distortion.active) {
      _distortion.phasor.set(_phasor);
      _distortion.active = true;
    }
  }

  float getVibrato() const {
    return _vibrato.rate;
  }

  void setVibrato(float rate) {
    _vibrato.rate = rate;
    if (_vibrato.rate <= 0.f) {
      _vibrato.active = false;
      _vibrato.phasor.reset();
      return;
    }

    // A typical string vibrato is 5-8 Hz, 0.2-0.4 semitones.
    _vibrato.phasor.setFaderSpeed(0.05, 1, 10);
    _vibrato.phasor.setFrequencyTarget(1.f + (9.f * _vibrato.rate));
    _vibrato.active = true;
    _vibrato.on     = false;
  }

private:
  class Phasor : public V2Audio::Phasor {
  private:
    float getClockFrequency() override {
      return Codec.getFrequency();
    }
  };

  const uint8_t _channel;
  bool          _delayResetParameters{};

  float  _frequency{};
  Phasor _phasor;

  struct {
    bool   active{};
    float  rate{};
    Phasor phasor;
    bool   on{};
  } _vibrato;

  struct {
    bool   active{};
    float  amount{};
    Phasor phasor;
  } _distortion;

  class Volume : public V2Audio::Fader {
  public:
    float peak{};
    float base{};

    void setFadeSec(float durationSec) {
      setStepsRange(durationSec * Codec.getFrequency());
    }
  } _volume;

  float getFormSample(Form form, float phase) const {
    switch (form) {
      case Form::Sine:
        return arm_sin_f32(2.f * (float)M_PI * phase);

      case Form::Triangle:
        if (phase < 0.25f)
          return 4.f * phase;

        if (phase < 0.75f)
          return 2.f - (4.f * phase);

        return (4.f * phase) - 4.f;

      case Form::Sawtooth:
        if (phase < 0.5f)
          return 2.f * phase;

        return (2.f * phase) - 2.f;

      case Form::Square:
        if (phase < 0.5f)
          return 1.f;

        return -1.f;
    }

    return 0;
  }

  float getMixedSample(float phase) {
    float sample{};
    float volume{};

    wave.sine.step();
    if (wave.sine > 0.f) {
      volume += wave.sine;
      sample += getFormSample(Form::Sine, phase) * wave.sine;
    }

    wave.triangle.step();
    if (wave.triangle > 0.f) {
      volume += wave.triangle;
      sample += getFormSample(Form::Triangle, phase) * wave.triangle;
    }

    wave.sawtooth.step();
    if (wave.sawtooth > 0.f) {
      volume += wave.sawtooth;
      sample += getFormSample(Form::Sawtooth, phase) * wave.sawtooth;
    }

    wave.square.step();
    if (wave.square > 0.f) {
      volume += wave.square;
      sample += getFormSample(Form::Square, phase) * wave.square;
    }

    if (volume > 1.f)
      sample /= volume;

    return sample;
  }

  float getSample() override {
    const bool  reset  = _phasor.step();
    const float phase  = _phasor;
    float       sample = getMixedSample(phase);

    // https://en.wikipedia.org/wiki/Phase_distortion_synthesis
    if (_distortion.active) {
      _distortion.phasor.step();
      // The resonance frequency counter at a slightly higher frequency, being
      // reset / synced when the phase wraps around.
      if (reset)
        _distortion.phasor.set(0);

      // The resonance phase used as the waveform.
      float distortion = getFormSample(Form::Sine, _distortion.phasor);

      // Multiply the inverted base phase, it levels out the jump
      // from the reset.
      distortion *= 1.f - phase;

      sample *= 1.f - _distortion.amount;
      sample += distortion * _distortion.amount;
    }

    if (_vibrato.active) {
      _vibrato.phasor.step();

      // A typical string vibrato is 5-8 Hz, 0.2-0.4 semitones.
      const bool on = getFormSample(Form::Square, _vibrato.phasor) > 0.f;
      if (on != _vibrato.on) {
        const float direction = on ? 1.f : -1.f;
        const float delta     = (_frequency * powf(2, 1.f / 12.f)) - _frequency;
        const float depth     = 0.1f + (0.5f * vibrato.depth);
        _phasor.setFaderSpeed(0.1, 20, 20000);
        _phasor.setFrequencyTarget(_frequency + (direction * delta * depth));
      }

      _vibrato.on = on;
    }

    if (_volume.step()) {
      if (_volume.peak > 0.f) {
        // Move from the peak to the base volume
        _volume.setTarget(_volume.base);
        _volume.peak = 0;

      } else if (_volume <= 0.f) {
        // Switch off.
        if (_delayResetParameters) {
          _delayResetParameters = false;
          resetParameters();
        }

        Codec.disableChannel(_channel);
      }
    }

    return sample * _volume;
  }
} Synth;

// Config, written to EEPROM.
static constexpr struct Configuration {
  float gain{-30};
  float tuning{};

  // LED color.
  struct {
    uint8_t h{15};
    uint8_t s{40};
    uint8_t v{100};
  } color;
} ConfigurationDefault;

static class Device : public V2Device {
public:
  constexpr Device() : V2Device() {
    metadata.description = "Wave Synthesizer";
    metadata.vendor      = "Versio Duo";
    metadata.product     = "V2 cube";
    metadata.home        = "https://versioduo.com/#cube";

    system.download  = "https://versioduo.com/download";
    system.configure = "https://versioduo.com/configure";

    // https://github.com/versioduo/arduino-board-package/blob/main/boards.txt
    usb.pid = 0xe9d0;

    configuration = {.size{sizeof(config)}, .data{&config}};
  }

  enum class CC {
    Volume = V2MIDI::CC::ChannelVolume,

    WaveSine     = V2MIDI::CC::Controller102,
    WaveTriangle = V2MIDI::CC::Controller103,
    WaveSawtooth = V2MIDI::CC::Controller104,
    WaveSquare   = V2MIDI::CC::Controller105,

    EnvelopeAttack  = V2MIDI::CC::SoundController4,
    EnvelopeDecay   = V2MIDI::CC::SoundController6,
    EnvelopeRelease = V2MIDI::CC::SoundController3,

    VibratoRate  = V2MIDI::CC::SoundController7,
    VibratoDepth = V2MIDI::CC::SoundController8,
    Portamento   = V2MIDI::CC::PortamentoTime,
    Distortion   = V2MIDI::CC::GeneralPurpose5,

    Colour     = V2MIDI::CC::Controller14,
    Saturation = V2MIDI::CC::Controller15,
    Brightness = V2MIDI::CC::Controller89,
    Rainbow    = V2MIDI::CC::Controller90,
  };

  static constexpr struct {
    uint8_t start;
    uint8_t count;
  } Notes{
    .start{V2MIDI::A(-1)},
    .count{88},
  };

  Configuration config{ConfigurationDefault};

  void stop() {
    Synth.stop();
    Synth.resetParameters();

    _play   = {};
    _volume = 100;
    _playing.reset();

    _rainbow = 0;
    _led.h   = (float)config.color.h / 127.f * 360.f;
    _led.s   = (float)config.color.s / 127.f;
    _led.v   = (float)config.color.v / 127.f;

    LED.setHSV(V2Colour::Orange, 1, 0.25);
    LEDExt.reset();

    stopMIDIFile();
    LED.reset();
    LED.setHSV(V2Colour::Orange, 1, 0.25);
    LEDExt.reset();
  }

private:
  V2Music::ForcedStop _force;

  struct {
    uint32_t usec{};
    bool     notes{};
  } _timeout;

  float _rainbow{};

  // LED color.
  struct {
    float h;
    float s;
    float v;
  } _led{};

  struct {
    uint8_t note{};
    uint8_t velocity{};
    uint8_t aftertouch{};
    int16_t pitchbend{};
  } _play;

  uint8_t                       _volume{100};
  V2Music::Playing<Notes.count> _playing;

  virtual void stopMIDIFile();

  void handleReset() override {
    _timeout = {};
    stop();
    _force.reset();
    Synth.reset();
    Codec.adjustSamplerate(config.tuning);
    Codec.setGain(config.gain);
  }

  void handleLoop() override {
    runTimeout();
  }

  void touchTimeout() {
    _timeout.usec  = V2Base::getUsec();
    _timeout.notes = true;
  }

  void runTimeout() {
    if (_timeout.usec == 0)
      return;

    if (V2Base::getUsecSince(_timeout.usec) < 180 * 1000 * 1000)
      return;

    if (_timeout.notes) {
      stop();
      _timeout.notes = false;
    }

    if (V2Base::getUsecSince(_timeout.usec) < 240 * 1000 * 1000)
      return;

    reset();
    _timeout.usec = 0;
  }

  void light(uint8_t note, float fraction) {
    const float brightness = 0.2f + (0.8f * fraction);
    LEDExt.setHSV(_led.h, _led.s, _led.v * brightness);
    led.flash(0.03, 0.3);
  }

  float adjustVolume(float fraction) const {
    if (_volume < 100) {
      const float range = (float)_volume / 100.f;
      return fraction * range;
    }

    const float range = (float)(_volume - 100) / 27.f;
    return powf(fraction, 1 - (0.5f * range));
  }

  void play(uint8_t channel, uint8_t note, uint8_t velocity) {
    if (note < Notes.start || note > (Notes.start + Notes.count - 1))
      return;

    _playing.update(note, velocity);

    // Restore previous note.
    if (velocity == 0) {
      uint8_t n;
      uint8_t v;
      if (_playing.getLast(n, v)) {
        note     = n;
        velocity = v;
      }
    }

    _play.note     = note;
    _play.velocity = velocity;
    playUpdate();
  }

  void playUpdate() {
    if (_play.note == 0)
      return;

    touchTimeout();

    if (_play.velocity == 0) {
      Synth.stop();
      _play = {};
      LEDExt.reset();
      return;
    }

    float frequency = V2Music::Frequency::fromNote(_play.note);
    if (_play.pitchbend < 0) {
      const float pitchbend      = (float)_play.pitchbend / -8192.f;
      const float nextFrequency  = V2Music::Frequency::fromNote(_play.note - 2);
      const float deltaFrequency = nextFrequency - frequency;
      frequency += deltaFrequency * pitchbend;

    } else if (_play.pitchbend > 0) {
      const float pitchbend      = (float)_play.pitchbend / 8191.f;
      const float nextFrequency  = V2Music::Frequency::fromNote(_play.note + 2);
      const float deltaFrequency = nextFrequency - frequency;
      frequency += deltaFrequency * pitchbend;
    }

    const float volume = _play.aftertouch > 0 ? (float)_play.aftertouch / 127.f : (float)_play.velocity / 127.f;
    Synth.play(frequency, adjustVolume(volume));

    light(_play.note, volume);
  }

  void allNotesOff() {
    if (_force.trigger()) {
      reset();
      return;
    }

    stop();
  }

  void handleNote(uint8_t channel, uint8_t note, uint8_t velocity) override {
    if (channel > 0)
      return;

    play(channel, note, velocity);
  }

  void handleNoteOff(uint8_t channel, uint8_t note, uint8_t velocity) override {
    if (channel > 0)
      return;

    handleNote(channel, note, 0);
  }

  void handleAftertouchChannel(uint8_t channel, uint8_t pressure) override {
    if (channel > 0)
      return;

    _play.aftertouch = pressure;
    playUpdate();
  }

  void handlePitchBend(uint8_t channel, int16_t value) override {
    if (channel > 0)
      return;

    _play.pitchbend = value;
    playUpdate();
  }

  void handleControlChange(uint8_t channel, uint8_t controller, uint8_t value) override {
    if (channel > 0)
      return;

    touchTimeout();

    switch (controller) {
      case V2MIDI::CC::AllSoundOff:
      case V2MIDI::CC::AllNotesOff:
        allNotesOff();
        break;

      case (uint8_t)CC::Volume:
        _volume = value;
        if (_play.velocity > 0)
          playUpdate();
        break;

      case (uint8_t)CC::WaveSine:
        Synth.wave.sine.setTarget((float)value / 127.f);
        break;

      case (uint8_t)CC::WaveTriangle:
        Synth.wave.triangle.setTarget((float)value / 127.f);
        break;

      case (uint8_t)CC::WaveSawtooth:
        Synth.wave.sawtooth.setTarget((float)value / 127.f);
        break;

      case (uint8_t)CC::WaveSquare:
        Synth.wave.square.setTarget((float)value / 127.f);
        break;

      case (uint8_t)CC::EnvelopeAttack:
        Synth.envelope.attack = (float)value / 127.f;
        break;

      case (uint8_t)CC::EnvelopeDecay:
        Synth.envelope.decay = (float)value / 127.f;
        break;

      case (uint8_t)CC::EnvelopeRelease:
        Synth.envelope.release = (float)value / 127.f;
        break;

      case (uint8_t)CC::VibratoRate:
        Synth.setVibrato((float)value / 127.f);
        break;

      case (uint8_t)CC::VibratoDepth:
        Synth.vibrato.depth = (float)value / 127.f;
        break;

      case (uint8_t)CC::Portamento:
        Synth.portamento = (float)value / 127.f;
        break;

      case (uint8_t)CC::Distortion:
        Synth.setDistortion((float)value / 127.f);
        break;

      case (uint8_t)CC::Colour:
        _led.h = (float)value / 127.f * 360.f;
        break;

      case (uint8_t)CC::Saturation:
        _led.s = (float)value / 127.f;
        break;

      case (uint8_t)CC::Brightness:
        _led.v = (float)value / 127.f;
        if (_rainbow > 0.f)
          LEDExt.rainbow(1, 4.5f - (_rainbow * 4.f), _led.v);
        break;

      case (uint8_t)CC::Rainbow:
        _rainbow = (float)value / 127.f;
        if (_rainbow <= 0.f)
          LEDExt.reset();
        else
          LEDExt.rainbow(1, 4.5f - (_rainbow * 4.f), _led.v);
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
      setting["title"]   = "Amplifier";
      setting["label"]   = "Gain";
      setting["text"]    = "dB";
      setting["min"]     = -120;
      setting["max"]     = 24;
      setting["default"] = ConfigurationDefault.gain;
      setting["path"]    = "gain";
    }
    {
      JsonObject setting = json.add<JsonObject>();
      setting["type"]    = "number";
      setting["title"]   = "Synthesizer";
      setting["label"]   = "Tuning";
      setting["text"]    = "Cent";
      setting["min"]     = -100;
      setting["max"]     = 100;
      setting["default"] = ConfigurationDefault.tuning;
      setting["path"]    = "tuning";
    }

    {
      JsonObject setting = json.add<JsonObject>();
      setting["type"]    = "color";
      setting["title"]   = "Light";
      setting["path"]    = "color";
    }
  }

  void exportConfiguration(JsonObject json) override {
    json["#gain"] = "Gain / Volume (-120 .. 24 dB)";
    json["gain"]  = config.gain;

    json["#tuning"] = "Frequency adjustment in cents (-100 .. 100)";
    json["tuning"]  = config.tuning;

    json["#color"]    = "The LED color. Hue, saturation, brightness, 0..127";
    JsonArray jsonLed = json["color"].to<JsonArray>();
    jsonLed.add(config.color.h);
    jsonLed.add(config.color.s);
    jsonLed.add(config.color.v);
  }

  void importConfiguration(JsonObject json) override {
    if (!json["gain"].isNull()) {
      config.gain = json["gain"];
      if (config.gain < -120.f)
        config.gain = -120;

      if (config.gain > 24.f)
        config.gain = 24;

      Codec.setGain(config.gain);
    }

    if (!json["tuning"].isNull()) {
      config.tuning = json["tuning"];
      if (config.tuning < -100.f)
        config.tuning = -100;

      if (config.tuning > 100.f)
        config.tuning = 100;

      Codec.adjustSamplerate(config.tuning);
    }

    JsonArray jsonLed = json["color"];
    if (jsonLed) {
      uint8_t color = jsonLed[0];
      if (color > 127)
        color = 127;
      config.color.h = color;
      _led.h         = (float)color / 127.f * 360.f;

      uint8_t saturation = jsonLed[1];
      if (saturation > 127)
        saturation = 127;
      config.color.s = saturation;
      _led.s         = (float)saturation / 127.f;

      uint8_t brightness = jsonLed[2];
      if (brightness > 127)
        brightness = 127;
      config.color.v = brightness;
      _led.v         = (float)brightness / 127.f;
    }
  }

  void exportInput(JsonObject json) override {
    JsonArray jsonControllers = json["controllers"].to<JsonArray>();
    {
      JsonObject jsonController = jsonControllers.add<JsonObject>();
      jsonController["name"]    = "Volume";
      jsonController["number"]  = (uint8_t)CC::Volume;
      jsonController["value"]   = _volume;
    }
    {
      JsonObject jsonController = jsonControllers.add<JsonObject>();
      jsonController["name"]    = "Sine";
      jsonController["number"]  = (uint8_t)CC::WaveSine;
      jsonController["value"]   = (uint8_t)(Synth.wave.sine * 127.f);
    }
    {
      JsonObject jsonController = jsonControllers.add<JsonObject>();
      jsonController["name"]    = "Triangle";
      jsonController["number"]  = (uint8_t)CC::WaveTriangle;
      jsonController["value"]   = (uint8_t)(Synth.wave.triangle * 127.f);
    }
    {
      JsonObject jsonController = jsonControllers.add<JsonObject>();
      jsonController["name"]    = "Sawtooth";
      jsonController["number"]  = (uint8_t)CC::WaveSawtooth;
      jsonController["value"]   = (uint8_t)(Synth.wave.sawtooth * 127.f);
    }
    {
      JsonObject jsonController = jsonControllers.add<JsonObject>();
      jsonController["name"]    = "Square";
      jsonController["number"]  = (uint8_t)CC::WaveSquare;
      jsonController["value"]   = (uint8_t)(Synth.wave.square * 127.f);
    }
    {
      JsonObject jsonController = jsonControllers.add<JsonObject>();
      jsonController["name"]    = "Attack Time";
      jsonController["number"]  = (uint8_t)CC::EnvelopeAttack;
      jsonController["value"]   = (uint8_t)(Synth.envelope.attack * 127.f);
    }
    {
      JsonObject jsonController = jsonControllers.add<JsonObject>();
      jsonController["name"]    = "Decay Time";
      jsonController["number"]  = (uint8_t)CC::EnvelopeDecay;
      jsonController["value"]   = (uint8_t)(Synth.envelope.decay * 127.f);
    }
    {
      JsonObject jsonController = jsonControllers.add<JsonObject>();
      jsonController["name"]    = "Release Time";
      jsonController["number"]  = (uint8_t)CC::EnvelopeRelease;
      jsonController["value"]   = (uint8_t)(Synth.envelope.release * 127.f);
    }
    {
      JsonObject jsonController = jsonControllers.add<JsonObject>();
      jsonController["name"]    = "Vibrato Rate";
      jsonController["number"]  = (uint8_t)CC::VibratoRate;
      jsonController["value"]   = (uint8_t)(Synth.getVibrato() * 127.f);
    }
    {
      JsonObject jsonController = jsonControllers.add<JsonObject>();
      jsonController["name"]    = "Vibrato Depth";
      jsonController["number"]  = (uint8_t)CC::VibratoDepth;
      jsonController["value"]   = (uint8_t)(Synth.vibrato.depth * 127.f);
    }
    {
      JsonObject jsonController = jsonControllers.add<JsonObject>();
      jsonController["name"]    = "Portamento";
      jsonController["number"]  = (uint8_t)CC::Portamento;
      jsonController["value"]   = (uint8_t)(Synth.portamento * 127.f);
    }
    {
      JsonObject jsonController = jsonControllers.add<JsonObject>();
      jsonController["name"]    = "Distortion";
      jsonController["number"]  = (uint8_t)CC::Distortion;
      jsonController["value"]   = (uint8_t)(Synth.getDistortion() * 127.f);
    }
    {
      JsonObject jsonController = jsonControllers.add<JsonObject>();
      jsonController["name"]    = "Hue";
      jsonController["number"]  = (uint8_t)CC::Colour;
      jsonController["value"]   = (uint8_t)(_led.h / 360.f * 127.f);
    }
    {
      JsonObject jsonController = jsonControllers.add<JsonObject>();
      jsonController["name"]    = "Saturation";
      jsonController["number"]  = (uint8_t)CC::Saturation;
      jsonController["value"]   = (uint8_t)(_led.s * 127.f);
    }
    {
      JsonObject jsonController = jsonControllers.add<JsonObject>();
      jsonController["name"]    = "Brightness";
      jsonController["number"]  = (uint8_t)CC::Brightness;
      jsonController["value"]   = (uint8_t)(_led.v * 127.f);
    }
    {
      JsonObject jsonController = jsonControllers.add<JsonObject>();
      jsonController["name"]    = "Rainbow";
      jsonController["number"]  = (uint8_t)CC::Rainbow;
      jsonController["value"]   = (uint8_t)(_rainbow * 127.f);
    }

    JsonObject jsonPitchbend = json["pitchbend"].to<JsonObject>();
    jsonPitchbend["value"]   = _play.pitchbend;

    JsonObject jsonAftertouch = json["aftertouch"].to<JsonObject>();
    jsonAftertouch["value"]   = _play.aftertouch;

    JsonObject jsonChromatic = json["chromatic"].to<JsonObject>();
    jsonChromatic["start"]   = Notes.start;
    jsonChromatic["count"]   = Notes.count;
  }

  virtual void exportSystemMIDIFile(JsonObject json);

  void exportSystem(JsonObject json) override {
    JsonObject jsonPower       = json["power"].to<JsonObject>();
    jsonPower["voltage"]       = serialized(String(Power.getVoltage(), 1));
    jsonPower["interruptions"] = Power.getInterruptions();

    JsonObject jsonCodec = json["codec"].to<JsonObject>();
    jsonCodec["load"]    = serialized(String(Codec.getLoad(), 3));

    exportSystemMIDIFile(json);
  }
} Device;

// Dispatch MIDI packets.
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

// Dispatch Link packets.
static class Link : public V2Link {
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

static class MIDIFile : public V2MIDI::File::Tracks {
public:
  constexpr MIDIFile() : V2MIDI::File::Tracks(MIDISong) {}

  bool handleSend(uint16_t track, V2MIDI::Packet* packet) {
    Device.dispatch(&Device.usb.midi, packet);
    return true;
  }

  void handleStateChange(V2MIDI::File::Tracks::State state) {
    switch (state) {
      case V2MIDI::File::Tracks::State::Stop:
        Device.stop();
        break;
    }
  }
} MIDIFile;

void Device::exportSystemMIDIFile(JsonObject json) {
  JsonObject jsonTrack = json["track"].to<JsonObject>();
  char       s[128];
  if (MIDIFile.copyTag(V2MIDI::File::Event::Meta::Title, s, sizeof(s)) > 0)
    jsonTrack["title"] = s;

  if (MIDIFile.copyTag(V2MIDI::File::Event::Meta::Copyright, s, sizeof(s)) > 0)
    jsonTrack["creator"] = s;
}

void Device::stopMIDIFile() {
  MIDIFile.stop();
}

static class Button : public V2Buttons::Button {
public:
  constexpr Button() : V2Buttons::Button(&_config, PIN_BUTTON) {}

private:
  const V2Buttons::Config _config{.clickUsec{200 * 1000}, .holdUsec{500 * 1000}};

  void handleHold(uint8_t count) override {
    MIDIFile.play();
    LED.rainbow();
    LEDExt.rainbow(1, 2);
  }

  void handleClick(uint8_t count) override {
    Device.stop();
  }
} Button;

void setup() {
  Serial.begin(9600);

  LED.begin();
  LED.setMaxBrightness(0.75);

  LEDExt.begin();
  LEDExt.setMaxBrightness(0.75);

  Link.begin();

  // Set the SERCOM interrupt priority, it requires a stable ~300 kHz interrupt
  // frequency. This needs to be after begin().
  setSerialPriority(&SerialPlug, 2);
  setSerialPriority(&SerialSocket, 2);

  Wire.begin();
  Wire.setClock(100000);
  Wire.setTimeout(1);

  ADC.begin();
  ADC.addChannel(V2Base::Analog::ADC::getChannel(PIN_VOLTAGE_SENSE));

  // Disable the amplifier.
  digitalWrite(PIN_CODEC_ENABLE, HIGH);
  pinMode(PIN_CODEC_ENABLE, OUTPUT);

  // Mute the amplifier.
  digitalWrite(PIN_CODEC_MUTE, LOW);
  pinMode(PIN_CODEC_MUTE, OUTPUT);

  Codec.begin([](Adafruit_ZeroDMA* dma) { Codec.fillNextBuffer(); });
  Synth.begin();

  Button.begin();
  Device.begin();
  Device.reset();
}

void loop() {
  LED.loop();
  LEDExt.loop();

  MIDI.loop();
  Link.loop();
  MIDIFile.loop();
  V2Buttons::loop();
  Power.loop();
  Device.loop();

  if (Device.idle())
    Device.sleep();
}
