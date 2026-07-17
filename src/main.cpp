#include <Arduino.h>
#include <LTE.h>

// ===== SIMの台紙を見てここを書き換える =====
#define APP_LTE_APN       "vmobile.jp"             // APN(接続先名)。IoTサービスSIMなら台紙記載の値に変更
#define APP_LTE_USER_NAME "IIJ"               // 認証ユーザー名
#define APP_LTE_PASSWORD  "IIJ"                   // 認証パスワード
#define APP_LTE_AUTH_TYPE (LTE_NET_AUTHTYPE_CHAP) // 認証方式。ダメならLTE_NET_AUTHTYPE_PAPも試す
#define APP_LTE_IP_TYPE   (LTE_NET_IPTYPE_V4V6)   // IPv4/IPv6両対応で要求
#define APP_LTE_RAT       (LTE_NET_RAT_CATM)      // 無線方式=LTE-M(Cat-M1)。このボードはこれ固定
// ==========================================

LTE lteAccess;
LTEScanner scannerNetworks;

void setup() {
  Serial.begin(115200);
  // シリアルモニタが開くまで待つ
  while (!Serial) { ; }
  Serial.println("LTE attach test");

  while (true) {
    Serial.println("modem starting...");
    if (lteAccess.begin() != LTE_SEARCHING) {
      Serial.println("ERROR: modem start failed. Retry in 5s.");
      lteAccess.shutdown();
      delay(5000);
      continue;
    }

    Serial.println("attaching to network...");
    if (lteAccess.attach(APP_LTE_RAT,
                        APP_LTE_APN,
                        APP_LTE_USER_NAME,
                        APP_LTE_PASSWORD,
                        APP_LTE_AUTH_TYPE,
                        APP_LTE_IP_TYPE) == LTE_READY) {
      Serial.println("SUCCESS: attach succeeded!");
      break;
    }

    Serial.println("ERROR: attach failed. Shutdown and retry");
    lteAccess.shutdown();
    delay(5000);
  }
}

void loop() {
  Serial.print("Carrier: ");
  Serial.println(scannerNetworks.getCurrentCarrier());
  Serial.print("Signal strength: ");
  Serial.print(scannerNetworks.getSignalStrength());
  Serial.println(" [dBm]");
  delay(5000);
}
