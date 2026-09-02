#!/usr/bin/env python3
"""Grab the CYD framebuffer as a PNG. usage: tools/screenshot.py out.png [--page N]"""
import sys, subprocess, pathlib
args = sys.argv[1:]
out = args[0] if args else "screen.png"
cmds = []
if "--page" in args:
    cmds.append(f"p {args[args.index('--page') + 1]}")
cmds.append(f"shot {out}")
sys.exit(subprocess.call([sys.executable, str(pathlib.Path(__file__).with_name("cyd.py")), *cmds]))
