#pragma once

// HiveMQ Cloud > Cluster > Overview に表示される接続先を入力します。
static const char MQTT_HOST[] = "";
static const unsigned short MQTT_PORT = 8883;

// HiveMQ Cloud > Access Management で作成した機器用認証情報です。
static const char MQTT_USERNAME[] = "";
static const char MQTT_PASSWORD[] = "";

// 同じクラスター内で重複しない名前にします。
static const char MQTT_CLIENT_ID[] = "spresense-ft5335m-01";

// スマホ画面とSpresenseで同じトピックを使います。
static const char MQTT_COMMAND_TOPIC[] = "spresense/ft5335m-01/command";
static const char MQTT_STATE_TOPIC[] = "spresense/ft5335m-01/state";

// MQTT_HOSTのサーバー証明書を検証するルートCAをPEM形式で入れます。
// クラスターURLが分かった後、こちらで正しい証明書を設定します。
static const char MQTT_ROOT_CA_PEM[] = "";
