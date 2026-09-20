#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
电脉卫士 —— 大模型断闸决策网关（PC 侧）

板上的 openvela 只负责执行与状态回报（见 nuttx-overlay 下的
esp32p4_guardd.c）。判断放在这里：把电气故障事件和视觉火情置信度交给
大模型研判，得到 CUT / ALARM / NONE，再把单字符指令下发给板子。

为什么判断端不放在板上：ESP32-P4 没有射频，openvela 侧也没有 esp-hosted
驱动（P4 要借板载 C6 的 WiFi），ESPRESSIF_WIFI 的 Kconfig 依赖里根本没有
P4。所以 openvela 固件当前联不了网，大模型只能接在主机侧。

安全设计（重要）：
  1) 本地规则引擎先算一个基线结论，大模型只能在此基础上**升级**，不能降级。
     本地判定要断闸就一定断，不管大模型说什么、也不管网络通不通。
     一个安全装置不能把最终否决权交给一条会超时的链路。

  2) 升级幅度**一次最多一级**。实测同一条事件、同一模型、temperature=0，
     两次运行分别给出 ALARM 和 CUT —— 大模型本身就是不确定的，而 CUT
     意味着真停电。封成单级之后，一次幻觉最坏只是多响一次蜂鸣器。

依赖：pyserial（乐鑫 IDF 自带的 python 环境里就有）

用法：
  python guard_bridge.py --config guard_bridge.ini --scenario scenarios/demo.json
  python guard_bridge.py --config guard_bridge.ini --interactive
  python guard_bridge.py --no-llm --scenario scenarios/demo.json   # 只跑规则
"""

import argparse
import configparser
import json
import os
import posixpath
import sys
import threading
import time
import urllib.error
import urllib.request
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

try:
    import serial
except ImportError:
    serial = None

HERE = os.path.dirname(os.path.abspath(__file__))


# ---------------------------------------------------------------- 监控台状态
#
# dashboard.html 每 300ms 拉一次 /state 渲染。这里只做展示，决策逻辑
# 不读这个字典，所以面板挂了也不影响断闸。

STATE = {
    "connected": False,
    "dry_run": False,
    "port": "",
    "llm_enabled": False,
    "model": "",
    "cut": 0,
    "buzzer": 0,
    "board_reply": "",
    "current": None,
    "history": [],
}
STATE_LOCK = threading.Lock()


def state_set(**kw):
    with STATE_LOCK:
        STATE.update(kw)


class _Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        path = posixpath.normpath(self.path.split("?")[0])
        if path in ("/", "/index.html", "/dashboard.html"):
            self._file(os.path.join(HERE, "dashboard.html"), "text/html")
        elif path == "/state":
            with STATE_LOCK:
                body = json.dumps(STATE, ensure_ascii=False).encode("utf-8")
            self._send(body, "application/json")
        else:
            self.send_error(404)

    def _file(self, path, ctype):
        try:
            with open(path, "rb") as fp:
                self._send(fp.read(), ctype)
        except OSError:
            self.send_error(404)

    def _send(self, body, ctype):
        self.send_response(200)
        self.send_header("Content-Type", ctype + "; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        pass          # 别把 HTTP 访问日志混进演示输出


def serve(port):
    srv = ThreadingHTTPServer(("127.0.0.1", port), _Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv

# ---------------------------------------------------------------- 决策等级

ACTIONS = ("NONE", "ALARM", "CUT")
CMD_OF = {"NONE": None, "ALARM": "b", "CUT": "c"}


def rank(action):
    return ACTIONS.index(action) if action in ACTIONS else 0


# ---------------------------------------------------------------- 本地规则

def local_rule(ev):
    """不依赖网络的兜底判据。返回 (action, reason)。"""
    t = ev.get("type", "").upper()
    sev = int(ev.get("severity", 0))
    fire = float(ev.get("vision_fire", 0.0))
    smoke = float(ev.get("vision_smoke", 0.0))

    if t == "FIRE" or fire >= 0.60:
        return "CUT", "视觉确认火情（fire=%.2f）" % fire
    if sev >= 3:
        return "CUT", "故障等级 Lv3"
    if t in ("ARC", "OVERLOAD") and fire >= 0.35:
        return "CUT", "%s 叠加视觉火情迹象（fire=%.2f），交叉验证升级" % (t, fire)
    if sev == 2 or t == "SMOKE" or smoke >= 0.40:
        return "ALARM", "Lv2 或烟雾迹象，告警不断闸"
    return "NONE", "未达告警阈值"


# ---------------------------------------------------------------- 大模型

SYSTEM_PROMPT = """你是配电柜电气火灾预警系统的研判助手。
系统在低压配电柜内采集电流、电压、温度与电弧特征，同时用摄像头做火情/烟雾识别。
你要根据一条告警事件判断应采取的动作，只能选三者之一：

  NONE  —— 无需动作
  ALARM —— 本地声光告警，不断闸
  CUT   —— 立即切断三相电源（会造成停电，属于重决策）

