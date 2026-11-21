/**
 * announce.cpp —— 塔台语音播报实现
 *
 * 文案由设备本地模板生成（零成本、可复现），航班号/数字按航空读法转换；
 * 语音由 TTS 服务合成（PC 端 tools/tts_server.py，Windows 自带语音，免费离线），
 * 设备取回 WAV 后经 ES8311 + 功放播出，叠加航空电台音效。
 */
#include "announce.h"
#include "port_audio.h"
#include "atc_ref.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <esp_heap_caps.h>
#include <mbedtls/base64.h>
#include <string.h>
#include <stdio.h>

#define ANN_RADIUS_KM   30.0f          /* 进圈播报半径 */
#define ANN_COOLDOWN_MS (15UL * 60UL * 1000UL)   /* 同机 15 分钟只播一次 */
#define ANN_MAX_REC     12
#define ANN_WAV_CAP     (1400 * 1024)  /* 云端合成 WAV 上限（约 45s 16k 单声道） */
#define MB_SSE_CAP      (1536 * 1024)   /* ModelBest SSE 响应缓冲 */

static char s_host[48] = "";           /* 备用：局域网 TTS host:port */
static char s_mb_key[80] = "";         /* 面壁 ModelBest Key（主用，VoxCPM2） */
static uint8_t *s_wav = nullptr;       /* PSRAM：WAV 缓冲 */
static char *s_sse = nullptr;          /* PSRAM：云端 SSE 原始响应 */
static volatile bool s_busy = false;   /* 正在播报/合成 */
static volatile int  s_auto = 1;       /* 进圈自动播报开关（1=开 0=只点击） */
static char s_status[64] = "";
static SemaphoreHandle_t s_sem = nullptr;

/* 待播队列（单槽即可：同时进圈的按顺序补播，后到的覆盖） */
static flight_t s_pending;
static float s_pendingKm = 0;
static volatile bool s_hasPending = false;

/* 已播记录：呼号 + 时间 */
typedef struct { char cs[FL_CALL_MAX]; uint32_t at; } ann_rec_t;
static ann_rec_t s_rec[ANN_MAX_REC];

/* ---------- 航空读法 ---------- */
static const char *DIG_CN[10] = { "洞", "幺", "两", "三", "四", "五", "六", "拐", "八", "九" };

/* 把整数读成中文（0~9999）。高度/距离用普通中文数词：
 * 18 → 十八（不是"幺十八"）；航空读法只用于逐位念的数字串）。 */
static void num_cn(int v, char *out, int cap)
{
    if (v < 0) v = 0;
    if (v > 9999) v = 9999;
    int th = v / 1000, h = (v / 100) % 10, t = (v / 10) % 10, u = v % 10;
    char b[48] = "";
    if (th) { strncat(b, DIG_CN[th], sizeof(b) - strlen(b) - 1); strncat(b, "千", sizeof(b) - strlen(b) - 1); }
    if (h) {
        if (th && h == 2) strncat(b, "两", sizeof(b) - strlen(b) - 1);   /* 两千 */
        else strncat(b, DIG_CN[h], sizeof(b) - strlen(b) - 1);
        strncat(b, "百", sizeof(b) - strlen(b) - 1);
    } else if (th && (t || u)) {
        strncat(b, "零", sizeof(b) - strlen(b) - 1);
    }
    if (t) {
        if (t == 2) strncat(b, "二", sizeof(b) - strlen(b) - 1);        /* 二十，不是两十 */
        else if (t > 1) strncat(b, DIG_CN[t], sizeof(b) - strlen(b) - 1);
        strncat(b, "十", sizeof(b) - strlen(b) - 1);
    } else if ((th || h) && u) {
        strncat(b, "零", sizeof(b) - strlen(b) - 1);
    }
    if (u) strncat(b, DIG_CN[u], sizeof(b) - strlen(b) - 1);
    if (!b[0]) snprintf(b, sizeof(b), "洞");
    snprintf(out, cap, "%s", b);
}

/* 航班号：字母原样，数字转航空读法（CA1724 → CA幺拐两四） */
static void fnum_spoken(const char *fnum, char *out, int cap)
{
    int m = 0;
    for (const char *p = fnum; *p && m < cap - 4; p++) {
        if (*p >= '0' && *p <= '9') {
            const char *d = DIG_CN[*p - '0'];
            while (*d && m < cap - 2) out[m++] = *d++;
        } else {
            out[m++] = *p;
        }
    }
    out[m] = 0;
}

