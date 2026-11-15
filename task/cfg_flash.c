/**
 * @file    cfg_flash.c
 * @brief   配置掉电保存实现
 */
#include "cfg_flash.h"
#include "app_state.h"
#include "modbus_rtu.h"     /* 复用 Crc16_Modbus() */
#include "stm32f1xx_hal.h"
#include <stddef.h>

/* STM32F103ZE: 512KB Flash，末页起始 0x08000000 + 0x7F800 */
#define CFG_FLASH_ADDR      0x0807F800UL
#define CFG_MAGIC           0x43464731UL    /* "CFG1" */
#define CFG_VERSION         1

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    uint16_t data[CFG_IDX_NUM];
    uint16_t crc;                           /* 覆盖前面所有字节的 CRC16 */
} cfg_blob_t;

static uint16_t blob_crc(const cfg_blob_t *p)
{
    return Crc16_Modbus((const uint8_t *)p, (uint16_t)offsetof(cfg_blob_t, crc));
}

uint8_t CfgFlash_Valid(void)
{
    const cfg_blob_t *p = (const cfg_blob_t *)CFG_FLASH_ADDR;

    if (p->magic != CFG_MAGIC)          return 0;
    if (p->version != CFG_VERSION)      return 0;
    if (p->count != CFG_IDX_NUM)        return 0;
    if (p->crc != blob_crc(p))          return 0;
    return 1;
}

void CfgFlash_Load(void)
{
    if (CfgFlash_Valid()) {
        const cfg_blob_t *p = (const cfg_blob_t *)CFG_FLASH_ADDR;
        App_CfgImport(p->data);
    } else {
        /* 首次上电或数据损坏：用默认值，dirty 会在空闲时写入 */
        App_CfgDefaults();
    }
}

uint8_t CfgFlash_Save(void)
{
    cfg_blob_t blob;
    FLASH_EraseInitTypeDef erase = {0};
    uint32_t page_err = 0;
    uint32_t addr = CFG_FLASH_ADDR;
    const uint16_t *src = (const uint16_t *)&blob;
    uint16_t i, n;

    blob.magic   = CFG_MAGIC;
    blob.version = CFG_VERSION;
    blob.count   = CFG_IDX_NUM;
    App_CfgExport(blob.data);
    blob.crc     = blob_crc(&blob);

    HAL_FLASH_Unlock();
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_PGERR | FLASH_FLAG_WRPERR);

    erase.TypeErase   = FLASH_TYPEERASE_PAGES;
    erase.PageAddress = CFG_FLASH_ADDR;
    erase.NbPages     = 1;
    if (HAL_FLASHEx_Erase(&erase, &page_err) != HAL_OK) {
        HAL_FLASH_Lock();
        return 0;
    }

    n = (uint16_t)((sizeof(cfg_blob_t) + 1U) / 2U);
    for (i = 0; i < n; i++) {
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, addr, src[i]) != HAL_OK) {
            HAL_FLASH_Lock();
            return 0;
        }
        addr += 2U;
    }
    HAL_FLASH_Lock();

    return CfgFlash_Valid();    /* 回读校验 */
}
