"""
asr_engine.py —— 用 Windows 自带中文语音识别引擎（System.Speech）把 WAV 转文字

为什么走这条路：
    系统已装 MS-2052（zh-CN）识别引擎，离线、免费、无需注册任何 API。
    音色/识别都用 Windows 自带组件，本机开箱即用。

用法（由 tts_server.py 内部调用，也可单独测试）：
    python tools/asr_engine.py some.wav
"""
import os
import subprocess
import sys
import tempfile

# 2052 = zh-CN。优先用中文引擎，没有就退回系统默认
PS_TEMPLATE = r"""
Add-Type -AssemblyName System.Speech
$wav = "{wav}"
$eng = $null
try {{
    $info = New-Object System.Globalization.CultureInfo("zh-CN")
    $eng = New-Object System.Speech.Recognition.SpeechRecognitionEngine($info)
}} catch {{
    $eng = New-Object System.Speech.Recognition.SpeechRecognitionEngine
}}
$eng.SetInputToWaveFile($wav)
$g = New-Object System.Speech.Recognition.DictationGrammar
$eng.LoadGrammar($g)
$res = $eng.Recognize()
if ($res -ne $null) {{
    [Console]::OutputEncoding = [System.Text.Encoding]::UTF8
    Write-Output $res.Text
}}
$eng.Dispose()
"""


def recognize_wav(path: str) -> str:
    """识别 WAV 文件，返回文本（失败返回空串）"""
    if not os.path.exists(path):
        return ""
    script = PS_TEMPLATE.format(wav=path.replace("\\", "\\\\"))
    try:
        r = subprocess.run(
            ["powershell", "-NoProfile", "-NonInteractive", "-Command", script],
            capture_output=True, timeout=60,
        )
    except subprocess.TimeoutExpired:
        return ""
    out = (r.stdout or b"").decode("utf-8", errors="replace").strip()
    # 去掉可能的 BOM / 空行
    return out.replace("\ufeff", "").strip()


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("用法: python asr_engine.py <wav文件>")
        sys.exit(1)
    print(recognize_wav(sys.argv[1]))
