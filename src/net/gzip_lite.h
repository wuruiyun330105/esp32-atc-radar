/* gzip_lite.h —— 轻量 gzip 嗅探 + miniz(tinfl) 解压（仅设备端）
 *
 * 背景：和风天气 2025 起无条件返回 gzip；飞常准 MCP 网关对大响应
 * 也可能 gzip。ESP-IDF/Arduino HTTPClient 不会自动解压，拿到压缩
 * 字节直接做 JSON 解析必然失败。两处数据模块共用本助手。
 *
 * 用法：recv 后若 gzip_inflate() 返回 >0 则用其输出，否则用原文
 * （未压缩响应原样返回 -1，走原路径，零开销）。
 */
#ifndef GZIP_LITE_H
#define GZIP_LITE_H

#ifdef ESP_PLATFORM

#include "miniz_tinfl.h"

/* 返回解压后字节数；非 gzip 或失败返回 -1（调用方回落用原文） */
static int gzip_inflate(const char *in, int in_len, char *out, int out_cap)
{
    if (in_len < 20 || (unsigned char)in[0] != 0x1f || (unsigned char)in[1] != 0x8b)
        return -1;
    unsigned char flg = (unsigned char)in[3];
    int off = 10;                                   /* 固定头 10 字节 */
    if (flg & 0x04) {                               /* FEXTRA */
        if (off + 2 > in_len) return -1;
        int xlen = (unsigned char)in[off] | ((unsigned char)in[off + 1] << 8);
        off += 2 + xlen;
    }
    if (flg & 0x08) { while (off < in_len && in[off]) off++; off++; }   /* FNAME */
    if (flg & 0x10) { while (off < in_len && in[off]) off++; off++; }  /* FCOMMENT */
    if (flg & 0x02) off += 2;                       /* FHCRC */
    if (off >= in_len) return -1;
    /* flags: bit2=输出缓冲可容纳完整流（免 32KB 回卷窗口） */
    size_t n = tinfl_decompress_mem_to_mem(out, (size_t)out_cap,
                                           in + off, (size_t)(in_len - off),
                                           TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    if (n == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED) return -1;
    out[n] = '\0';
    return (int)n;
}

#endif /* ESP_PLATFORM */
#endif /* GZIP_LITE_H */