/* 航司代码 → 中文呼号（真实通话里都这么念：MU→东方，CA→国航） */
static const char *airline_cn(const char *fnum)
{
    struct { const char *code, *cn; } M[] = {
        {"MU", "东方"}, {"CES", "东方"}, {"CA", "国航"}, {"CCA", "国航"},
        {"CZ", "南方"}, {"CSN", "南方"}, {"HU", "海南"}, {"CHH", "海南"},
        {"3U", "四川"}, {"CSC", "四川"}, {"ZH", "深圳"}, {"CSZ", "深圳"},
        {"MF", "厦门"}, {"CXA", "厦门"}, {"SC", "山东"}, {"CDG", "山东"},
        {"FM", "上海"}, {"CSH", "上海"}, {"9C", "春秋"}, {"CQH", "春秋"},
        {"HO", "吉祥"}, {"DKH", "吉祥"}, {"GS", "天津"}, {"GCR", "天津"},
        {"KN", "联合"}, {"CBK", "联合"}, {"PN", "西部"}, {"CHB", "西部"},
        {"EU", "成都"}, {"UEA", "成都"}, {"GJ", "长龙"}, {"CDC", "长龙"},
    };
    int bestLen = 0;
    const char *hit = nullptr;
    for (unsigned i = 0; i < sizeof(M) / sizeof(M[0]); i++) {
        size_t L = strlen(M[i].code);
        if ((int)L > bestLen && strncmp(fnum, M[i].code, L) == 0) {
            bestLen = (int)L;
            hit = M[i].cn;
        }
    }
    return hit;
}

/* ---------- 文案生成 ----------
 * 拟人化走 MiniCPM5-2B（面壁，塔台人设 + few-shot，每次措辞都不同），
 * 失败自动退回本地模板，保证链路永不空跑。 */

/* JSON 字符串解转义（含 \uXXXX → UTF-8） */
static size_t json_unescape(const char *src, char *out, size_t outSz)
{
    size_t m = 0;
    const char *p = src;
    while (*p && *p != '"' && m < outSz - 1) {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
            case 'n': out[m++] = '\n'; break;
            case 't': out[m++] = '\t'; break;
            case 'r': out[m++] = '\r'; break;
            case 'u': {
                unsigned cp = 0;
                for (int i = 0; i < 4 && p[1]; i++) {
                    char c = *++p;
                    cp <<= 4;
                    if (c >= '0' && c <= '9') cp |= c - '0';
                    else if (c >= 'a' && c <= 'f') cp |= c - 'a' + 10;
                    else if (c >= 'A' && c <= 'F') cp |= c - 'A' + 10;
                }
                if (cp < 0x80) out[m++] = (char)cp;
                else if (cp < 0x800) {
                    out[m++] = (char)(0xC0 | (cp >> 6));
                    out[m++] = (char)(0x80 | (cp & 0x3F));
                } else {
                    out[m++] = (char)(0xE0 | (cp >> 12));
                    out[m++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    out[m++] = (char)(0x80 | (cp & 0x3F));
                }
                break;
            }
            default: out[m++] = *p; break;
            }
            p++;
        } else {
            out[m++] = *p++;
        }
    }
    out[m] = 0;
    return m;
}

/* 文案后处理：ASCII 航班号 → 航空读法；「零」按读法改「洞」 */
static void fix_script_fnum(const char *fnum, char *script, size_t scriptSz)
{
    char spoken[32];
    fnum_spoken(fnum, spoken, sizeof(spoken));
    char *hit = strstr(script, fnum);
    if (hit) {
        size_t fnLen = strlen(fnum);
        size_t headLen = (size_t)(hit - script);
        size_t tailLen = strlen(hit + fnLen);
        if (headLen + strlen(spoken) + tailLen < scriptSz) {
            memmove(hit + strlen(spoken), hit + fnLen, tailLen + 1);
            memcpy(hit, spoken, strlen(spoken));
        }
    }
    for (char *p = script; p[0] && p[1] && p[2]; p++)
        if ((uint8_t)p[0] == 0xE9 && (uint8_t)p[1] == 0x9B && (uint8_t)p[2] == 0xB6) {
            p[0] = (char)0xE6; p[1] = (char)0xB4; p[2] = (char)0x9E;
            p += 2;
        }
}

