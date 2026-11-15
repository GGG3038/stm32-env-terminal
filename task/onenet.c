/**
 * @file    onenet.c
 * @brief   OneNet 物模型（MQTT）接口
 *
 * 相对原版的改动：
 *   1. 上报属性扩展为 temp / hum / r1~r4 / led / bl / alm / mode / mbok / up
 *   2. 订阅 $sys/{pid}/{dev}/thing/property/set，云端可下发控制
 *      （原版 OneNet_RevPro 是死代码：从未订阅，主循环也从不读接收缓存，
 *        所以云端下发的命令一条也收不到）
 *   3. 下发用 cJSON 统一解析：继电器 / LED / 背光 / 工作模式 / 阈值都能改
 *   4. 打印全部改成 ASCII（串口终端上 GB2312 中文会花屏）
 *
 * OneNet 物模型属性值必须是数字或布尔，所以温湿度发 "25.6" 这样的数值文本，
 * 不能发字符串。
 */
#include "stm32f1xx_hal.h"
#include "esp8266.h"
#include "onenet.h"
#include "MqttKit.h"
#include "Usart.h"
#include "usart_wifi.h"
#include "app_state.h"
#include "relay.h"
#include "power_mgr.h"
#include <string.h>
#include <stdio.h>
#include "cJSON.h"

/* 产品 ID / 设备名 / 主题见 onenet.h */

/* 上报 JSON 缓冲（只有云端任务用，放静态区不占任务栈） */
#define JSON_BUF_SIZE	384
static char s_json[JSON_BUF_SIZE];

/* ===================== JSON 拼装小工具 =====================
   返回新的写入位置；缓冲不够就原样返回（绝不越界）。
   逗号由 s_first 控制：第一项不加，之后的项前加一个。
   不能用 off 判断——off 是缓冲区偏移，前缀写完后就已经不为 0，
   会导致 "params":{,"temp":... 这种非法 JSON。 */
static uint8_t s_first = 1;

static uint16_t js_num(char *b, uint16_t cap, uint16_t off, const char *key, int32_t v)
{
	int n = snprintf(b + off, (size_t)(cap - off), "%s\"%s\":{\"value\":%ld}",
	                 s_first ? "" : ",", key, (long)v);

	if (n > 0 && (uint32_t)n < (uint32_t)(cap - off)) {
		s_first = 0;
		return (uint16_t)(off + n);
	}
	return off;
}

/* 只保留整数/布尔通用的写法。当前物模型里 temp/hum 是 int32，
   发 "27.0" 会被平台拒绝（code 2211: int32 type error），所以统一发整数。 */

/* ===================== 连接 ===================== */

//==========================================================
//	函数名称：	OneNet_DevLink
//	函数功能：	登录 OneNet（MQTT CONNECT + CONNACK）
//	返回值：	1-成功	0-失败
//==========================================================
_Bool OneNet_DevLink(void)
{
	MQTT_PACKET_STRUCTURE mqttPacket = {NULL, 0, 0, 0};
	unsigned char *dataPtr;
	_Bool status = 1;

	UsartPrintf(USART_DEBUG, "OneNet_DevLink (PROID:%s DEVID:%s)", ONENET_PROID, ONENET_DEVID);

	/* keepalive 必须 <= 1800s：OneNET 的合法范围是 10~1800，
	   超过 1800（例如 3600）服务器会直接断开 TCP 且不回 CONNACK，
	   现象就是串口一直打印 "IPD" not found。
	   低功耗模式下大部分时间在睡，所以取上限 1800，尽量少被踢 */
	if (MQTT_PacketConnect(ONENET_PROID, ONENET_TOKEN, ONENET_DEVID, 1800, 1,
	                       MQTT_QOS_LEVEL0, NULL, NULL, 0, &mqttPacket) == 0)
	{
		ESP8266_SendData(mqttPacket._data, mqttPacket._len);
		dataPtr = ESP8266_GetIPD(250);

		if (dataPtr != NULL)
		{
			if (MQTT_UnPacketRecv(dataPtr) == MQTT_PKT_CONNACK)
			{
				switch (MQTT_UnPacketConnectAck(dataPtr))
				{
					case 0: UsartPrintf(USART_DEBUG, "Tips: link OK");  status = 0; break;
					case 1: UsartPrintf(USART_DEBUG, "WARN: link fail, protocol ver"); break;
					case 2: UsartPrintf(USART_DEBUG, "WARN: link fail, clientid"); break;
					case 3: UsartPrintf(USART_DEBUG, "WARN: link fail, server unavailable"); break;
					case 4: UsartPrintf(USART_DEBUG, "WARN: link fail, user/pass"); break;
					case 5: UsartPrintf(USART_DEBUG, "WARN: link fail, auth (token expired?)"); break;
					default: UsartPrintf(USART_DEBUG, "ERR: link fail, unknown"); break;
				}
			}
		}

		MQTT_DeleteBuffer(&mqttPacket);
	}
	else
		UsartPrintf(USART_DEBUG, "WARN: MQTT_PacketConnect Failed");

	App_SetCloudLink(1, (uint8_t)(status == 0 ? 1 : 0));

	return status;
}

