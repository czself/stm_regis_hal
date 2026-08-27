#include <stdint.h>

/**
 * @brief 封装 VOFA+ JustFloat 数据包
 * @param buffer  用于存储打包后数据的缓冲区
 * @param data    指向 float 数组的指针
 * @param channels 数据通道数
 * @return uint32_t 返回总包长度（字节）
 */
uint32_t pack_vofa_justfloat(uint8_t* buffer, float* data, uint8_t channels) {
    uint32_t offset = 0;
    
    // 1. 将所有 float 数据拷贝到 buffer 中
    for (uint8_t i = 0; i < channels; i++) {
        // 使用指针强转或 memcpy 避免对齐问题
        *((float*)(buffer + offset)) = data[i];
        offset += sizeof(float);
    }
    
    // 2. 追加 JustFloat 帧尾: 0x00 0x00 0x80 0x7F
    buffer[offset++] = 0x00;
    buffer[offset++] = 0x00;
    buffer[offset++] = 0x80;
    buffer[offset++] = 0x7F;
    
    return offset; // 返回总长度，方便串口发送函数调用
}