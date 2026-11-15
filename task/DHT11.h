#ifndef __DHT11_H
#define __DHT11_H

uint8_t dht11_readdata(uint8_t *temp,uint8_t *hum);
uint8_t dht11_init(void);
void dht11_mode_out(void);
void dht11_rst(void);
uint8_t dht11_check(void);

#endif