/* MiniCPM5-2B 生成塔台文案（照搬原仓库的人设与 few-shot 思路） */
static bool gen_script_llm(const flight_t *f, float km, char *out, int cap)
{
    if (!s_mb_key[0] || !s_sse) return false;

    int alt_m = (int)((float)f->alt * 100.0f * 0.3048f);
    int spd_kmh = (int)(f->spd * 1.852f);
    char user[160];
    snprintf(user, sizeof(user), "航班%s，高度%d米，速度%d公里每小时，距离%.0f公里",
             f->callsign, alt_m, spd_kmh, (double)km);

    char body[1024];
    snprintf(body, sizeof(body),
        "{\"model\":\"MiniCPM5-2B\",\"stream\":false,\"max_tokens\":600,\"temperature\":1.0,"
        "\"messages\":["
        "{\"role\":\"system\",\"content\":\"你是机场塔台管制员，在雷达席做实时播报。"
        "把航班信息浓缩成一句25字以内的中文口语播报，直接输出播报句本身，"
        "不要任何标题、前缀、引号或解释。"
        "航班号必须逐字符原样照抄（大写字母+阿拉伯数字，如CA1724），绝对不要转换成中文读法。"
        "高度、速度、距离保持阿拉伯数字不要转成中文。\"},"
        "{\"role\":\"user\",\"content\":\"航班CA1724，高度3452米，速度604公里每小时，距离18公里\"},"
        "{\"role\":\"assistant\",\"content\":\"注意，CA1724正在进近，高度3452米\"},"
        "{\"role\":\"user\",\"content\":\"航班MU5381，高度610米，速度330公里每小时，距离12公里\"},"
        "{\"role\":\"assistant\",\"content\":\"MU5381，距离跑道12公里，准备落地\"},"
        "{\"role\":\"user\",\"content\":\"%s\"}"
        "]}", user);

    WiFiClientSecure cli;
    cli.setInsecure();
    HTTPClient http;
    http.setTimeout(20000);
    if (!http.begin(cli, "https://api.modelbest.cn/v1/chat/completions")) return false;
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", String("Bearer ") + s_mb_key);
    int code = http.POST((uint8_t *)body, strlen(body));
    if (code != 200) {
        Serial.printf("[ann] LLM http %d\n", code);
        http.end();
        return false;
    }
    String resp = http.getString();          /* 响应仅几 KB，内部堆放得下 */
    http.end();

    const char *p = strstr(resp.c_str(), "\"role\":\"assistant\"");
    if (!p) return false;
    p = strstr(p, "\"content\":\"");
    if (!p) return false;
    p += strlen("\"content\":\"");
    size_t n = json_unescape(p, out, cap);
    while (n && (out[n - 1] == '\n' || out[n - 1] == ' ' || out[n - 1] == '.')) out[--n] = 0;
    if (!n) return false;
    /* 有效性：太短、括号回显（模型复读指令）都算失败 */
    if (n < 8) return false;
    if (strstr(out, "（") || strstr(out, "(") || strstr(out, "口语")) return false;
    fix_script_fnum(f->callsign[0] ? f->callsign : "", out, cap);
    Serial.printf("[ann] LLM 文案: %s\n", out);
    return true;
}

/* 本地模板兜底（原逻辑） */
static void build_script_tpl(const flight_t *f, float km, char *out, int cap)
{
    /* 呼号：航司字母换中文（MU5102 → 东方 + 五幺洞两），数字走航空读法 */
    const char *cs = f->callsign[0] ? f->callsign : "未知";
    const char *air = airline_cn(cs);
    const char *numPart = cs;
    if (air) {
        while (*numPart && !(*numPart >= '0' && *numPart <= '9')) numPart++;
    }
    char digits[40];
    fnum_spoken(air ? numPart : cs, digits, sizeof(digits));
    char fn[56];
    if (air) snprintf(fn, sizeof(fn), "%s%s", air, digits);
    else     snprintf(fn, sizeof(fn), "%s", digits);

    char dist[24], alt[24];
    num_cn((int)(km + 0.5f), dist, sizeof(dist));
    int alt_m = (int)((float)f->alt * 100.0f * 0.3048f);   /* 百英尺 → 米 */
    num_cn(alt_m, alt, sizeof(alt));

    /* 口吻分层 + 句式轮换，避免每次都一个腔调 */
    static const char *ARR[] = {
        "%s，距离%s公里，高度%s米，准备落地",
        "注意%s，五边%s公里，高度%s米",
        "%s，进近中，距跑道%s公里，高度%s米",
    };
    static const char *CRZ[] = {
        "%s，距离%s公里，巡航高度",
        "雷达看到%s，%s公里外，高度正常",
        "%s，%s公里，巡航通过",
    };
    static const char *MID_LVL[] = {
        "%s，距离%s公里，高度%s米",
        "雷达识别%s，%s公里，高度%s米",
        "%s，%s公里处，高度%s米",
    };
    static uint8_t s_idx = 0;
    s_idx = (uint8_t)((s_idx + 1 + (uint8_t)(millis() % 3)) % 3);

    if (alt_m < 900)
        snprintf(out, cap, ARR[s_idx], fn, dist, alt);
    else if (alt_m >= 8000)
        snprintf(out, cap, CRZ[s_idx], fn, dist);
    else
        snprintf(out, cap, MID_LVL[s_idx], fn, dist, alt);
}