//==========================================================
//	函数名称：	OneNet_Subscribe
//==========================================================
void OneNet_Subscribe(const char *topics[], unsigned char topic_cnt)
{
	unsigned char i = 0;
	MQTT_PACKET_STRUCTURE mqttPacket = {NULL, 0, 0, 0};

	for (; i < topic_cnt; i++)
		UsartPrintf(USART_DEBUG, "Subscribe: %s", topics[i]);

	if (MQTT_PacketSubscribe(MQTT_SUBSCRIBE_ID, MQTT_QOS_LEVEL0, topics, topic_cnt, &mqttPacket) == 0)
	{
		ESP8266_SendData(mqttPacket._data, mqttPacket._len);
		MQTT_DeleteBuffer(&mqttPacket);
	}
}

//==========================================================
//	函数名称：	OneNet_Publish
//==========================================================
void OneNet_Publish(const char *topic, const char *msg)
{
	MQTT_PACKET_STRUCTURE mqttPacket = {NULL, 0, 0, 0};

	UsartPrintf(USART_DEBUG, "Publish: %s", msg);

	if (MQTT_PacketPublish(MQTT_PUBLISH_ID, topic, msg, strlen(msg), MQTT_QOS_LEVEL0, 0, 1, &mqttPacket) == 0)
	{
		ESP8266_SendData(mqttPacket._data, mqttPacket._len);
		MQTT_DeleteBuffer(&mqttPacket);
	}
}

//==========================================================
//	函数名称：	OneNet_ReportProps
//	函数功能：	把所有属性打包上报（云端任务按上传周期调用）
//==========================================================
void OneNet_ReportProps(void)
{
	app_state_t s;
	uint16_t off = 0;

	App_Snapshot(&s);

	off = (uint16_t)snprintf(s_json, JSON_BUF_SIZE, "{\"id\":\"1\",\"version\":\"1.0\",\"params\":{");
	s_first = 1;                       /* 第一个属性前不加逗号 */
	/* 物模型里必须先存在这些标识符，否则平台会整条拒收（code 2306:
	   identifier not exist）——所以 temp/hum/r1~r4 一个都不能少。
	   r1~r4 建成 int32、读写属性；下发也支持 true/false（json_onoff）。 */
	off = js_num(s_json, JSON_BUF_SIZE, off, "temp", s.sensor_ok ? (s.temp_x10 / 10) : 0);
	off = js_num(s_json, JSON_BUF_SIZE, off, "hum",  s.sensor_ok ? (s.hum_x10  / 10) : 0);
	off = js_num(s_json, JSON_BUF_SIZE, off, "r1", (s.relay_mask & 0x01) ? 1 : 0);
	off = js_num(s_json, JSON_BUF_SIZE, off, "r2", (s.relay_mask & 0x02) ? 1 : 0);
	off = js_num(s_json, JSON_BUF_SIZE, off, "r3", (s.relay_mask & 0x04) ? 1 : 0);
	off = js_num(s_json, JSON_BUF_SIZE, off, "r4", (s.relay_mask & 0x08) ? 1 : 0);

	if (off < (JSON_BUF_SIZE - 2)) {          /* 收尾 */
		s_json[off++] = '}';
		s_json[off++] = '}';
		s_json[off]   = '\0';
	}

	OneNet_Publish(ONENET_TOPIC_PROP_POST, s_json);
}

/* ===================== 云端下发 ===================== */

/* 取布尔/数字型属性值：0 / 1；0xFF 表示 JSON 里没有这个属性 */
static uint8_t json_onoff(const cJSON *it)
{
	if (it == NULL)                return 0xFF;
	if (it->type == cJSON_True)    return 1;
	if (it->type == cJSON_False)   return 0;
	if (it->type == cJSON_Number)  return (it->valueint != 0) ? 1 : 0;
	return 0xFF;
}

