/* macOS simulator stand-in: there is no I2C bus, every ioctl fails at run time. */
#pragma once
#include <stdint.h>
#define I2C_SLAVE 0x0703
#define I2C_SLAVE_FORCE 0x0706
#define I2C_SMBUS 0x0720
#define I2C_SMBUS_READ 1
#define I2C_SMBUS_WRITE 0
#define I2C_SMBUS_BYTE_DATA 2
#define I2C_SMBUS_WORD_DATA 3
#define I2C_SMBUS_BLOCK_MAX 32
union i2c_smbus_data {
    uint8_t byte;
    uint16_t word;
    uint8_t block[I2C_SMBUS_BLOCK_MAX + 2];
};
struct i2c_smbus_ioctl_data {
    uint8_t read_write;
    uint8_t command;
    uint32_t size;
    union i2c_smbus_data * data;
};