/* 对外入口：LLM 优先，模板兜底 */
static void build_script(const flight_t *f, float km, char *out, int cap)
{
    if (gen_script_llm(f, km, out, cap)) return;
    build_script_tpl(f, km, out, cap);
}

/* ---------- TTS 拉取 ---------- */
static int url_encode(const char *in, char *out, int cap)
{
    static const char *HEXD = "0123456789ABCDEF";
    int m = 0;
    for (const uint8_t *p = (const uint8_t *)in; *p && m < cap - 4; p++) {
        uint8_t c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out[m++] = (char)c;
        } else {
            out[m++] = '%';
            out[m++] = HEXD[c >> 4];
            out[m++] = HEXD[c & 0x0F];
        }
    }
    out[m] = 0;
    return m;
}

/* POST api.modelbest.cn /v1/audio/speech → SSE(base64) → 拼成 WAV，返回字节数 */
/* 单次请求，收满 SSE 后解码。返回解码字节数，*complete 标记是否收到 done */
static size_t tts_mb_once(const char *text, bool *complete)
{
    if (complete) *complete = false;
    if (!s_mb_key[0] || !s_sse || !s_wav) return 0;

    /* JSON 转义文案 */
    char esc[400];
    size_t e = 0;
    for (const char *p = text; *p && e < sizeof(esc) - 2; p++) {
        if (*p == '"' || *p == '\\') esc[e++] = '\\';
        esc[e++] = *p;
    }
    esc[e] = 0;

    /* 请求体（含 base64 参考音色 ~128KB）→ PSRAM 常驻缓冲 */
    static char *s_req = nullptr;
    if (!s_req) s_req = (char *)heap_caps_malloc(400 * 1024, MALLOC_CAP_SPIRAM);
    char *body = s_req;
    int bodyLen;
    if (ATC_REF_WAV_LEN > 4 && ATC_REF_TEXT[0]) {
        /* 提供了参考音频 → 走音色克隆 */
        int h = snprintf(body, 400 * 1024,
                 "{\"model\":\"VoxCPM2\",\"input\":\"%s\",\"response_format\":\"wav\",\"ref_audio\":\"data:audio/wav;base64,",
                 esc);
        size_t b64len = 0;
        if (mbedtls_base64_encode((unsigned char *)body + h, 400 * 1024 - h - 4, &b64len,
                                  ATC_REF_WAV, ATC_REF_WAV_LEN) != 0) {
            Serial.println("[ann] ref b64 encode failed");
            return 0;
        }
        strcpy(body + h + b64len, "\",\"ref_text\":\"");
        strcat(body + h + b64len, ATC_REF_TEXT);
        strcat(body + h + b64len, "\"}");
        bodyLen = h + b64len + strlen(body + h + b64len);
    } else {
        /* 无参考音频 → 普通 VoxCPM2 合成 */
        bodyLen = snprintf(body, 400 * 1024,
                 "{\"model\":\"VoxCPM2\",\"input\":\"%s\",\"response_format\":\"wav\"}", esc);
    }

    /* TLS 握手需要 ~40KB 连续内部块；等 LLM/上一次请求释放完再开始 */
    uint32_t tw = millis();
    while (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) < 45 * 1024 &&
           millis() - tw < 10000)
        vTaskDelay(pdMS_TO_TICKS(100));
    Serial.printf("[ann] TTS 前堆 %u/%u\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    WiFiClientSecure cli;
    cli.setInsecure();
    HTTPClient http;
    http.setTimeout(60000);
    if (!http.begin(cli, "https://api.modelbest.cn/v1/audio/speech")) {
        Serial.println("[ann] http.begin 失败（内存不足）");
        return 0;
    }
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", String("Bearer ") + s_mb_key);

    int code = http.POST((uint8_t *)body, bodyLen);
    if (code != 200) {
        Serial.printf("[ann] TTS http %d | body=%u\n", code, (unsigned)bodyLen);
        Serial.printf("[ann] 服务端: %s\n", http.getString().c_str());
        http.end();
        return 0;
    }

    /* 收 SSE 到 PSRAM */
    struct Sink : public Stream {
        size_t n = 0;
        size_t write(uint8_t b) override { if (n < MB_SSE_CAP - 1) s_sse[n++] = (char)b; return 1; }
        size_t write(const uint8_t *p, size_t len) override {
            size_t room = MB_SSE_CAP - 1 - n;
            size_t c = len < room ? len : room;
            memcpy(s_sse + n, p, c); n += c; return c;
        }
        int available() override { return 0; }
        int read() override { return -1; }
        int peek() override { return -1; }
    } sink;
    http.writeToStream(&sink);
    s_sse[sink.n] = 0;
    http.end();

    /* 逐块解码 "audio":"..." 拼 WAV */
    size_t wavLen = 0;
    const char *p = s_sse, *end = s_sse + sink.n;
    while (p < end) {
        const char *a = strstr(p, "\"audio\":\"");
        if (!a || a >= end) break;
        a += 9;
        const char *b = strchr(a, '"');
        if (!b || b > end) break;
        size_t b64len = (size_t)(b - a);
        size_t olen = 0;
        if (wavLen + b64len / 4 * 3 + 8 < ANN_WAV_CAP &&
            mbedtls_base64_decode(s_wav + wavLen, ANN_WAV_CAP - wavLen, &olen,
                                  (const unsigned char *)a, b64len) == 0)
            wavLen += olen;
        p = b + 1;
    }
    if (complete) *complete = strstr(s_sse, "speech.audio.done") != nullptr;
    Serial.printf("[ann] 云端语音 %u 字节（SSE %u，%s）\n",
                  (unsigned)wavLen, (unsigned)sink.n,
                  (complete && *complete) ? "完整" : "被截断");
    return wavLen;
}