/* 可直接下发的数值型配置 */
static const struct {
	const char *name;
	cfg_idx_t   idx;
} s_cfg_prop[] = {
	{ "t_hi",   CFG_TEMP_HI       },
	{ "t_lo",   CFG_TEMP_LO       },
	{ "rh_hi",  CFG_HUM_HI        },
	{ "rh_lo",  CFG_HUM_LO        },
	{ "period", CFG_UPLOAD_PERIOD },
	{ "wake",   CFG_AWAKE_WINDOW  },	/* 保留：SLEEP 方案不再使用 */
};

/* 属性设置回执：把下发报文里的 id 原样带回，code=200 表示执行成功。
   不发回执时继电器其实也会动，但 OneNet 控制台会显示“设备未响应”。 */
static void onenet_send_set_reply(cJSON *json)
{
	cJSON *jid = cJSON_GetObjectItem(json, "id");
	char idbuf[32] = "0";
	char reply[96];

	if (jid != NULL) {
		if (jid->type == cJSON_String && jid->valuestring != NULL)
			snprintf(idbuf, sizeof(idbuf), "%s", jid->valuestring);
		else if (jid->type == cJSON_Number)
			snprintf(idbuf, sizeof(idbuf), "%ld", (long)jid->valueint);
	}

	snprintf(reply, sizeof(reply), "{\"id\":\"%s\",\"code\":200,\"msg\":\"success\"}", idbuf);
	OneNet_Publish(ONENET_TOPIC_PROP_SET_REPLY, reply);
}

//==========================================================
//	函数名称：	onenet_apply_params
//	函数功能：	执行云端下发的属性（params 对象）
//==========================================================
static void onenet_apply_params(const char *payload, unsigned short len)
{
	cJSON *json, *params, *it;
	uint8_t v, mask;
	uint8_t i;

	if (len > 256) {                     /* 下发报文不该这么大，防越界 */
		UsartPrintf(USART_DEBUG, "WARN: set payload too long");
		return;
	}

	json = cJSON_Parse(payload);
	if (json == NULL) {
		UsartPrintf(USART_DEBUG, "WARN: set payload not JSON");
		return;
	}

	params = cJSON_GetObjectItem(json, "params");
	if (params == NULL) {
		cJSON_Delete(json);
		return;
	}

	/* ---- 4 路继电器：先算好新掩码，再整体下发 ---- */
	mask = Relay_Get();
	v = json_onoff(cJSON_GetObjectItem(params, "r1"));
	if (v != 0xFF) mask = (uint8_t)((mask & ~0x01U) | (v ? 0x01U : 0));
	v = json_onoff(cJSON_GetObjectItem(params, "r2"));
	if (v != 0xFF) mask = (uint8_t)((mask & ~0x02U) | (v ? 0x02U : 0));
	v = json_onoff(cJSON_GetObjectItem(params, "r3"));
	if (v != 0xFF) mask = (uint8_t)((mask & ~0x04U) | (v ? 0x04U : 0));
	v = json_onoff(cJSON_GetObjectItem(params, "r4"));
	if (v != 0xFF) mask = (uint8_t)((mask & ~0x08U) | (v ? 0x08U : 0));
	if (mask != Relay_Get()) {
		Relay_Apply(mask);
		UsartPrintf(USART_DEBUG, "cloud: relay mask=%d", mask);
	}

	/* ---- LED / 背光 / 工作模式 ---- */
	v = json_onoff(cJSON_GetObjectItem(params, "led"));
	if (v != 0xFF) Led_Set(v);

	v = json_onoff(cJSON_GetObjectItem(params, "bl"));
	if (v != 0xFF) PowerMgr_SetBacklight(v);

	v = json_onoff(cJSON_GetObjectItem(params, "mode"));
	if (v != 0xFF) PowerMgr_RequestMode(v ? APP_MODE_LOWPOWER : APP_MODE_NORMAL);

	/* ---- 阈值等数值属性（越界会被 App_SetCfg 拒绝） ---- */
	for (i = 0; i < (sizeof(s_cfg_prop) / sizeof(s_cfg_prop[0])); i++) {
		it = cJSON_GetObjectItem(params, s_cfg_prop[i].name);
		if (it != NULL && it->type == cJSON_Number) {
			int32_t nv = it->valueint;
			if (nv < 0) nv = 0;
			if (nv > 65535) nv = 65535;
			(void)App_SetCfg(s_cfg_prop[i].idx, (uint16_t)nv);
		}
	}

	onenet_send_set_reply(json);
	cJSON_Delete(json);
}

