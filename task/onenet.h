#ifndef _ONENET_H_
#define _ONENET_H_

/* ===== 产品/设备信息 =====
   真实凭据放在 app_secrets.h（已 .gitignore），模板见 app_secrets.example.h。
   Token 有有效期，过期后云端会拒绝登录（串口打印 "link fail, auth"），
   到控制台重新生成即可。注意：Token 必须用 MQTT 设备的密钥生成，
   它和 API 的 sec_key 不是同一把（API 那把算出来的会被 MQTT 拒绝）。 */
#include "app_secrets.h"

/* 物模型主题：上报属性 / 接收下发 */
#define ONENET_TOPIC_PROP_POST	"$sys/" ONENET_PROID "/" ONENET_DEVID "/thing/property/post"
#define ONENET_TOPIC_PROP_SET	"$sys/" ONENET_PROID "/" ONENET_DEVID "/thing/property/set"
/* 属性设置回执：收到 property/set 后回复，控制台才会显示"设置成功" */
#define ONENET_TOPIC_PROP_SET_REPLY	"$sys/" ONENET_PROID "/" ONENET_DEVID "/thing/property/set_reply"

/* 登录 OneNet（MQTT CONNECT）：1-成功 0-失败 */
_Bool OneNet_DevLink(void);

/* 订阅主题（登录成功后调用；clean_session=1，断线重连后必须重新订阅） */
void OneNet_Subscribe(const char *topics[], unsigned char topic_cnt);

/* 处理平台下发数据（收到 +IPD 数据后调用） */
void OneNet_RevPro(unsigned char *cmd);

/* 发布任意主题 */
void OneNet_Publish(const char *topic, const char *msg);

/* 把全部属性打包上报到物模型 property/post（按上传周期调用） */
void OneNet_ReportProps(void);

#endif
