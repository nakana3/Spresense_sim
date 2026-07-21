#include <Arduino.h>
#include <ArduinoMqttClient.h>
#include <LTE.h>
#include <RTC.h>

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "secrets.h"

namespace {
// IIJmio / vmobile.jp (the same settings that passed the LTE HTTP test).
constexpr char APN[] = "vmobile.jp";
constexpr char APN_USER[] = "IIJ";
constexpr char APN_PASSWORD[] = "IIJ";

// D9 is the signal pin that passed the standalone FT5335M movement test. The
// LTE disconnect was ultimately isolated to the phone's application PING, not
// to this pin or to PWM generation.
constexpr uint8_t SERVO_SIGNAL_PIN = PIN_D09;
constexpr uint16_t SERVO_CENTER_US = 1500;
constexpr uint16_t SERVO_LEFT_US = 1350;
constexpr uint16_t SERVO_RIGHT_US = 1650;
constexpr uint16_t SERVO_MIN_US = 900;
constexpr uint16_t SERVO_MAX_US = 2100;
constexpr uint32_t SERVO_FRAME_US = 20000;

// The phone sends a low-rate application heartbeat while armed. LTE-M can
// occasionally delay packets for several seconds, so use a 30-second lease.
// A real MQTT disconnect still stops PWM immediately in loop().
constexpr uint32_t ARM_HANDSHAKE_TIMEOUT_MS = 30000;
constexpr uint32_t ACTIVE_COMMAND_TIMEOUT_MS = 30000;
constexpr uint32_t RECONNECT_INTERVAL_MS = 5000;
constexpr uint32_t STATE_HEARTBEAT_MS = 10000;
constexpr uint8_t MQTT_FAILURES_BEFORE_LTE_RESET = 3;
constexpr size_t MAX_COMMAND_LENGTH = 32;

LTE lteAccess;
LTETLSClient tlsClient;
MqttClient mqttClient(tlsClient);

bool configurationIsReady = false;
bool modemStarted = false;
bool servoOutputActive = false;
bool armed = false;
uint16_t targetPulseUs = SERVO_CENTER_US;
uint32_t nextServoFrameUs = 0;
uint32_t lastCommandMs = 0;
uint32_t nextReconnectAtMs = 0;
uint32_t lastStatePublishMs = 0;
uint8_t consecutiveMqttFailures = 0;
bool forceLteReset = false;
bool retainedStateCleared = false;

bool stateDirty = false;
const char* pendingState = "boot";
const char* positionName = "stopped";

bool deadlineReached(uint32_t now, uint32_t deadline)
{
  return static_cast<int32_t>(now - deadline) >= 0;
}

void stopServoOutput()
{
  pinMode(SERVO_SIGNAL_PIN, OUTPUT);
  digitalWrite(SERVO_SIGNAL_PIN, LOW);
}

void serviceServoOutput()
{
  if (!servoOutputActive) {
    return;
  }

  const uint32_t now = micros();
  if (static_cast<int32_t>(now - nextServoFrameUs) < 0) {
    return;
  }

  // Generate one standard RC-servo frame in software. The earlier standalone
  // FT5335M test used the same pulse method successfully. Avoiding the NuttX
  // hardware-PWM device keeps this test independent of the PWM driver.
  digitalWrite(SERVO_SIGNAL_PIN, HIGH);
  delayMicroseconds(targetPulseUs);
  digitalWrite(SERVO_SIGNAL_PIN, LOW);

  nextServoFrameUs += SERVO_FRAME_US;
  if (static_cast<int32_t>(micros() - nextServoFrameUs) >= 0) {
    nextServoFrameUs = micros() + SERVO_FRAME_US;
  }
}

void queueState(const char* state)
{
  pendingState = state;
  stateDirty = true;
}

void startFailsafe(const char* reason)
{
  armed = false;
  servoOutputActive = false;
  stopServoOutput();
  positionName = "stopped";

  Serial.print("FAILSAFE: ");
  Serial.println(reason);
  queueState(reason);
}

bool startServoOutput(uint16_t pulseWidthUs)
{
  if (pulseWidthUs < SERVO_MIN_US || pulseWidthUs > SERVO_MAX_US) {
    Serial.println("SERVO ERROR: pulse width is outside the safe range.");
    return false;
  }
  nextServoFrameUs = micros();
  return true;
}

bool hasValue(const char* value)
{
  return value != nullptr && value[0] != '\0';
}

bool validateConfiguration()
{
  bool valid = true;

  if (!hasValue(MQTT_HOST)) {
    Serial.println("CONFIG ERROR: MQTT_HOST is empty in include/secrets.h");
    valid = false;
  }
  if (!hasValue(MQTT_USERNAME) || !hasValue(MQTT_PASSWORD)) {
    Serial.println("CONFIG ERROR: MQTT username/password is empty in include/secrets.h");
    valid = false;
  }
  if (!hasValue(MQTT_ROOT_CA_PEM)) {
    Serial.println("CONFIG ERROR: MQTT_ROOT_CA_PEM is empty in include/secrets.h");
    valid = false;
  }
  if (!hasValue(MQTT_COMMAND_TOPIC) || !hasValue(MQTT_STATE_TOPIC)) {
    Serial.println("CONFIG ERROR: MQTT topics are empty in include/secrets.h");
    valid = false;
  }

  return valid;
}

void configureMqttWill()
{
  // HiveMQ publishes this message if the LTE/TLS connection disappears.
  // This gives the phone an event-driven OFFLINE indication without a timer.
  mqttClient.beginWill(MQTT_STATE_TOPIC, false, 0);
  mqttClient.print("{\"state\":\"offline\",\"armed\":false,\"position\":\"stopped\",\"pulse_us\":0}");
  mqttClient.endWill();
}

bool setClockFromLTE()
{
  Serial.println("Setting clock from LTE network...");
  RTC.begin();

  const uint32_t timeoutAt = millis() + 30000;
  unsigned long epoch = 0;
  while ((epoch = lteAccess.getTime()) == 0 && !deadlineReached(millis(), timeoutAt)) {
    delay(1000);
  }

  if (epoch == 0) {
    Serial.println("Could not get network time; TLS certificate cannot be verified.");
    return false;
  }

  RtcTime networkTime(epoch);
  RTC.setTime(networkTime);
  Serial.println("RTC is ready for TLS certificate verification.");
  return true;
}

bool connectLTE()
{
  Serial.println("Starting LTE modem...");

  if (modemStarted) {
    mqttClient.stop();
    tlsClient.stop();
    lteAccess.shutdown();
    delay(1000);
  }

  const LTEModemStatus beginStatus = lteAccess.begin();
  Serial.print("LTE begin status: ");
  Serial.println(static_cast<int>(beginStatus));
  if (beginStatus != LTE_SEARCHING) {
    Serial.println("LTE modem did not enter LTE_SEARCHING.");
    modemStarted = false;
    return false;
  }
  modemStarted = true;

  Serial.print("Attaching to APN: ");
  Serial.println(APN);
  const LTEModemStatus attachStatus = lteAccess.attach(
      LTE_NET_RAT_CATM,
      APN,
      APN_USER,
      APN_PASSWORD,
      LTE_NET_AUTHTYPE_CHAP,
      LTE_NET_IPTYPE_V4V6,
      true);

  Serial.print("LTE attach status: ");
  Serial.println(static_cast<int>(attachStatus));
  if (attachStatus != LTE_READY) {
    Serial.println("LTE attach failed.");
    return false;
  }

  Serial.print("LTE ready. IP address: ");
  Serial.println(lteAccess.getIPAddress());
  return setClockFromLTE();
}

void publishState()
{
  if (!stateDirty || !mqttClient.connected()) {
    return;
  }

  char message[160];
  snprintf(message,
           sizeof(message),
           "{\"state\":\"%s\",\"armed\":%s,\"position\":\"%s\",\"pulse_us\":%u}",
           pendingState,
           armed ? "true" : "false",
           positionName,
           static_cast<unsigned int>(servoOutputActive ? targetPulseUs : 0));

  // Never retain device state: an old "online" message must not enable the
  // phone controls while the LTE device is disconnected.
  // Keep device-side MQTT traffic at QoS 0. The Spresense LTE TLS transport is
  // more stable without the blocking QoS 1 PUBACK exchange; the phone still
  // confirms commands through these explicit state messages.
  if (mqttClient.beginMessage(MQTT_STATE_TOPIC, false, 0, false) == 0) {
    return;
  }
  mqttClient.print(message);
  if (mqttClient.endMessage() != 0) {
    stateDirty = false;
    lastStatePublishMs = millis();
    Serial.print("State: ");
    Serial.println(message);
  }
}

void discardIncomingMessage()
{
  while (mqttClient.available()) {
    mqttClient.read();
  }
}

void trimCommand(char* command)
{
  char* first = command;
  while (*first != '\0' && isspace(static_cast<unsigned char>(*first))) {
    ++first;
  }
  if (first != command) {
    memmove(command, first, strlen(first) + 1);
  }

  size_t length = strlen(command);
  while (length > 0 && isspace(static_cast<unsigned char>(command[length - 1]))) {
    command[--length] = '\0';
  }

  for (size_t index = 0; index < length; ++index) {
    command[index] = static_cast<char>(toupper(static_cast<unsigned char>(command[index])));
  }
}

void onMqttMessage(int messageSize)
{
  if (mqttClient.messageTopic() != MQTT_COMMAND_TOPIC) {
    discardIncomingMessage();
    return;
  }

  // Never execute a command retained by the broker. This prevents an old ARM
  // command from running automatically after a reconnect or power cycle.
  if (mqttClient.messageRetain()) {
    Serial.println("Rejected retained MQTT command.");
    discardIncomingMessage();
    queueState("rejected_retained");
    return;
  }

  if (messageSize <= 0 || messageSize > static_cast<int>(MAX_COMMAND_LENGTH)) {
    Serial.println("Rejected oversized or empty MQTT command.");
    discardIncomingMessage();
    queueState("rejected_size");
    return;
  }

  char command[MAX_COMMAND_LENGTH + 1];
  size_t length = 0;
  while (mqttClient.available()) {
    const int value = mqttClient.read();
    if (value >= 0 && length < MAX_COMMAND_LENGTH) {
      command[length++] = static_cast<char>(value);
    }
  }
  command[length] = '\0';
  trimCommand(command);

  if (strcmp(command, "PING") == 0) {
    // Keep the armed lease alive without flooding the serial monitor. MQTT's
    // own keep-alive remains active independently of this application message.
    if (armed) {
      lastCommandMs = millis();
    }
    return;
  }

  Serial.print("MQTT command: ");
  Serial.println(command);

  if (strcmp(command, "ARM") == 0) {
    armed = true;
    servoOutputActive = false;
    stopServoOutput();
    targetPulseUs = SERVO_CENTER_US;
    positionName = "stopped";
    lastCommandMs = millis();
    queueState("armed");
    return;
  }

  if (strcmp(command, "DISARM") == 0 || strcmp(command, "STOP") == 0) {
    startFailsafe("disarmed");
    return;
  }

  if (!armed) {
    Serial.println("Rejected motion command: ARM is required first.");
    queueState("rejected_not_armed");
    return;
  }

  if (strcmp(command, "LEFT") == 0) {
    targetPulseUs = SERVO_LEFT_US;
    positionName = "left";
  } else if (strcmp(command, "CENTER") == 0) {
    targetPulseUs = SERVO_CENTER_US;
    positionName = "center";
  } else if (strcmp(command, "RIGHT") == 0) {
    targetPulseUs = SERVO_RIGHT_US;
    positionName = "right";
  } else {
    Serial.println("Rejected unknown command.");
    queueState("rejected_unknown");
    return;
  }

  if (!startServoOutput(targetPulseUs)) {
    startFailsafe("servo_pwm_error");
    return;
  }
  servoOutputActive = true;
  lastCommandMs = millis();
  queueState("moving");
}

bool connectMQTT()
{
  Serial.print("Connecting securely to MQTT broker: ");
  Serial.println(MQTT_HOST);

  mqttClient.stop();
  tlsClient.stop();

  mqttClient.setId(MQTT_CLIENT_ID);
  mqttClient.setUsernamePassword(MQTT_USERNAME, MQTT_PASSWORD);
  mqttClient.setCleanSession(true);
  // A short keep-alive keeps the LTE TCP/TLS path active and verifies the
  // broker response before the mobile network can leave a stale connection.
  mqttClient.setKeepAliveInterval(15000);
  mqttClient.setConnectionTimeout(30000);
  mqttClient.onMessage(onMqttMessage);

  if (!mqttClient.connect(MQTT_HOST, MQTT_PORT)) {
    Serial.print("MQTT connection failed. Error: ");
    Serial.println(mqttClient.connectError());
    return false;
  }

  if (!mqttClient.subscribe(MQTT_COMMAND_TOPIC, 0)) {
    Serial.println("MQTT subscription failed.");
    mqttClient.stop();
    return false;
  }

  Serial.print("MQTT connected. Command topic: ");
  Serial.println(MQTT_COMMAND_TOPIC);

  // Clear state retained by older firmware versions once per boot. Repeating
  // this publish on every reconnect adds an unnecessary TLS write at the most
  // fragile point in the recovery sequence.
  if (!retainedStateCleared &&
      mqttClient.beginMessage(MQTT_STATE_TOPIC, 0UL, true, 0, false) != 0) {
    retainedStateCleared = mqttClient.endMessage() != 0;
  }

  queueState("online");
  publishState();
  return true;
}
}  // namespace

