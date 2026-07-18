#include <Arduino.h>

namespace {
constexpr uint8_t SERVO_SIGNAL_PIN = PIN_D09;

// FT5335M specification: 900-2100 us, 1500 us center.
// The first test intentionally uses only a small range around center.
constexpr uint16_t CENTER_PULSE_US = 1500;
constexpr uint16_t CLOCKWISE_TEST_PULSE_US = 1350;
constexpr uint16_t COUNTERCLOCKWISE_TEST_PULSE_US = 1650;
constexpr uint32_t SERVO_FRAME_US = 20000;  // Standard 50 Hz servo signal.

void sendServoFrame(uint16_t pulseWidthUs)
{
  digitalWrite(SERVO_SIGNAL_PIN, HIGH);
  delayMicroseconds(pulseWidthUs);
  digitalWrite(SERVO_SIGNAL_PIN, LOW);
  delayMicroseconds(SERVO_FRAME_US - pulseWidthUs);
}

void holdPosition(uint16_t pulseWidthUs, uint32_t durationMs)
{
  const uint32_t frameCount = durationMs * 1000UL / SERVO_FRAME_US;
  for (uint32_t frame = 0; frame < frameCount; ++frame) {
    sendServoFrame(pulseWidthUs);
  }
}

void moveAndReport(const char* label, uint16_t pulseWidthUs, uint32_t durationMs)
{
  Serial.print(label);
  Serial.print(": ");
  Serial.print(pulseWidthUs);
  Serial.println(" us");
  holdPosition(pulseWidthUs, durationMs);
}
}  // namespace

void setup()
{
  pinMode(SERVO_SIGNAL_PIN, OUTPUT);
  digitalWrite(SERVO_SIGNAL_PIN, LOW);

  Serial.begin(115200);
  Serial.println("FEETECH FT5335M servo test");
  Serial.println("WARNING: Use an external 6-8.4 V supply rated for servo stall current.");
  Serial.println("WARNING: Share GND, but do not connect servo V+ to Spresense.");
  Serial.println("Remove the horn/load and keep clear of the servo shaft.");
  Serial.println("The one-time test starts automatically in 5 seconds.");
  delay(5000);

  moveAndReport("Center", CENTER_PULSE_US, 2000);
  moveAndReport("Small clockwise movement", CLOCKWISE_TEST_PULSE_US, 2000);
  moveAndReport("Return to center", CENTER_PULSE_US, 1000);
  moveAndReport("Small counterclockwise movement", COUNTERCLOCKWISE_TEST_PULSE_US, 2000);
  moveAndReport("Return to center", CENTER_PULSE_US, 2000);

  digitalWrite(SERVO_SIGNAL_PIN, LOW);
  Serial.println("Servo test complete. PWM output stopped.");
  Serial.println("Press RESET to run the test again.");
}

void loop()
{
  delay(1000);
}