/* 外层：SSE 偶尔几十 KB 就断，按 done 标记判断完整性，最多 3 次 */
static size_t tts_fetch_mb(const char *text)
{
    for (int attempt = 0; attempt < 8; attempt++) {
        bool done = false;
        size_t n = tts_mb_once(text, &done);
        if (done && n > 8000) return n;
        if (n > 1000 && attempt == 7) return n;      /* 最后一次，凑合播 */
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
    return 0;
}

/* GET http://host/tts?text=... → WAV 写入 s_wav，返回字节数（0=失败） */
static size_t tts_fetch(const char *text)
{
    if (s_mb_key[0]) {
        size_t n = tts_fetch_mb(text);
        if (n > 1000) return n;
    }
    if (!s_host[0] || !s_wav) return 0;
    char enc[400];
    url_encode(text, enc, sizeof(enc));
    char url[560];
    snprintf(url, sizeof(url), "http://%s/tts?text=%s", s_host, enc);

    WiFiClient cli;
    HTTPClient http;
    http.setTimeout(15000);
    if (!http.begin(cli, url)) return 0;
    int code = http.GET();
    if (code != 200) {
        Serial.printf("[ann] TTS http %d\n", code);
        http.end();
        return 0;
    }
    WiFiClient *st = http.getStreamPtr();
    size_t n = 0;
    uint32_t t0 = millis();
    while (n < ANN_WAV_CAP - 1 && millis() - t0 < 15000) {
        size_t avail = st->available();
        if (avail) {
            size_t room = ANN_WAV_CAP - 1 - n;
            if (avail > room) avail = room;
            int r = st->read(s_wav + n, avail);
            if (r > 0) { n += (size_t)r; t0 = millis(); }
        } else if (!http.connected()) {
            break;
        } else {
            vTaskDelay(5);
        }
    }
    http.end();
    return n;
}

/* ---------- 播报任务 ---------- */
static void announce_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (!s_hasPending) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        flight_t f = s_pending;
        float km = s_pendingKm;
        s_hasPending = false;

        s_busy = true;
        char script[160];
        build_script(&f, km, script, sizeof(script));
        snprintf(s_status, sizeof(s_status), "%s", script);
        Serial.printf("[ann] 播报: %s\n", script);

        size_t n = tts_fetch(script);
        if (n > 1000 && audio_ready()) {
            audio_play_wav(s_wav, n);
        } else if (n <= 1000) {
            Serial.println("[ann] TTS 无响应，改播提示音");
            audio_ping();
        }
        s_status[0] = 0;
        s_busy = false;
    }
}

