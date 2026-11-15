/**
 * @file    app_secrets.example.h
 * @brief   私有凭据模板：复制为 app_secrets.h 后填入自己的值
 *
 *   copy task/app_secrets.example.h task/app_secrets.h
 *
 * app_secrets.h 已在 .gitignore 中忽略，不会被提交。
 */
#ifndef __APP_SECRETS_H
#define __APP_SECRETS_H

/* ---- WiFi（ESP8266 STA 模式） ---- */
#define ESP8266_WIFI_SSID       "your-wifi-ssid"
#define ESP8266_WIFI_PASS       "your-wifi-password"

/* ---- OneNet 物模型 MQTT 设备 ----
 * 产品 ID / 设备名在 OneNet 控制台查看；
 * Token 必须用 MQTT 设备的密钥生成（不是 API 的 sec_key），
 * 有效期较长但过期后需重新生成。 */
#define ONENET_PROID            "your-product-id"
#define ONENET_DEVID            "your-device-id"
#define ONENET_TOKEN            "version=2018-10-31&res=products%2Fyour-product-id%2Fdevices%2Fyour-device-id&et=0000000000&method=md5&sign=your-signature"

#endif /* __APP_SECRETS_H */