void setup()
{
  pinMode(SERVO_SIGNAL_PIN, OUTPUT);
  digitalWrite(SERVO_SIGNAL_PIN, LOW);

  Serial.begin(115200);
  delay(3000);

  Serial.println("Spresense LTE remote servo controller");
  Serial.println("Safety: ARM alone keeps PWM off; timeout/disconnect stops PWM immediately.");
  Serial.println("Servo signal D9 software PWM: 1350 / 1500 / 1650 us at 50 Hz.");

  stopServoOutput();

  configurationIsReady = validateConfiguration();
  if (!configurationIsReady) {
    Serial.println("Fill include/secrets.h, then upload again.");
    return;
  }

  configureMqttWill();
  // LTETLSClient copies this PEM into its own buffer. Configure it once rather
  // than freeing and reallocating the certificate on every MQTT reconnect.
  tlsClient.setCACert(MQTT_ROOT_CA_PEM);

  const bool lteReady = connectLTE();
  if (lteReady && connectMQTT()) {
    consecutiveMqttFailures = 0;
    nextReconnectAtMs = millis();
  } else {
    if (lteReady) {
      consecutiveMqttFailures = 1;
    }
    nextReconnectAtMs = millis() + RECONNECT_INTERVAL_MS;
  }
}

void loop()
{
  if (!configurationIsReady) {
    delay(1000);
    return;
  }

  serviceServoOutput();

  if (mqttClient.connected()) {
    mqttClient.poll();

    if (!mqttClient.connected()) {
      startFailsafe("mqtt_disconnected");
      // First retry only MQTT/TLS over the existing LTE attachment. Restarting
      // the modem after one transient TLS failure creates a long offline gap.
      forceLteReset = false;
      nextReconnectAtMs = millis() + RECONNECT_INTERVAL_MS;
      return;
    }

    const uint32_t commandTimeoutMs = servoOutputActive
                                          ? ACTIVE_COMMAND_TIMEOUT_MS
                                          : ARM_HANDSHAKE_TIMEOUT_MS;
    if (armed && millis() - lastCommandMs > commandTimeoutMs) {
      startFailsafe("command_timeout");
    }

    if (millis() - lastStatePublishMs >= STATE_HEARTBEAT_MS) {
      stateDirty = true;
    }

    publishState();
    // Keep the loop responsive enough to generate a 50 Hz software PWM frame.
    delay(1);
    return;
  }

  if (armed) {
    startFailsafe("mqtt_disconnected");
  }

  if (!deadlineReached(millis(), nextReconnectAtMs)) {
    delay(20);
    return;
  }

  bool lteReady = !forceLteReset && lteAccess.getStatus() == LTE_READY;
  if (!lteReady) {
    if (forceLteReset) {
      Serial.println("MQTT transport failed; restarting LTE modem before retry.");
    }
    lteReady = connectLTE();
    if (lteReady) {
      forceLteReset = false;
    }
  }

  if (lteReady && connectMQTT()) {
    consecutiveMqttFailures = 0;
    forceLteReset = false;
    nextReconnectAtMs = millis();
  } else {
    if (consecutiveMqttFailures < UINT8_MAX) {
      ++consecutiveMqttFailures;
    }
    Serial.print("Connection retry failed; consecutive MQTT failures: ");
    Serial.println(consecutiveMqttFailures);
    if (consecutiveMqttFailures >= MQTT_FAILURES_BEFORE_LTE_RESET) {
      forceLteReset = true;
      consecutiveMqttFailures = 0;
      Serial.println("Retry threshold reached; next attempt will restart LTE.");
    }
    nextReconnectAtMs = millis() + RECONNECT_INTERVAL_MS;
  }
}
