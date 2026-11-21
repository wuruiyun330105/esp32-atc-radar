"""
tts_server.py —— 塔台播报用的本地 TTS 服务（Windows 自带语音，免费离线）

作用：
    ESP32 雷达进圈时 GET http://<本机IP>:8765/tts?text=注意%2C... 
    本服务用 Windows 语音合成（System.Speech）生成 WAV，直接回给设备播放。

为什么这样做：
    原仓库 emwstudio/esp32-radar 用面壁 VoxCPM2 克隆音色（需 ModelBest API Key）。
    本方案改用 Windows 本地语音，零成本、不联网、无需注册；音色选中文女声（Huihui）。
    想升级成克隆音色，把 _synth_wav() 换成调 ModelBest 即可，接口不变。

用法：
    python tools/tts_server.py            # 默认 0.0.0.0:8765
    python tools/tts_server.py 9000       # 换端口

设备端配置：配网页/固件默认 tts_host = "<本机局域网IP>:8765"
"""
import os
import subprocess
import sys
import tempfile
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import urlparse, parse_qs, unquote

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8765

# 中文语音候选（按优先级）；系统没有中文语音时回退默认
VOICE_CANDIDATES = ["Microsoft Huihui Desktop", "Microsoft Yaoyao", "Microsoft Kangkang", ""]

PS_TEMPLATE = r"""
Add-Type -AssemblyName System.Speech
$sp = New-Object System.Speech.Synthesis.SpeechSynthesizer
$voice = $null
foreach ($v in @({voices})) {{
    try {{ $sp.SelectVoice($v); $voice = $v; break }} catch {{ }}
}}
if ($voice -eq $null) {{ Write-Host "voice=default" }} else {{ Write-Host "voice=$voice" }}
$sp.Rate = 0
$fmt = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo(16000,
        [System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen,
        [System.Speech.AudioFormat.AudioChannel]::Mono)
$sp.SetOutputToWaveFile("{out}", $fmt)
$sp.Speak("{text}")
$sp.Dispose()
"""


def _ps_escape(s: str) -> str:
    return s.replace("`", "``").replace('"', '`"').replace("$", "`$")


def synth_wav(text: str) -> bytes:
    """调用 Windows 语音合成，返回 WAV 字节"""
    voices = ",".join('"%s"' % v for v in VOICE_CANDIDATES if v)
    out = os.path.join(tempfile.gettempdir(), "radar_tts.wav")
    if os.path.exists(out):
        try:
            os.remove(out)
        except OSError:
            pass
    script = PS_TEMPLATE.format(voices=voices, out=out.replace("\\", "\\\\"),
                                text=_ps_escape(text))
    subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", script],
                   capture_output=True, timeout=30)
    if not os.path.exists(out):
        return b""
    with open(out, "rb") as f:
        return f.read()


class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        # 语音指令识别：原始 WAV 从设备 POST 上来 → 返回 UTF-8 文本
        if urlparse(self.path).path != "/asr":
            self.send_error(404)
            return
        n = int(self.headers.get("Content-Length", 0) or 0)
        data = self.rfile.read(n) if n else b""
        if len(data) < 44:
            self.send_error(400, "empty wav")
            return
        wav = os.path.join(tempfile.gettempdir(), "radar_asr.wav")
        with open(wav, "wb") as f:
            f.write(data)
        try:
            from asr_engine import recognize_wav
            text = recognize_wav(wav)
        except Exception as e:
            print("[asr] 识别异常:", e, flush=True)
            text = ""
        print("[asr] %d 字节 -> %r" % (len(data), text), flush=True)
        body = text.encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        u = urlparse(self.path)
        if u.path != "/tts":
            self.send_error(404)
            return
        q = parse_qs(u.query)
        text = unquote(q.get("text", [""])[0]).strip()
        if not text:
            self.send_error(400, "missing text")
            return
        print("[tts]", text, flush=True)
        wav = synth_wav(text)
        if not wav:
            print("[tts] 合成失败（检查系统是否装了中文语音）", flush=True)
            self.send_error(500, "tts failed")
            return
        self.send_response(200)
        self.send_header("Content-Type", "audio/wav")
        self.send_header("Content-Length", str(len(wav)))
        self.end_headers()
        self.wfile.write(wav)
        print("[tts] 返回 %d 字节" % len(wav), flush=True)

    def log_message(self, *a):
        pass


if __name__ == "__main__":
    print("塔台播报 TTS 服务 → http://0.0.0.0:%d/tts?text=..." % PORT)
    print("系统语音:", end=" ")
    try:
        r = subprocess.run(["powershell", "-NoProfile", "-Command",
                            "Add-Type -AssemblyName System.Speech; "
                            "(New-Object System.Speech.Synthesis.SpeechSynthesizer).GetInstalledVoices()"
                            " | % { $_.VoiceInfo.Name }"],
                           capture_output=True, text=True, timeout=20)
        print((r.stdout or "").strip() or "(无)")
    except Exception as e:
        print("查询失败:", e)
    HTTPServer(("0.0.0.0", PORT), Handler).serve_forever()
