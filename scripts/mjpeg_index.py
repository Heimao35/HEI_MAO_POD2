#!/usr/bin/env python3
"""为裸 MJPEG 生成可选帧位置索引（.vix）。

裸 MJPEG 没有逐帧时间戳，而且通常是变码率；播放器拖动进度条时如果按
文件字节比例定位画面、同时按时间比例定位音轨，两者会不一致。本工具用
ffprobe 读出每一帧的 JPEG SOI 偏移，按固定帧间隔（默认 1 秒）保存稀疏
检查点。固件据此把“时间”换算成精确的帧号与字节偏移，拖动后音画仍对齐。

用法：
    python scripts/mjpeg_index.py "MEDIA/video/MyClip_24fps/MyClip_24fps.mjpeg"
    python scripts/mjpeg_index.py video.mjpeg --fps 30 --interval 1.0

帧率优先取文件名中的 24fps/30fps/60fps；没有标记时必须用 --fps 指定。
输出与视频同目录、同主文件名的 .vix（放在视频文件夹内）。
"""
import argparse
import json
import os
import re
import struct
import subprocess
import sys

MAGIC = b"HMVX"
VERSION = 1
HEADER = struct.Struct("<4sHHIIIII")  # magic, version, reserved, frame_us,
                                      # total_frames, interval, count, file_size


def infer_fps(video, override):
    if override:
        return override
    match = re.search(r"(24|30|60)fps", os.path.basename(video), re.IGNORECASE)
    if match:
        return int(match.group(1))
    sys.exit("无法从文件名识别帧率；请用 --fps 24/30/60 指定")


def packet_positions(ffprobe, video, fps):
    cmd = [
        ffprobe, "-v", "error", "-f", "mjpeg", "-framerate", str(fps),
        "-show_entries", "packet=pos", "-of", "json", video,
    ]
    try:
        result = subprocess.run(cmd, check=True, capture_output=True, text=True)
    except FileNotFoundError:
        sys.exit("找不到 ffprobe；用 --ffprobe 指定完整路径")
    except subprocess.CalledProcessError as exc:
        sys.exit("ffprobe 失败：%s" % (exc.stderr.strip() or exc.returncode))
    packets = json.loads(result.stdout).get("packets", [])
    positions = []
    for packet in packets:
        pos = packet.get("pos")
        if pos is None:
            continue
        pos = int(pos)
        if positions and pos <= positions[-1]:
            sys.exit("帧偏移不是严格递增，文件可能损坏")
        positions.append(pos)
    if not positions:
        sys.exit("ffprobe 没有解析到任何帧")
    return positions


def main():
    parser = argparse.ArgumentParser(description="生成裸 MJPEG 的 .vix 帧位置索引")
    parser.add_argument("video", help="裸 MJPEG 文件（.mjpeg/.mjpg）")
    parser.add_argument("--fps", type=int, choices=(24, 30, 60),
                        help="帧率；默认从文件名中的 24fps/30fps/60fps 识别")
    parser.add_argument("--interval", type=float, default=1.0,
                        help="检查点间隔秒数，默认 1.0（越小拖动越精确、索引越大）")
    parser.add_argument("--ffprobe", default="ffprobe", help="ffprobe 可执行文件")
    args = parser.parse_args()

    if not os.path.isfile(args.video):
        sys.exit("文件不存在：%s" % args.video)
    fps = infer_fps(args.video, args.fps)
    frame_us = round(1_000_000 / fps)
    size = os.path.getsize(args.video)
    if size <= 0:
        sys.exit("视频为空")
    if size >= 2 ** 32:
        sys.exit("FAT32 单文件上限为 4 GiB-1，无法索引此文件")

    positions = packet_positions(args.ffprobe, args.video, fps)
    if any(pos >= size for pos in positions):
        sys.exit("ffprobe 返回了越界偏移")

    step = max(1, round(fps * args.interval))
    checkpoints = positions[::step]
    total = len(positions)
    out_path = os.path.splitext(args.video)[0] + ".vix"
    with open(out_path, "wb") as out:
        out.write(HEADER.pack(MAGIC, VERSION, 0, frame_us, total,
                              step, len(checkpoints), size))
        out.write(struct.pack("<%dI" % len(checkpoints), *checkpoints))

    print("%s: %d 帧, %dfps, %d 个检查点, %d 字节"
          % (out_path, total, fps, len(checkpoints),
             HEADER.size + 4 * len(checkpoints)))


if __name__ == "__main__":
    main()
