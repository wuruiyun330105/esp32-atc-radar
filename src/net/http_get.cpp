/**
 * net/http_get.cpp —— 设备端 HTTP 抓取（ESP32 / Arduino）
 *
 * 为什么单独一个 .cpp：
 *   Arduino 的 HTTPClient / WiFiClientSecure 是 C++ 类，而解析器是纯 C，
 *   用 extern "C" 导出成 C 接口，两边各写各的。
 *
 * 内存：响应体直接落在 PSRAM（8MB 里划 384KB），避免 String 反复扩容把内部堆撑爆。
 * 两种连接：https 用 WiFiClientSecure，http（局域网 dump1090）用 WiFiClient。
 */
#ifdef ESP_PLATFORM

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <esp_heap_caps.h>

#define HTTP_BODY_CAP (384 * 1024)

/* 把 HTTPClient::writeToStream 的输出（含 chunked 解块后的原始字节）接进 PSRAM 缓冲。
 * 之前直接读 getStreamPtr() 拿到的是未解块的 chunked 流，响应体里混着分块长度头。 */
class PsramWriter : public Stream {
public:
    char *buf; size_t cap, pos = 0;
    PsramWriter(char *b, size_t c) : buf(b), cap(c) {}
    size_t write(uint8_t c) override { if (pos < cap - 1) buf[pos++] = (char)c; return 1; }
    size_t write(const uint8_t *p, size_t n) override {
        size_t room = cap - 1 - pos; if (n > room) n = room;
        memcpy(buf + pos, p, n); pos += n; return n;
    }
    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }
    void flush() override {}
};

extern "C" char *http_get_psram_ex(const char *url, int *out_len, int *out_code)
{
    static char *buf = nullptr;
    if (out_len) *out_len = 0;
    if (out_code) *out_code = -1;   /* 默认：连接层失败（未拿到 HTTP 状态码） */
    if (!buf) {
        buf = (char *)heap_caps_malloc(HTTP_BODY_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!buf) buf = (char *)heap_caps_malloc(HTTP_BODY_CAP, MALLOC_CAP_8BIT);
        if (!buf) return nullptr;
    }

    bool https = (strncmp(url, "https://", 8) == 0);
    WiFiClientSecure *sec = nullptr;
    WiFiClient *plain = nullptr;
    if (https) {
        sec = new WiFiClientSecure();
        sec->setInsecure();                 /* 简化：不校验证书链（雷达数据非敏感） */
        sec->setTimeout(9);
    } else {
        plain = new WiFiClient();
        plain->setTimeout(9);
    }

    HTTPClient http;
    http.setTimeout(9000);
    http.setReuse(false);
    bool begun = https ? http.begin(*sec, url) : http.begin(*plain, url);
    int got = -1;

    if (begun) {
        http.setUserAgent("esp32-ppi-radar/1.0");
        int code = http.GET();
        Serial.printf("[http] GET %s -> %d %s\n", url, code, http.errorToString(code).c_str());
        if (out_code) *out_code = code;     /* 403/429 等状态码透传给调用方做健康记忆 */
        if (code == 200) {
            /* writeToStream 内部处理 chunked 解块，直接读流会混入分块长度头 */
            PsramWriter w(buf, HTTP_BODY_CAP);
            http.writeToStream(&w);
            buf[w.pos] = 0;
            got = (int)w.pos;
        }
        http.end();
    }

    delete sec;
    delete plain;
    if (out_len) *out_len = got > 0 ? got : 0;
    return got > 0 ? buf : nullptr;
}

/* 兼容旧接口：不关心状态码的调用方 */
extern "C" char *http_get_psram(const char *url, int *out_len)
{
    return http_get_psram_ex(url, out_len, nullptr);
}

/* MCP 网关用的 JSON POST：飞常准 c-gw.variflight.com 是 JSON-RPC over HTTPS。
 * - Bearer 认证走 Authorization 头
 * - 网关用 Mcp-Session-Id 头传递会话（initialize 时下发，后续调用带回）：
 *   sid_in 非空则作为请求头发出；sid_out 接收响应下发的会话
 * - 响应体落在同一块 PSRAM 缓冲（net_task 串行使用，与 GET 不冲突） */
extern "C" char *http_post_json_ex(const char *url, const char *bearer,
                                   const char *json_body,
                                   const char *sid_in,
                                   char *sid_out, int sid_cap,
                                   int *out_len, int *out_code)
{
    static char *buf = nullptr;
    if (out_len) *out_len = 0;
    if (out_code) *out_code = -1;
    /* 注意：调用方常把同一个 s_sid 既传 sid_in 又传 sid_out（别名）。
     * 必须先把入参值保存到本地副本，再清空 sid_out，否则头永远发不出去。 */
    char sid_saved[80] = "";
    if (sid_in && sid_in[0]) strlcpy(sid_saved, sid_in, sizeof(sid_saved));
    /* 响应里没带会话头时不要清空 sid_out：调用方把 s_sid 传进来是当会话仓库用的，
     * 网关只在 initialize 响应里下发会话头，后续响应没有，清空会把会话弄丢 */
    if (!buf) {
        buf = (char *)heap_caps_malloc(HTTP_BODY_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!buf) buf = (char *)heap_caps_malloc(HTTP_BODY_CAP, MALLOC_CAP_8BIT);
        if (!buf) return nullptr;
    }

    bool https = (strncmp(url, "https://", 8) == 0);
    WiFiClientSecure *sec = nullptr;
    WiFiClient *plain = nullptr;
    if (https) {
        sec = new WiFiClientSecure();
        sec->setInsecure();
        sec->setTimeout(20);            /* MCP 网关聚合 400+ 条航班，比 GET 慢 */
    } else {
        plain = new WiFiClient();
        plain->setTimeout(20);
    }

    HTTPClient http;
    http.setTimeout(20000);
    http.setReuse(false);
    bool begun = https ? http.begin(*sec, url) : http.begin(*plain, url);
    int got = -1;

    if (begun) {
        http.setUserAgent("esp32-ppi-radar/1.0");
        http.addHeader("Content-Type", "application/json");
        http.addHeader("Accept", "application/json, text/event-stream");
        if (bearer && bearer[0])
            http.addHeader("Authorization", String("Bearer ") + bearer);
        if (sid_saved[0])
            http.addHeader("Mcp-Session-Id", sid_saved);
        const char *HDRKEY = "Mcp-Session-Id";
        http.collectHeaders(&HDRKEY, 1);
        int code = http.POST(json_body);
        Serial.printf("[http] POST %s -> %d %s\n", url, code, http.errorToString(code).c_str());
        if (out_code) *out_code = code;
        if (sid_out && sid_cap > 0) {
            String s = http.header("Mcp-Session-Id");
            if (s.length() > 0)                 /* 只有真拿到了才覆盖 */
                strlcpy(sid_out, s.c_str(), sid_cap);
        }
        if (code == 200 || (code >= 400 && code < 500)) {
            /* 4xx 也读 body：网关的报错原因在响应体里。writeToStream 解 chunked */
            PsramWriter w(buf, HTTP_BODY_CAP);
            http.writeToStream(&w);
            buf[w.pos] = 0;
            got = (int)w.pos;
        }
        http.end();
    }

    delete sec;
    delete plain;
    if (out_len) *out_len = got > 0 ? got : 0;
    /* 4xx 的 body 也返回（调用方打印报错原文），只有连接层失败才给 NULL */
    return got > 0 ? buf : nullptr;
}

#endif /* ESP_PLATFORM */
