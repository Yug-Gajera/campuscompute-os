#!/usr/bin/env python3
"""
CampusCompute GUI - a graphical front-end for the ./campuscompute C program.

All OS work (scheduling, pthreads, shared memory, semaphores, Banker's,
fork/waitpid, setrlimit) is done by the C program. This GUI only runs its
commands, streams their real output into a window, and draws Gantt charts
from the scheduler output. Nothing shown here is simulated by Python.

Run from the project folder:
    sudo apt install python3-tk      # once
    make                             # build the C program
    python3 gui/campuscompute_gui.py # or: make gui
"""
import glob
import os
import queue
import re
import subprocess
import threading
import tkinter as tk
from tkinter import ttk, messagebox

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BINARY = os.path.join(ROOT, "campuscompute")

BG, PANEL, FG, DIM = "#1e1f29", "#272935", "#e6e6e6", "#8a8fa3"
ACCENT, GOOD, BAD, WARN = "#7aa2f7", "#9ece6a", "#f7768e", "#e0af68"
JOB_COLORS = ["#7aa2f7", "#9ece6a", "#e0af68", "#bb9af7", "#7dcfff", "#f7768e",
              "#73daca", "#ff9e64", "#c0caf5", "#2ac3de"]
SLICE_RE = re.compile(r"(J(\d+)|IDLE)\[(\d+)-(\d+)\]")


def parse_timeline(text):
    """Return [(label, start, end), ...] from an 'Execution order:' line."""
    for line in text.splitlines():
        if "Execution order:" in line:
            return [(m.group(1), int(m.group(3)), int(m.group(4))) for m in SLICE_RE.finditer(line)]
    return []


def parse_averages(text):
    vals = {}
    for key, label in (("tat", "Average turnaround time"), ("wt", "Average waiting time"),
                       ("rt", "Average response time")):
        m = re.search(label + r"\s*:\s*([\d.]+)", text)
        if m:
            vals[key] = m.group(1)
    return vals


