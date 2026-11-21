/**
 * data_models.h —— 各页共用的数据结构
 * UI 层只依赖这些结构体，由数据层填充（设备端真实数据 / PC端mock）
 */
#ifndef DATA_MODELS_H
#define DATA_MODELS_H

#include <stdint.h>
#include <stdbool.h>

/* ---------- 天气数据（和风实时天气） ---------- */
#define WX_COND_MAX 24
#define WX_DIR_MAX  16

typedef struct {
    int  year, mon, day;
    int  weekday;              /* 0=周日 1=周一 ... 6=周六 */
    int  hour, min, sec;
    int  temp;                 /* 温度 ℃ */
    int  feels_like;           /* 体感 ℃ */
    int  humidity;             /* 湿度 % */
    int  wind_scale;           /* 风力等级 */
    char wind_dir[WX_DIR_MAX]; /* 风向，如 "东风" */
    char condition[WX_COND_MAX];/* 天气状况，如 "多云" */
    int  icon;                 /* 和风 icon 代码：100晴 101多云 3xx雨 ... */
    int  pressure;             /* 气压 hPa */
    int  precip;               /* 降水量 mm */
    int  update_hour, update_min; /* 数据更新时间(obsTime) */
    int  valid;                /* 是否有效（拿到过数据） */
} weather_data_t;

/* ---------- 航班目标（雷达页） ---------- */
#define FL_MAX       24        /* 同时跟踪的目标上限 */
#define FL_CALL_MAX  10        /* 呼号 8+1 */
#define FL_TYPE_MAX  8         /* 机型 B738 */
#define FL_ICAO_MAX  8         /* ICAO24 十六进制 */
#define FL_SQK_MAX   6         /* 应答机 4 位 */
#define FL_HIST_N    6         /* 历史尾迹点数 */

typedef struct {
    float x, y;                    /* 屏幕坐标（像素），数据层换算后填 */
    float hdg;                     /* 航向 0-360（度） */
    float spd;                     /* 地速（节） */
    int   alt;                     /* 气压高度（百英尺，与塔台显示一致） */
    int   vs;                      /* 垂直速率 ft/min */
    float dist_nm;                 /* 距雷达中心（海里） */
    float brg;                     /* 方位角（0=正北，顺时针） */
    char  callsign[FL_CALL_MAX];   /* 呼号 CES5102 */
    char  type[FL_TYPE_MAX];       /* 机型 B738 */
    char  icao[FL_ICAO_MAX];       /* ICAO24 地址 780A21 */
    char  squawk[FL_SQK_MAX];      /* 应答机 4132 */
    uint8_t is_ssr;                /* 1=二次雷达(有应答机/Mode S) 0=一次雷达 */
    uint8_t active;
    float glow;                    /* 余晖亮度 0..1（扫描激发后衰减，渲染用） */
    float hist[FL_HIST_N][2];      /* 历史尾迹屏幕坐标 */
    uint8_t hist_n;                /* 已记录尾迹点数 */
} flight_t;

typedef struct {
    flight_t items[FL_MAX];
    int      count;
    int      range_nm;             /* 当前量程（海里） */
    float    center_lat, center_lon;
    int      valid;
    int      src;                  /* 0=演示 1=真实(网络) */
    uint32_t updated_ms;           /* 上次数据刷新时刻(lv_tick) */
} flight_list_t;

/* ---------- 航班动态（飞常准 MCP 机场大屏，按需拉取计费） ---------- */
#define BD_MAX       10        /* 屏上最多显示 10 条（即将出发的优先） */
#define BD_FNUM_MAX  10        /* 航班号 MU5315 / HO1046 */
#define BD_DEST_MAX  28        /* 到达地（中文 UTF-8）"广州白云" */
#define BD_STAT_MAX  22        /* 状态 "催促登机"/"登机结束"/"延误"/"取消" */
#define BD_AP_MAX    10        /* 机场三字码 SHA */
#define BD_APNAME_MAX 28       /* 机场名 "上海虹桥" */

typedef struct {
    char fnum[BD_FNUM_MAX];
    char dest[BD_DEST_MAX];    /* 到达地（出发大屏） */
    char status[BD_STAT_MAX];
    char delay[10];            /* "+15" 延误分钟，空=准点 */
    char plan[8];              /* 计划 "19:35" */
    char est[8];               /* 预计 "19:50"，空=与计划同 */
    char term[8];              /* 航站楼 T2 */
    int  mins;                 /* 计划时刻分钟数（排序/过滤内部用） */
} board_item_t;

typedef struct {
    char airport[BD_AP_MAX];
    char airport_name[BD_APNAME_MAX];
    board_item_t items[BD_MAX];
    int  count;
    int  valid;
    int  cutoff_mins;          /* 内部：过滤线（计划分钟数），解析用 */
} board_list_t;

/* ---------- 在飞航班（雷达实时机群，飞常准轨迹接口） ---------- */
#define BD_INFL_MAX 16

typedef struct {
    char fnum[BD_FNUM_MAX];    /* 航班号 FM9306 */
    char dep[8];               /* 出发三字码 CAN */
    char arr[8];               /* 到达三字码 SHA */
    char date[12];             /* yyyy-MM-dd */
    char plan[8];              /* 计划到达时刻 HH:MM */
} board_inflight_t;

/* ---------- 配置（存 NVS/文件） ---------- */
#define CFG_SSID_MAX 33
#define CFG_PASS_MAX 65
#define CFG_KEY_MAX  65
#define CFG_HOST_MAX 48

/* 航班数据源 */
#define FL_SRC_DEMO   0            /* 本地演示（无网也能跑） */
#define FL_SRC_ADSB   1            /* airplanes.live / adsb.fi（免 key，最准） */
#define FL_SRC_OPENSKY 2           /* OpenSky Network（免 key，全球） */
#define FL_SRC_LOCAL   3           /* 局域网 dump1090（自架 RTL-SDR，覆盖最好） */

typedef struct {
    char  wifi_ssid[CFG_SSID_MAX];
    char  wifi_pass[CFG_PASS_MAX];
    char  qweather_key[CFG_KEY_MAX];
    char  qweather_host[CFG_KEY_MAX];  /* 专属 API Host */
    float latitude;                    /* 定位纬度（手机注入/手动） */
    float longitude;                   /* 定位经度 */
    int   range_nm;                    /* 雷达量程 */
    int   theme;                       /* 预留：主题编号 */
    int   flight_src;                  /* FL_SRC_* */
    int   ann_auto;                    /* 1=进圈自动播报 0=只点击播报 */
    char  adsb_host[CFG_HOST_MAX];     /* 本地接收机 192.168.1.50:8080 */
    char  vf_key[CFG_KEY_MAX];         /* 飞常准 MCP Key（sk-user-...，航班动态页） */
    char  tts_host[CFG_HOST_MAX];      /* 备用：局域网 TTS 服务（空=不用） */
    char  mb_key[CFG_KEY_MAX];         /* 面壁 ModelBest Key：VoxCPM2 语音合成（主用） */
    char  ai_url[96];                  /* 千问兼容端点 base */
    char  ai_key[192];                 /* 千问 API Key（较长） */
    char  ai_model[32];                /* 对话模型名 */
} app_config_t;

#endif