void announce_init(void)
{
    if (!s_wav)
        s_wav = (uint8_t *)heap_caps_malloc(ANN_WAV_CAP, MALLOC_CAP_SPIRAM);
    if (!s_sse)
        s_sse = (char *)heap_caps_malloc(MB_SSE_CAP, MALLOC_CAP_SPIRAM);
    if (!s_wav) {
        Serial.println("[ann] PSRAM 缓冲分配失败，播报不可用");
        return;
    }
    if (!s_sem) s_sem = xSemaphoreCreateBinary();
    memset(s_rec, 0, sizeof(s_rec));
    /* 24KB 栈：TLS 握手 + I2S 大块写入 + 重采样都在这个任务里，12KB 会溢出 */
    xTaskCreatePinnedToCore(announce_task, "ann", 24576, NULL, 2, NULL, 0);
    Serial.println("[ann] 播报任务就绪");
}

void announce_set_mbkey(const char *key)
{
    snprintf(s_mb_key, sizeof(s_mb_key), "%s", key ? key : "");
    Serial.printf("[ann] 面壁 VoxCPM2: %s\n", s_mb_key[0] ? "已配置" : "未配置");
}

void announce_set_host(const char *host_port)
{
    snprintf(s_host, sizeof(s_host), "%s", host_port ? host_port : "");
    Serial.printf("[ann] TTS 服务: %s\n", s_host[0] ? s_host : "未配置（播报静默）");
}

void announce_set_auto(int on) { s_auto = on ? 1 : 0; }
int announce_auto(void) { return s_auto; }

void announce_notify(const flight_t *f, float dist_km)
{
    if (!s_auto) { Serial.println("[ann] 拦截: STBY 自动播报关闭"); return; }
    if (!f || !f->callsign[0]) { Serial.println("[ann] 拦截: 无呼号"); return; }
    if (!s_wav) { Serial.println("[ann] 拦截: WAV 缓冲未分配"); return; }
    if (!s_mb_key[0] && !s_host[0]) { Serial.println("[ann] 拦截: 无 TTS 配置"); return; }
    if (dist_km > ANN_RADIUS_KM) { Serial.println("[ann] 拦截: 超出30km"); return; }
    if (s_hasPending) { Serial.println("[ann] 拦截: 已有待播"); return; }
    if (s_busy) { Serial.println("[ann] 拦截: 正在播报中"); return; }

    uint32_t now = millis();
    for (int i = 0; i < ANN_MAX_REC; i++) {
        if (s_rec[i].cs[0] && strcmp(s_rec[i].cs, f->callsign) == 0) {
            if (now - s_rec[i].at < ANN_COOLDOWN_MS) return;
            s_rec[i].at = now;
            goto queue;
        }
    }
    for (int i = 0; i < ANN_MAX_REC; i++) {
        if (!s_rec[i].cs[0]) {
            snprintf(s_rec[i].cs, sizeof(s_rec[i].cs), "%s", f->callsign);
            s_rec[i].at = now;
            break;
        }
    }
queue:
    s_pending = *f;
    s_pendingKm = dist_km;
    s_hasPending = true;
}

int announce_busy(void) { return s_busy ? 1 : 0; }

/* 点击即播：跳过冷却与距离判断（距离用目标当前值算文案） */
void announce_speak_now(const flight_t *f)
{
    if (!f || !f->callsign[0] || !s_wav) return;
    if (!s_mb_key[0] && !s_host[0]) return;
    if (s_hasPending || s_busy) return;
    s_pending = *f;
    s_pendingKm = f->dist_nm * 1.852f;
    s_hasPending = true;
}

void announce_status(char *buf, int n)
{
    if (!buf || n <= 0) return;
    snprintf(buf, n, "%s", s_status);
}

void announce_test(void)
{
    flight_t f;
    memset(&f, 0, sizeof(f));
    snprintf(f.callsign, sizeof(f.callsign), "MU5102");
    f.alt = 82;        /* 约 2500 米 */
    f.spd = 280;
    f.hdg = 45;
    announce_notify(&f, 18.0f);
}