判据要点：
- 电弧(ARC)与过载(OVERLOAD)是电气火灾的典型前兆，若同时有视觉火情证据，应果断断闸。
- 单纯的电压波动、瞬时过载通常不需要断闸。
- 烟雾早于明火，可先告警。
- 停电本身有代价（可能影响医疗、生产设备），不要过度断闸。

只输出 JSON，不要任何其它文字：
{"action": "NONE|ALARM|CUT", "confidence": 0.0-1.0, "reason": "一句话中文理由"}"""


def ask_llm(cfg, ev, timeout=20):
    """调用 OpenAI 兼容接口。失败返回 None（由本地规则兜底）。"""
    url = cfg["api_base"].rstrip("/") + "/chat/completions"
    body = {
        "model": cfg["model"],
        "temperature": 0,
        "messages": [
            {"role": "system", "content": SYSTEM_PROMPT},
            {"role": "user", "content": json.dumps(ev, ensure_ascii=False)},
        ],
    }
    req = urllib.request.Request(
        url,
        data=json.dumps(body).encode("utf-8"),
        headers={
            "Content-Type": "application/json",
            "Authorization": "Bearer " + cfg["api_key"],
        },
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            data = json.loads(resp.read().decode("utf-8"))
        text = data["choices"][0]["message"]["content"].strip()
        # 有些模型会用围栏包起来，剥掉再解析
        if "{" in text and "}" in text:
            text = text[text.find("{"): text.rfind("}") + 1]
        out = json.loads(text)
        act = str(out.get("action", "")).upper()
        if act not in ACTIONS:
            return None
        return {
            "action": act,
            "confidence": float(out.get("confidence", 0.0)),
            "reason": str(out.get("reason", "")),
        }
    except (urllib.error.URLError, urllib.error.HTTPError, OSError,
            ValueError, KeyError, IndexError) as exc:
        print("  [LLM] 调用失败，改用本地规则：%s" % exc)
        return None


# ---------------------------------------------------------------- 串口

class Board(object):
    """openvela guardd 的串口通道。

    单字符逐个发送、每个间隔 250 ms —— USB-Serial/JTAG 的接收侧连续快写
    会把主机的 OUT 端点堵住（已知缺陷，见 evidence/），慢发反而稳定。
    """

    def __init__(self, port, dry_run=False):
        self.dry_run = dry_run
        self.ser = None
        if dry_run:
            return
        if serial is None:
            raise RuntimeError("缺少 pyserial，请用乐鑫 IDF 自带的 python 运行")
        self.ser = serial.Serial(port, 115200, timeout=0.3, write_timeout=3)
        time.sleep(0.3)
        self.ser.reset_input_buffer()

    def send(self, ch):
        if self.dry_run:
            print("  [dry-run] 本应下发 '%s'" % ch)
            return ""
        self.ser.write(ch.encode())
        time.sleep(0.25)
        out = b""
        for _ in range(10):
            out += self.ser.read(256)
            time.sleep(0.15)
        return out.decode("utf-8", "replace")

    def drain(self):
        if self.dry_run or self.ser is None:
            return ""
        out = b""
        for _ in range(6):
            out += self.ser.read(256)
            time.sleep(0.15)
        return out.decode("utf-8", "replace")

    def refresh_state(self):
        """发 's' 读回真实引脚状态，喂给监控台。面板上的灯来自这里，
        不是本地猜的——'GUARD STATUS cut=1 buzzer=0' 是板子自己报的。"""
        if self.dry_run:
            return
        reply = self.send("s")
        for tok in reply.replace("\n", " ").split():
            if tok.startswith("cut="):
                state_set(cut=int(tok[4:] or 0))
            elif tok.startswith("buzzer="):
                state_set(buzzer=int(tok[7:] or 0))

    def close(self):
        if self.ser is not None:
            self.ser.close()


# ---------------------------------------------------------------- 主流程

def handle(ev, cfg, board, log, use_llm):
    print("")
    print("事件: %s" % json.dumps(ev, ensure_ascii=False))

    # 先把事件推上面板，让研判过程在屏幕上是可见的——评委看到的是
    # "证据先到、结论后出"，而不是一步跳到结果。
    state_set(current={"event": ev, "local": None, "llm": None,
                       "final": None, "decided_by": ""})

    l_act, l_why = local_rule(ev)
    print("  本地规则 -> %-5s  %s" % (l_act, l_why))
    with STATE_LOCK:
        STATE["current"]["local"] = {"action": l_act, "reason": l_why}

    llm = ask_llm(cfg, ev, cfg.get("timeout", 20)) if use_llm else None
    if llm:
        print("  大模型   -> %-5s  (conf=%.2f) %s"
              % (llm["action"], llm["confidence"], llm["reason"]))
    with STATE_LOCK:
        STATE["current"]["llm"] = llm

    final, src = l_act, "本地规则"
    escalated = False
    capped = False

    if llm and rank(llm["action"]) > rank(l_act):
        # 升级幅度封顶：一次最多升一级。
        #
        # 为什么要封这一刀：实测同一条事件，同一个模型、temperature=0，
        # 两次运行分别给出 ALARM 和 CUT。大模型本身是不确定的，而 CUT
        # 意味着真停电。不封顶的话，一次幻觉就能让配电柜无故拉闸。
        # 封成单级之后，最坏情况只是多响一次蜂鸣器。
        target = min(rank(llm["action"]), rank(l_act) + 1)
        final = ACTIONS[target]
        escalated = True
        capped = final != llm["action"]
        if capped:
            src = ("大模型升级：本地规则 %s → %s（模型建议 %s，单级封顶）"
                   % (l_act, final, llm["action"]))
            print("  ** 大模型升级：%s → %s（模型建议 %s，按单级封顶）**"
                  % (l_act, final, llm["action"]))
        else:
            src = "大模型升级：本地规则 %s → %s" % (l_act, final)
            print("  ** 大模型升级：本地规则 %s → %s **" % (l_act, final))
    elif llm and rank(llm["action"]) < rank(l_act):
        print("  大模型建议降级，按安全设计忽略——本地判定保留否决权")
        src = "本地规则（已否决大模型的降级建议 %s）" % llm["action"]

    print("  最终决策 -> %s（%s）" % (final, src))
    with STATE_LOCK:
        STATE["current"]["final"] = final
        STATE["current"]["decided_by"] = src
        STATE["current"]["escalated"] = escalated
        STATE["history"].append({
            "ts": datetime.now().strftime("%H:%M:%S"),
            "type": ev.get("type", "?"),
            "severity": int(ev.get("severity", 0)),
            "action": final,
            "esc": escalated,
        })
        STATE["history"][:] = STATE["history"][-10:]

    reply = ""
    cmd = CMD_OF[final]
    if cmd:
        if final == "ALARM":
            state_set(buzzer=1)      # 鸣笛期间面板同步亮起
        reply = board.send(cmd).strip()

        # NSH 风格的回显会把刚发的那个字符也带回来，去掉它再展示
        if reply[:1].lower() == cmd:
            reply = reply[1:].strip()

        if reply:
            print("  板子回报: %s" % reply.replace("\n", " | "))
        state_set(board_reply=reply)
        board.refresh_state()        # 灯的状态以板子回报为准

    log.write(json.dumps({
        "ts": datetime.now().isoformat(timespec="seconds"),
        "event": ev,
        "local": {"action": l_act, "reason": l_why},
        "llm": llm,
        "final": final,
        "decided_by": src,
        "llm_escalated": escalated,
        "escalation_capped": capped,
        "board_reply": reply,
    }, ensure_ascii=False) + "\n")
    log.flush()
    return final


def load_cfg(path):
    cfg = {"api_base": "", "api_key": "", "model": "", "port": "COM3",
           "timeout": 20}
    if path and os.path.exists(path):
        cp = configparser.ConfigParser()
        cp.read(path, encoding="utf-8")
        if cp.has_section("llm"):
            for k in ("api_base", "api_key", "model"):
                if cp.has_option("llm", k):
                    cfg[k] = cp.get("llm", k).strip()
            if cp.has_option("llm", "timeout"):
                cfg["timeout"] = cp.getint("llm", "timeout")
        if cp.has_section("serial") and cp.has_option("serial", "port"):
            cfg["port"] = cp.get("serial", "port").strip()

    # 环境变量优先，便于不把 key 写进文件
    cfg["api_key"] = os.environ.get("GUARD_API_KEY", cfg["api_key"])
    cfg["api_base"] = os.environ.get("GUARD_API_BASE", cfg["api_base"])
    cfg["model"] = os.environ.get("GUARD_MODEL", cfg["model"])
    return cfg


def main():
    ap = argparse.ArgumentParser(description="电脉卫士 大模型断闸决策网关")
    ap.add_argument("--config", default="guard_bridge.ini")
    ap.add_argument("--port", default=None, help="覆盖配置里的串口")
    ap.add_argument("--scenario", default=None, help="事件序列 JSON")
    ap.add_argument("--interactive", action="store_true")
    ap.add_argument("--no-llm", action="store_true", help="只跑本地规则")
    ap.add_argument("--dry-run", action="store_true", help="不碰串口")
    ap.add_argument("--log", default="guard_bridge.log")
    ap.add_argument("--serve", type=int, default=8765,
                    help="监控台端口，0 表示不起（默认 8765）")
    ap.add_argument("--pace", type=float, default=3.0,
                    help="事件之间的停顿秒数，录像时调大好看（默认 3）")
    args = ap.parse_args()

    cfg = load_cfg(args.config)
    if args.port:
        cfg["port"] = args.port

    use_llm = not args.no_llm
    if use_llm and not (cfg["api_key"] and cfg["api_base"] and cfg["model"]):
        print("未配置大模型（api_base/api_key/model），本次只跑本地规则。")
        print("填 %s 或设环境变量 GUARD_API_BASE / GUARD_API_KEY / GUARD_MODEL"
              % args.config)
        use_llm = False

    if args.serve:
        serve(args.serve)
        print("监控台: http://127.0.0.1:%d   （录像时全屏这个页面）"
              % args.serve)

    board = Board(cfg["port"], args.dry_run)
    state_set(connected=not args.dry_run, dry_run=args.dry_run,
              port=cfg["port"], llm_enabled=use_llm, model=cfg["model"])

    banner = board.drain()
    if banner.strip():
        print("板子: %s" % banner.strip().replace("\n", " | "))
        state_set(board_reply=banner.strip())
    board.refresh_state()

    log = open(args.log, "a", encoding="utf-8")
    try:
        if args.scenario:
            # utf-8-sig：记事本和 PowerShell 存出来的文件带 BOM，别让它绊倒
            with open(args.scenario, encoding="utf-8-sig") as fp:
                events = json.load(fp)
            if args.serve:
                print("面板已就绪，%.0f 秒后开始注入事件…" % args.pace)
                time.sleep(args.pace)
            for ev in events:
                handle(ev, cfg, board, log, use_llm)
                time.sleep(args.pace)

            # 演示结束把断闸复位，别让板子停在继电器吸合状态
            print("")
            print("演示结束，复位断闸输出")
            tail = board.send("r").strip()
            if tail[:1].lower() == "r":
                tail = tail[1:].strip()
            print(tail)
            state_set(board_reply=tail)
            board.refresh_state()
            if args.serve:
                print("面板保持在线，按 Ctrl+C 结束")
                try:
                    while True:
                        time.sleep(1)
                except KeyboardInterrupt:
                    pass
        elif args.interactive:
            print("输入 JSON 事件，一行一条；空行退出。例：")
            print('  {"type":"ARC","severity":2,"vision_fire":0.71,'
                  '"ctx":"A相电流 18.2A，柜温 63C"}')
            for line in sys.stdin:
                line = line.strip()
                if not line:
                    break
                try:
                    handle(json.loads(line), cfg, board, log, use_llm)
                except ValueError as exc:
                    print("  JSON 解析失败: %s" % exc)
        else:
            ap.error("需要 --scenario 或 --interactive")
    finally:
        log.close()
        board.close()


if __name__ == "__main__":
    main()