class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("CampusCompute - Intelligent Job Scheduling and Resource Management")
        self.geometry("1340x840")
        self.minsize(1050, 680)
        self.configure(bg=BG)
        self.out_q = queue.Queue()
        self.running = False
        self.buttons = []
        self._style()
        self._build()
        self.after(40, self._drain)
        if not os.path.exists(BINARY):
            self._log(f"'{BINARY}' not found - click 'Build (make)' first.\n", "bad")
        else:
            self._log("Ready. Pick a feature on the left. Every result comes from the C program "
                      "./campuscompute; the exact command is shown before its output.\n", "dim")

    # ---------------------------------------------------------------- layout
    def _style(self):
        s = ttk.Style(self)
        s.theme_use("clam")
        s.configure(".", background=BG, foreground=FG, fieldbackground=PANEL)
        s.configure("TFrame", background=BG)
        s.configure("Card.TFrame", background=PANEL)
        s.configure("TLabel", background=PANEL, foreground=FG, font=("Sans", 10))
        s.configure("Head.TLabel", background=PANEL, foreground=ACCENT, font=("Sans", 11, "bold"))
        s.configure("Title.TLabel", background=BG, foreground=FG, font=("Sans", 15, "bold"))
        s.configure("Sub.TLabel", background=BG, foreground=DIM, font=("Sans", 10))
        s.configure("Status.TLabel", background=BG, foreground=DIM, font=("Sans", 10))
        s.configure("TButton", background="#3b3f54", foreground=FG, padding=(8, 4), borderwidth=0)
        s.map("TButton", background=[("active", "#4b5070"), ("disabled", "#2c2e3b")],
              foreground=[("disabled", "#666a7d")])
        s.configure("Accent.TButton", background="#3d59a1")
        s.map("Accent.TButton", background=[("active", "#4a6bc0")])
        s.configure("TCombobox", fieldbackground="#33364a", background="#33364a", foreground=FG,
                    arrowcolor=FG)
        s.configure("TSpinbox", fieldbackground="#33364a", background="#33364a", foreground=FG,
                    arrowcolor=FG)
        s.map("TCombobox", fieldbackground=[("readonly", "#33364a")], foreground=[("readonly", FG)],
              selectbackground=[("readonly", "#33364a")], selectforeground=[("readonly", FG)])
        s.configure("Vertical.TScrollbar", background="#3b3f54", troughcolor=BG, arrowcolor=FG, borderwidth=0)
        s.configure("Horizontal.TScrollbar", background="#3b3f54", troughcolor=BG, arrowcolor=FG, borderwidth=0)
        self.option_add("*TCombobox*Listbox.background", "#33364a")
        self.option_add("*TCombobox*Listbox.foreground", FG)

    def _build(self):
        top = ttk.Frame(self, padding=(14, 10, 14, 4))
        top.pack(fill="x")
        ttk.Label(top, text="CampusCompute", style="Title.TLabel").pack(side="left")
        ttk.Label(top, text="   OS Level 3 Mini-Project  |  GUI front-end for ./campuscompute",
                  style="Sub.TLabel").pack(side="left", pady=(4, 0))
        b = ttk.Button(top, text="Build (make)", command=lambda: self.run(["make"], raw=True))
        b.pack(side="right")
        self.buttons.append(b)

        body = ttk.Frame(self, padding=(14, 4, 14, 8))
        body.pack(fill="both", expand=True)

        # left: scrollable control column
        left_wrap = ttk.Frame(body, width=375)
        left_wrap.pack(side="left", fill="y")
        left_wrap.pack_propagate(False)
        canvas = tk.Canvas(left_wrap, bg=BG, highlightthickness=0, width=360)
        sb = ttk.Scrollbar(left_wrap, orient="vertical", command=canvas.yview)
        self.left = ttk.Frame(canvas)
        self.left.bind("<Configure>", lambda e: canvas.configure(scrollregion=canvas.bbox("all")))
        canvas.create_window((0, 0), window=self.left, anchor="nw", width=352)
        canvas.configure(yscrollcommand=sb.set)
        canvas.pack(side="left", fill="both", expand=True)
        sb.pack(side="right", fill="y")
        # mouse-wheel scrolls the control column only while the pointer is over it
        def wheel_on(_):
            canvas.bind_all("<Button-4>", lambda e: canvas.yview_scroll(-2, "units"))
            canvas.bind_all("<Button-5>", lambda e: canvas.yview_scroll(2, "units"))
            canvas.bind_all("<MouseWheel>", lambda e: canvas.yview_scroll(-1 if e.delta > 0 else 1, "units"))

        def wheel_off(_):
            for ev in ("<Button-4>", "<Button-5>", "<MouseWheel>"):
                canvas.unbind_all(ev)
        left_wrap.bind("<Enter>", wheel_on)
        left_wrap.bind("<Leave>", wheel_off)

        jobfiles = sorted(os.path.relpath(p, ROOT) for p in glob.glob(os.path.join(ROOT, "tests", "jobs_*.txt")))
        bankfiles = sorted(os.path.relpath(p, ROOT) for p in glob.glob(os.path.join(ROOT, "tests", "banker_*.txt")))
        self.jobfile = tk.StringVar(value="tests/jobs_sample.txt")
        self.quantum = tk.StringVar(value="2")
        self.bankfile = tk.StringVar(value="tests/banker_safe.txt")
        self.prod, self.events, self.cap = tk.StringVar(value="3"), tk.StringVar(value="5"), tk.StringVar(value="4")
        self.wcount, self.wfail = tk.StringVar(value="3"), tk.StringVar(value="all")
        self.rlim = tk.StringVar(value="all")

        c = self._card("FR1 / FR2  Jobs & CPU scheduling")
        self._row(c, "Job file", lambda r: ttk.Combobox(r, textvariable=self.jobfile, values=jobfiles, width=24))
        self._row(c, "RR quantum", lambda r: ttk.Spinbox(r, from_=1, to=100, textvariable=self.quantum, width=6))
        self._btns(c, [("Jobs", lambda: self.run(["jobs", self.jobfile.get()])),
                       ("FCFS", lambda: self.sched("fcfs")),
                       ("RR", lambda: self.sched("rr"))])
        self._btns(c, [("Priority", lambda: self.sched("priority")),
                       ("Compare all", self.compare, "Accent.TButton")])

        c = self._card("FR3  Threaded logger (pthreads)")
        self._row(c, "Producers", lambda r: ttk.Spinbox(r, from_=1, to=16, textvariable=self.prod, width=6))
        self._row(c, "Events each", lambda r: ttk.Spinbox(r, from_=1, to=100, textvariable=self.events, width=6))
        self._row(c, "Buffer size", lambda r: ttk.Spinbox(r, from_=1, to=64, textvariable=self.cap, width=6))
        self._btns(c, [("Run logger", lambda: self.run(["logger", self.prod.get(), self.events.get(), self.cap.get()]))])

        c = self._card("FR4  Shared-memory IPC")
        self._btns(c, [("Run IPC", lambda: self.run(["ipc", self.jobfile.get()]))])

        c = self._card("FR5  Banker's algorithm")
        self._row(c, "State file", lambda r: ttk.Combobox(r, textvariable=self.bankfile, values=bankfiles, width=24))
        self._btns(c, [("Check safety", lambda: self.run(["banker", self.bankfile.get()]))])

        c = self._card("FR6  Worker processes")
        self._row(c, "Workers", lambda r: ttk.Spinbox(r, from_=1, to=8, textvariable=self.wcount, width=6))
        self._row(c, "Failure type", lambda r: ttk.Combobox(r, textvariable=self.wfail, width=10,
                                                  values=["all", "exit", "signal", "hang"], state="readonly"))
        self._btns(c, [("Normal", lambda: self.run(["workers", "normal", self.wcount.get()])),
                       ("Simulate failure", lambda: self.run(["workers", "fail", self.wfail.get()]))])

        c = self._card("FR7  Resource limits & cleanup")
        self._row(c, "Limit", lambda r: ttk.Combobox(r, textvariable=self.rlim, width=10,
                                           values=["all", "cpu", "fsize", "mem"], state="readonly"))
        self._btns(c, [("Apply limit", lambda: self.run(["rlimit", self.rlim.get()])),
                       ("Cleanup check", lambda: self.run(["cleanup-check"]))])

        c = self._card("Integrated / FR8")
        self._btns(c, [("Run full demo", lambda: self.run(["demo", self.jobfile.get(), self.bankfile.get()]),
                        "Accent.TButton"),
                       ("Help", lambda: self.run(["help"]))])
        self._btns(c, [("Invalid input test", self.invalid_tests),
                       ("Show log", lambda: self.run(["tail", "-n", "40", "logs/campuscompute.log"], raw=True))])

        # right: gantt + output
        right = ttk.Frame(body)
        right.pack(side="left", fill="both", expand=True, padx=(12, 0))

        gcard = ttk.Frame(right, style="Card.TFrame", padding=10)
        gcard.pack(fill="x")
        ttk.Label(gcard, text="Gantt chart (drawn from the C program's execution order)",
                  style="Head.TLabel").pack(anchor="w")
        self.gantt = tk.Canvas(gcard, height=170, bg=PANEL, highlightthickness=0)
        self.gantt.pack(fill="x", pady=(6, 0))
        self.gantt.bind("<Configure>", lambda e: self._redraw())
        self.timelines = []

        ocard = ttk.Frame(right, style="Card.TFrame", padding=10)
        ocard.pack(fill="both", expand=True, pady=(10, 0))
        hdr = ttk.Frame(ocard, style="Card.TFrame")
        hdr.pack(fill="x")
        ttk.Label(hdr, text="Program output", style="Head.TLabel").pack(side="left")
        ttk.Button(hdr, text="Clear", command=self._clear).pack(side="right")
        txt_wrap = ttk.Frame(ocard, style="Card.TFrame")
        txt_wrap.pack(fill="both", expand=True, pady=(6, 0))
        self.text = tk.Text(txt_wrap, bg="#16171f", fg=FG, insertbackground=FG, wrap="none",
                            font=("DejaVu Sans Mono", 10), relief="flat", padx=8, pady=6)
        ys = ttk.Scrollbar(txt_wrap, orient="vertical", command=self.text.yview)
        xs = ttk.Scrollbar(txt_wrap, orient="horizontal", command=self.text.xview)
        self.text.configure(yscrollcommand=ys.set, xscrollcommand=xs.set)
        ys.pack(side="right", fill="y")
        xs.pack(side="bottom", fill="x")
        self.text.pack(side="left", fill="both", expand=True)
        for tag, color in (("good", GOOD), ("bad", BAD), ("warn", WARN), ("dim", DIM),
                           ("head", ACCENT), ("cmd", "#7dcfff")):
            self.text.tag_configure(tag, foreground=color)
        self.text.tag_configure("head", font=("DejaVu Sans Mono", 10, "bold"))
        self.text.tag_configure("cmd", font=("DejaVu Sans Mono", 10, "bold"))

        self.status = ttk.Label(self, text="Idle", style="Status.TLabel", padding=(14, 0, 14, 8))
        self.status.pack(fill="x")

    def _card(self, title):
        f = ttk.Frame(self.left, style="Card.TFrame", padding=10)
        f.pack(fill="x", pady=(0, 8))
        ttk.Label(f, text=title, style="Head.TLabel").pack(anchor="w", pady=(0, 6))
        return f

    def _row(self, parent, label, make):
        r = ttk.Frame(parent, style="Card.TFrame")
        r.pack(fill="x", pady=2)
        ttk.Label(r, text=label, width=12).pack(side="left")
        make(r).pack(side="left")

    def _btns(self, parent, specs):
        r = ttk.Frame(parent, style="Card.TFrame")
        r.pack(fill="x", pady=(6, 0))
        for spec in specs:
            b = ttk.Button(r, text=spec[0], command=spec[1], style=spec[2] if len(spec) > 2 else "TButton")
            b.pack(side="left", padx=(0, 6))
            self.buttons.append(b)

    # ---------------------------------------------------------------- output
    def _log(self, line, tag=None):
        if tag is None:
            low = line.lower()
            if "campuscompute |" in low:
                tag = "head"
            elif line.startswith("====") or line.strip().startswith("----"):
                tag = "dim"
            elif ("error" in low or "FAIL" in line or "UNSAFE" in line or "STALE" in line
                  or ("failed" in low and " 0 failed" not in low)):
                tag = "bad"
            elif "SIMULATED" in line or "watchdog" in low or "blocked on" in low or "waiting on" in low:
                tag = "warn"
            elif ("PASS" in line or "SAFE state" in line or "CLEAN" in line or "COMPLETED" in line
                  or "Safe sequence" in line):
                tag = "good"
        self.text.insert("end", line, tag)
        self.text.see("end")

    def _clear(self):
        self.text.delete("1.0", "end")

    def _drain(self):
        try:
            while True:
                kind, payload = self.out_q.get_nowait()
                if kind == "line":
                    self._log(payload)
                elif kind == "done":
                    rc, cmd, after = payload
                    msg = {0: "finished OK", 1: "runtime failure", 2: "invalid usage/input"}.get(rc, "exit")
                    self._log(f"\n[exit status {rc}: {msg}]\n\n", "good" if rc == 0 else "bad")
                    self.status.configure(text=f"Last command: {cmd}   ->   exit {rc} ({msg})")
                    self._set_running(False)
                    if after:
                        after()
                elif kind == "timelines":
                    self.timelines = payload
                    self._redraw()
        except queue.Empty:
            pass
        self.after(40, self._drain)

    def _set_running(self, on):
        self.running = on
        for b in self.buttons:
            b.state(["disabled"] if on else ["!disabled"])
        if on:
            self.status.configure(text="Running...")

    # ---------------------------------------------------------------- running
    def run(self, args, raw=False, after=None):
        """Run ./campuscompute <args> (or a raw command) and stream its output."""
        if self.running:
            return
        if not raw and not os.path.exists(BINARY):
            messagebox.showerror("Not built", "Build the program first (Build (make) button or run 'make').")
            return
        cmd = args if raw else [BINARY] + args
        shown = " ".join(args if raw else ["./campuscompute"] + args)
        self._log(f"$ {shown}\n", "cmd")
        self._set_running(True)

        def worker():
            try:
                p = subprocess.Popen(cmd, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     text=True, bufsize=1)
                for line in p.stdout:
                    self.out_q.put(("line", line))
                rc = p.wait()
            except OSError as e:
                self.out_q.put(("line", f"cannot run command: {e}\n"))
                rc = 1
            self.out_q.put(("done", (rc, shown, after)))
        threading.Thread(target=worker, daemon=True).start()

    def _quiet(self, args):
        r = subprocess.run([BINARY] + args, cwd=ROOT, capture_output=True, text=True)
        return r.stdout

    def _quantum(self):
        q = self.quantum.get().strip()
        return q if q.isdigit() and 1 <= int(q) <= 100 else "2"

    def sched(self, policy):
        args = [policy, self.jobfile.get()] + ([self._quantum()] if policy == "rr" else [])
        name = {"fcfs": "FCFS", "rr": f"RR (q={self._quantum()})", "priority": "Priority"}[policy]

        def draw():
            out = self._quiet(args)
            tl = parse_timeline(out)
            self.out_q.put(("timelines", [(name, tl, parse_averages(out))] if tl else []))
        self.run(args, after=lambda: threading.Thread(target=draw, daemon=True).start())

    def compare(self):
        jf, q = self.jobfile.get(), self._quantum()

        def draw():
            rows = []
            for pol, name in (("fcfs", "FCFS"), ("rr", f"RR (q={q})"), ("priority", "Priority")):
                out = self._quiet([pol, jf] + ([q] if pol == "rr" else []))
                tl = parse_timeline(out)
                if tl:
                    rows.append((name, tl, parse_averages(out)))
            self.out_q.put(("timelines", rows))
        self.run(["compare", jf, q], after=lambda: threading.Thread(target=draw, daemon=True).start())

    def invalid_tests(self):
        seq = [["frobnicate"], ["rr", self.jobfile.get(), "0"], ["jobs", "tests/jobs_invalid.txt"]]

        def step(i=0):
            if i < len(seq):
                self.run(seq[i], after=lambda: step(i + 1))
        step()

    # ---------------------------------------------------------------- gantt
    def _redraw(self):
        g = self.gantt
        g.delete("all")
        w = max(g.winfo_width(), 400)
        if not self.timelines:
            g.configure(height=60)
            g.create_text(w / 2, 30, text="Run FCFS, RR, Priority or Compare all to see the timeline here.",
                          fill=DIM, font=("Sans", 10))
            return
        row_h, top, left, right = 34, 8, 200, 20
        rows = self.timelines
        g.configure(height=top + len(rows) * (row_h + 22) + 6)
        end = max(e for _, tl, _ in rows for _, _, e in tl) or 1
        scale = (w - left - right) / end
        for r, (name, tl, avg) in enumerate(rows):
            y = top + r * (row_h + 22)
            g.create_text(8, y + row_h / 2 - 6, text=name, anchor="w", fill=FG, font=("Sans", 10, "bold"))
            if avg:
                g.create_text(8, y + row_h / 2 + 9, anchor="w", fill=DIM, font=("Sans", 8),
                              text=f"TAT {avg.get('tat', '-')}  WT {avg.get('wt', '-')}  RT {avg.get('rt', '-')}")
            ticks = set()
            for label, s, e in tl:
                x0, x1 = left + s * scale, left + e * scale
                if label == "IDLE":
                    g.create_rectangle(x0, y, x1, y + row_h, fill="#2c2e3b", outline="#555a70", dash=(3, 2))
                    color_txt = DIM
                else:
                    color = JOB_COLORS[int(label[1:]) % len(JOB_COLORS)]
                    g.create_rectangle(x0, y, x1, y + row_h, fill=color, outline=PANEL, width=2)
                    color_txt = "#16171f"
                if x1 - x0 > 18:
                    g.create_text((x0 + x1) / 2, y + row_h / 2, text=label, fill=color_txt,
                                  font=("Sans", 9, "bold"))
                ticks.update((s, e))
            last_x = -100
            for t in sorted(ticks):
                x = left + t * scale
                if x - last_x >= 16:
                    g.create_text(x, y + row_h + 9, text=str(t), fill=DIM, font=("Sans", 8))
                    last_x = x


if __name__ == "__main__":
    App().mainloop()