//==========================================================
//	函数名称：	OneNet_RevPro
//	函数功能：	平台下发数据处理（云端任务收到数据后调用）
//==========================================================
void OneNet_RevPro(unsigned char *cmd)
{
	MQTT_PACKET_STRUCTURE mqttPacket = {NULL, 0, 0, 0};

	char *req_payload = NULL;
	char *cmdid_topic = NULL;

	unsigned short topic_len = 0;
	unsigned short req_len = 0;

	unsigned char type = 0;
	unsigned char qos = 0;
	static unsigned short pkt_id = 0;

	short result = -1;

	type = MQTT_UnPacketRecv(cmd);
	switch (type)
	{
		case MQTT_PKT_CMD:
			result = MQTT_UnPacketCmd(cmd, &cmdid_topic, &req_payload, &req_len);
			if (result == 0) {
				UsartPrintf(USART_DEBUG, "cmdid: %s, req: %s", cmdid_topic, req_payload);
				if (MQTT_PacketCmdResp(cmdid_topic, req_payload, &mqttPacket) == 0) {
					ESP8266_SendData(mqttPacket._data, mqttPacket._len);
					MQTT_DeleteBuffer(&mqttPacket);
				}
			}
			break;

		case MQTT_PKT_PUBLISH:
			result = MQTT_UnPacketPublish(cmd, &cmdid_topic, &topic_len, &req_payload, &req_len, &qos, &pkt_id);
			if (result == 0) {
				UsartPrintf(USART_DEBUG, "topic: %s, payload: %s", cmdid_topic, req_payload);

				/* 物模型属性下发：解析并执行 */
				if (strstr(cmdid_topic, "property/set") != NULL) {
					onenet_apply_params(req_payload, req_len);
				}
			}
			break;

		case MQTT_PKT_PUBACK:
			if (MQTT_UnPacketPublishAck(cmd) == 0)
				UsartPrintf(USART_DEBUG, "Tips: MQTT Publish Send OK");
			result = 0;
			break;

		case MQTT_PKT_PUBREC:
			result = MQTT_UnPacketPublishRec(cmd);
			if (result == 0) {
				UsartPrintf(USART_DEBUG, "Tips: Rev PublishRec");
				if (MQTT_PacketPublishRel(MQTT_PUBLISH_ID, &mqttPacket) == 0) {
					ESP8266_SendData(mqttPacket._data, mqttPacket._len);
					MQTT_DeleteBuffer(&mqttPacket);
				}
			}
			break;

		case MQTT_PKT_PUBREL:
			result = MQTT_UnPacketPublishRel(cmd, pkt_id);
			if (result == 0) {
				UsartPrintf(USART_DEBUG, "Tips: Rev PublishRel");
				if (MQTT_PacketPublishComp(MQTT_PUBLISH_ID, &mqttPacket) == 0) {
					ESP8266_SendData(mqttPacket._data, mqttPacket._len);
					MQTT_DeleteBuffer(&mqttPacket);
				}
			}
			break;

		case MQTT_PKT_PUBCOMP:
			if (MQTT_UnPacketPublishComp(cmd) == 0)
				UsartPrintf(USART_DEBUG, "Tips: Rev PublishComp");
			result = 0;
			break;

		case MQTT_PKT_SUBACK:
			if (MQTT_UnPacketSubscribe(cmd) == 0)
				UsartPrintf(USART_DEBUG, "Tips: MQTT Subscribe OK");
			else
				UsartPrintf(USART_DEBUG, "Tips: MQTT Subscribe Err");
			result = 0;
			break;

		case MQTT_PKT_UNSUBACK:
			if (MQTT_UnPacketUnSubscribe(cmd) == 0)
				UsartPrintf(USART_DEBUG, "Tips: MQTT UnSubscribe OK");
			else
				UsartPrintf(USART_DEBUG, "Tips: MQTT UnSubscribe Err");
			result = 0;
			break;

		default:
			result = -1;
			break;
	}

	ESP8266_Clear();

	if (result == -1) return;

	if (type == MQTT_PKT_CMD || type == MQTT_PKT_PUBLISH) {
		MQTT_FreeBuffer(cmdid_topic);
		MQTT_FreeBuffer(req_payload);
	}
}
