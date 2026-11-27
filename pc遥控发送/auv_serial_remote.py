"""基于 STM32 串口发送板的四通道 AUV 遥控上位机。"""

from __future__ import annotations

import queue
import threading
import time
import tkinter as tk
from tkinter import messagebox, ttk

import serial
from serial.tools import list_ports


BAUDRATE = 115200
HEARTBEAT_S = 0.25
UI_FONT = "Microsoft YaHei UI"
MONO_FONT = "Consolas"

CHANNELS = (
    {"id": "M1", "name": "左前进", "default_throttle": 60},
    {"id": "M2", "name": "右前进", "default_throttle": 60},
    {"id": "M3", "name": "前上升", "default_throttle": 60},
    {"id": "M4", "name": "后上升", "default_throttle": 60},
)


class SerialLink:
    def __init__(self, on_line, on_state):
        self.on_line = on_line
        self.on_state = on_state
        self.ser: serial.Serial | None = None
        self._write_lock = threading.Lock()
        self._stop_event = threading.Event()
        self._reader: threading.Thread | None = None
        self._buffer = b""

    @property
    def connected(self) -> bool:
        return self.ser is not None and self.ser.is_open

    def connect(self, port: str) -> None:
        self.disconnect(send_stop=False, notify=False)
        self.ser = serial.Serial(
            port=port,
            baudrate=BAUDRATE,
            bytesize=serial.EIGHTBITS,
            parity=serial.PARITY_NONE,
            stopbits=serial.STOPBITS_ONE,
            timeout=0.05,
            write_timeout=0.25,
        )
        self.ser.reset_input_buffer()
        self._buffer = b""
        self._stop_event.clear()
        self._reader = threading.Thread(target=self._read_loop, daemon=True)
        self._reader.start()
        self.send_line("PING")
        self.send_line("STATUS?")
        self.on_state("已连接", True)

    def disconnect(self, send_stop: bool = True, notify: bool = True) -> None:
        was_connected = self.connected
        if send_stop and was_connected:
            try:
                self.send_line("S")
                time.sleep(0.05)
            except serial.SerialException:
                pass

        self._stop_event.set()
        ser = self.ser
        self.ser = None
        if ser is not None:
            try:
                ser.cancel_read()
            except (AttributeError, serial.SerialException):
                pass
            try:
                ser.close()
            except serial.SerialException:
                pass

        reader = self._reader
        if reader is not None and reader is not threading.current_thread():
            reader.join(timeout=0.4)
        self._reader = None
        if notify and was_connected:
            self.on_state("未连接", False)

    def send_line(self, text: str) -> None:
        ser = self.ser
        if ser is None or not ser.is_open:
            raise serial.SerialException("串口尚未连接")
        payload = (text.rstrip("\r\n") + "\n").encode("ascii")
        with self._write_lock:
            ser.write(payload)

    def _read_loop(self) -> None:
        next_heartbeat = time.monotonic() + HEARTBEAT_S
        while not self._stop_event.is_set():
            ser = self.ser
            if ser is None or not ser.is_open:
                break
            try:
                waiting = ser.in_waiting
                chunk = ser.read(waiting if waiting else 1)
                if chunk:
                    self._consume(chunk)
                now = time.monotonic()
                if now >= next_heartbeat:
                    self.send_line("H")
                    next_heartbeat = now + HEARTBEAT_S
            except (serial.SerialException, OSError):
                if not self._stop_event.is_set():
                    self.disconnect(send_stop=False, notify=False)
                    self.on_state("串口异常", False)
                break

    def _consume(self, chunk: bytes) -> None:
        self._buffer += chunk
        while b"\n" in self._buffer:
            raw, self._buffer = self._buffer.split(b"\n", 1)
            line = raw.decode("ascii", errors="replace").strip()
            if line:
                self.on_line(line)


class RemoteApp:
    def __init__(self, root: tk.Tk):
        self.root = root
        self.root.title("AUV 四电机遥控")
        self.root.geometry("1040x720")
        self.root.minsize(940, 660)
        self.root.configure(bg="#eef2f3")
        self.root.protocol("WM_DELETE_WINDOW", self.close)

        self.events: queue.Queue[tuple[str, object]] = queue.Queue()
        self.link = SerialLink(
            on_line=lambda line: self.events.put(("line", line)),
            on_state=lambda text, connected: self.events.put(
                ("state", (text, connected))
            ),
        )

        self.motor_vars = [
            {
                "direction": tk.StringVar(value="S"),
                "throttle": tk.IntVar(value=channel["default_throttle"]),
                "throttle_job": None,
            }
            for channel in CHANNELS
        ]
        self._build_styles()
        self._build_ui()
        self._refresh_ports()
        self.root.bind("<space>", lambda event: self.all_stop())
        self.root.after(50, self._poll_events)
        self._set_connection_state("未连接", False)

    def _build_styles(self) -> None:
        style = ttk.Style(self.root)
        style.theme_use("clam")
        style.configure("TFrame", background="#eef2f3")
        style.configure("Panel.TFrame", background="#ffffff")
        style.configure(
            "TLabel",
            background="#eef2f3",
            foreground="#1f2d33",
            font=(UI_FONT, 10),
        )
        style.configure(
            "Panel.TLabel",
            background="#ffffff",
            foreground="#1f2d33",
            font=(UI_FONT, 10),
        )
        style.configure(
            "Muted.Panel.TLabel",
            background="#ffffff",
            foreground="#65757d",
            font=(UI_FONT, 9),
        )
        style.configure(
            "Value.Panel.TLabel",
            background="#ffffff",
            foreground="#176b78",
            font=(MONO_FONT, 10, "bold"),
        )
        style.configure(
            "ChannelTitle.Panel.TLabel",
            background="#ffffff",
            foreground="#17262e",
            font=(UI_FONT, 14, "bold"),
        )
        style.configure(
            "TButton",
            font=(UI_FONT, 9),
            padding=(10, 7),
        )
        style.configure(
            "Accent.TButton",
            background="#176b78",
            foreground="#ffffff",
            borderwidth=0,
            padding=(13, 9),
        )
        style.map(
            "Accent.TButton",
            background=[("active", "#0e5964"), ("disabled", "#aac0c4")],
        )
        style.configure(
            "Stop.TButton",
            background="#b6424f",
            foreground="#ffffff",
            borderwidth=0,
            padding=(13, 9),
        )
        style.map(
            "Stop.TButton",
            background=[("active", "#94333e"), ("disabled", "#d3b9bd")],
        )
        style.configure(
            "Direction.TRadiobutton",
            background="#ffffff",
            foreground="#293a42",
            font=(UI_FONT, 9),
            padding=(7, 5),
        )
        style.map(
            "Direction.TRadiobutton",
            background=[("selected", "#d9ebee"), ("active", "#e9f2f3")],
            foreground=[("selected", "#0f5964")],
        )
        style.configure(
            "Horizontal.TScale",
            background="#ffffff",
            troughcolor="#dce5e7",
        )

    def _build_ui(self) -> None:
        shell = ttk.Frame(self.root, padding=18)
        shell.pack(fill="both", expand=True)
        shell.columnconfigure(0, weight=1)
        shell.rowconfigure(2, weight=1)

        header = ttk.Frame(shell)
        header.grid(row=0, column=0, sticky="ew", pady=(0, 12))
        header.columnconfigure(0, weight=1)
        ttk.Label(
            header,
            text="AUV 四电机遥控",
            font=(UI_FONT, 20, "bold"),
        ).grid(row=0, column=0, sticky="w")

        self.connection_dot = tk.Canvas(
            header,
            width=16,
            height=16,
            bg="#eef2f3",
            highlightthickness=0,
        )
        self.connection_dot.grid(row=0, column=1, padx=(8, 5))
        self.dot_item = self.connection_dot.create_oval(
            3, 3, 13, 13, fill="#9aa7ad", outline=""
        )
        self.connection_label = ttk.Label(header, text="未连接", style="TLabel")
        self.connection_label.grid(row=0, column=2, sticky="e")

        toolbar = ttk.Frame(shell, style="Panel.TFrame", padding=12)
        toolbar.grid(row=1, column=0, sticky="ew", pady=(0, 12))
        toolbar.columnconfigure(1, weight=1)

        ttk.Label(toolbar, text="串口", style="Muted.Panel.TLabel").grid(
            row=0, column=0, padx=(0, 8)
        )
        self.port_var = tk.StringVar()
        self.port_box = ttk.Combobox(
            toolbar,
            textvariable=self.port_var,
            state="readonly",
            width=17,
        )
        self.port_box.grid(row=0, column=1, sticky="w")
        self.refresh_button = ttk.Button(
            toolbar, text="刷新", command=self._refresh_ports
        )
        self.refresh_button.grid(row=0, column=2, padx=6)
        self.connect_button = ttk.Button(
            toolbar, text="连接", command=self.toggle_connection
        )
        self.connect_button.grid(row=0, column=3, padx=(0, 6))
        self.status_button = ttk.Button(
            toolbar, text="查询状态", command=self.query_status
        )
        self.status_button.grid(row=0, column=4, padx=(0, 6))
        self.stop_all_button = ttk.Button(
            toolbar,
            text="全部停止",
            style="Stop.TButton",
            command=self.all_stop,
        )
        self.stop_all_button.grid(row=0, column=5)

        motor_grid = ttk.Frame(shell)
        motor_grid.grid(row=2, column=0, sticky="nsew")
        for column in range(2):
            motor_grid.columnconfigure(column, weight=1, uniform="motor")
        for row in range(2):
            motor_grid.rowconfigure(row, weight=1, uniform="motor")

        self.motor_widgets: list[list[ttk.Widget]] = []
        self.motor_frames: list[ttk.Frame] = []
        self.local_state_labels: list[ttk.Label] = []
        self.throttle_labels: list[ttk.Label] = []
        self.throttle_scales: list[ttk.Scale] = []

        for index, channel in enumerate(CHANNELS):
            frame = ttk.Frame(motor_grid, style="Panel.TFrame", padding=16)
            self.motor_frames.append(frame)
            frame.grid(
                row=index // 2,
                column=index % 2,
                sticky="nsew",
                padx=(0, 6) if index % 2 == 0 else (6, 0),
                pady=(0, 6) if index < 2 else (6, 0),
            )
            frame.columnconfigure(0, weight=1)
            self._build_motor_panel(frame, index, channel)

        monitor = ttk.Frame(shell, style="Panel.TFrame", padding=12)
        self.monitor_frame = monitor
        monitor.grid(row=3, column=0, sticky="nsew", pady=(12, 0))
        monitor.columnconfigure(0, weight=0, minsize=430)
        monitor.columnconfigure(1, weight=1)
        monitor.rowconfigure(1, weight=1)

        ttk.Label(
            monitor,
            text="发送板反馈",
            style="ChannelTitle.Panel.TLabel",
        ).grid(row=0, column=0, sticky="w", pady=(0, 8))
        ttk.Label(
            monitor,
            text="串口记录",
            style="Muted.Panel.TLabel",
        ).grid(row=0, column=1, sticky="w", padx=(16, 0), pady=(0, 8))

        feedback = ttk.Frame(monitor, style="Panel.TFrame")
        feedback.grid(row=1, column=0, sticky="nsew")
        for column in range(2):
            feedback.columnconfigure(column, weight=1, uniform="feedback")

        self.board_state_labels: list[ttk.Label] = []
        for index, channel in enumerate(CHANNELS):
            cell = ttk.Frame(feedback, style="Panel.TFrame")
            cell.grid(
                row=index // 2,
                column=index % 2,
                sticky="ew",
                padx=(0, 14) if index % 2 == 0 else (0, 0),
                pady=(0, 10),
            )
            ttk.Label(
                cell,
                text=f"{channel['id']}  {channel['name']}",
                style="Muted.Panel.TLabel",
            ).pack(anchor="w")
            value = ttk.Label(cell, text="停止", style="Value.Panel.TLabel")
            value.pack(anchor="w", pady=(2, 0))
            self.board_state_labels.append(value)

        meta = ttk.Frame(feedback, style="Panel.TFrame")
        meta.grid(row=2, column=0, columnspan=2, sticky="ew")
        ttk.Label(meta, text="电脑链路", style="Muted.Panel.TLabel").grid(
            row=0, column=0, sticky="w"
        )
        self.pc_link_label = ttk.Label(meta, text="-", style="Value.Panel.TLabel")
        self.pc_link_label.grid(row=1, column=0, sticky="w", padx=(0, 28))
        ttk.Label(meta, text="运行时间", style="Muted.Panel.TLabel").grid(
            row=0, column=1, sticky="w"
        )
        self.uptime_label = ttk.Label(meta, text="-", style="Value.Panel.TLabel")
        self.uptime_label.grid(row=1, column=1, sticky="w", padx=(0, 28))
        ttk.Label(meta, text="刷新周期", style="Muted.Panel.TLabel").grid(
            row=0, column=2, sticky="w"
        )
        ttk.Label(meta, text="200 ms", style="Value.Panel.TLabel").grid(
            row=1, column=2, sticky="w"
        )

        log_frame = tk.Frame(monitor, bg="#1b252b", highlightthickness=0)
        log_frame.grid(row=1, column=1, sticky="nsew", padx=(16, 0))
        log_frame.rowconfigure(0, weight=1)
        log_frame.columnconfigure(0, weight=1)
        self.log_text = tk.Text(
            log_frame,
            bg="#1b252b",
            fg="#d8e3e6",
            insertbackground="#d8e3e6",
            font=(MONO_FONT, 9),
            relief="flat",
            padx=10,
            pady=10,
            wrap="word",
            state="disabled",
            height=9,
        )
        self.log_text.grid(row=0, column=0, sticky="nsew")
        scrollbar = ttk.Scrollbar(
            log_frame, orient="vertical", command=self.log_text.yview
        )
        scrollbar.grid(row=0, column=1, sticky="ns")
        self.log_text.configure(yscrollcommand=scrollbar.set)

    def _build_motor_panel(
        self, frame: ttk.Frame, index: int, channel: dict[str, object]
    ) -> None:
        header = ttk.Frame(frame, style="Panel.TFrame")
        header.grid(row=0, column=0, sticky="ew")
        header.columnconfigure(1, weight=1)
        ttk.Label(
            header,
            text=str(channel["id"]),
            style="Muted.Panel.TLabel",
        ).grid(row=0, column=0, sticky="w", padx=(0, 8))
        ttk.Label(
            header,
            text=str(channel["name"]),
            style="ChannelTitle.Panel.TLabel",
        ).grid(row=0, column=1, sticky="w")
        local_state = ttk.Label(header, text="停止", style="Value.Panel.TLabel")
        local_state.grid(row=0, column=2, sticky="e")
        self.local_state_labels.append(local_state)

        direction_row = ttk.Frame(frame, style="Panel.TFrame")
        direction_row.grid(row=1, column=0, sticky="ew", pady=(12, 10))
        direction_widgets = []
        for column, (text, value) in enumerate(
            (("正转", "F"), ("停止", "S"), ("反转", "R"))
        ):
            button = ttk.Radiobutton(
                direction_row,
                text=text,
                value=value,
                variable=self.motor_vars[index]["direction"],
                style="Direction.TRadiobutton",
                command=lambda i=index: self._direction_changed(i),
            )
            button.grid(row=0, column=column, sticky="w", padx=(0, 8))
            direction_widgets.append(button)

        throttle_header = ttk.Frame(frame, style="Panel.TFrame")
        throttle_header.grid(row=2, column=0, sticky="ew")
        throttle_header.columnconfigure(1, weight=1)
        ttk.Label(
            throttle_header,
            text="油门",
            style="Muted.Panel.TLabel",
        ).grid(row=0, column=0, sticky="w")
        throttle_label = ttk.Label(
            throttle_header,
            text=f"{self.motor_vars[index]['throttle'].get()}%",
            style="Value.Panel.TLabel",
        )
        throttle_label.grid(row=0, column=2, sticky="e")
        self.throttle_labels.append(throttle_label)

        slider_row = ttk.Frame(frame, style="Panel.TFrame")
        slider_row.grid(row=3, column=0, sticky="ew", pady=(7, 0))
        slider_row.columnconfigure(1, weight=1)
        minus = ttk.Button(
            slider_row,
            text="-",
            width=3,
            command=lambda i=index: self._step_throttle(i, -5),
        )
        minus.grid(row=0, column=0)
        scale = ttk.Scale(
            slider_row,
            from_=1,
            to=100,
            orient="horizontal",
            variable=self.motor_vars[index]["throttle"],
            command=lambda raw, i=index: self._throttle_changed(i, raw),
        )
        scale.set(self.motor_vars[index]["throttle"].get())
        scale.grid(row=0, column=1, sticky="ew", padx=8)
        plus = ttk.Button(
            slider_row,
            text="+",
            width=3,
            command=lambda i=index: self._step_throttle(i, 5),
        )
        plus.grid(row=0, column=2)
        self.throttle_scales.append(scale)

        widgets: list[ttk.Widget] = list(direction_widgets)
        widgets.extend((minus, scale, plus))
        self.motor_widgets.append(widgets)

    def _refresh_ports(self) -> None:
        ports = sorted(port.device for port in list_ports.comports())
        self.port_box["values"] = ports
        if ports and self.port_var.get() not in ports:
            self.port_var.set(ports[0])
        elif not ports:
            self.port_var.set("")

    def toggle_connection(self) -> None:
        if self.link.connected:
            self.link.disconnect()
            self._mark_all_stopped()
            return

        port = self.port_var.get()
        if not port:
            messagebox.showwarning("串口", "请选择一个串口。")
            return
        try:
            self.link.connect(port)
        except (serial.SerialException, OSError) as exc:
            self._set_connection_state("未连接", False)
            messagebox.showerror("串口连接失败", str(exc))
            return
        self._append_log(f"打开 {port}  {BAUDRATE}")

    def query_status(self) -> None:
        if not self.link.connected:
            return
        try:
            self.link.send_line("STATUS?")
        except (serial.SerialException, OSError) as exc:
            self._handle_serial_error(exc)

    def all_stop(self) -> None:
        if self.link.connected:
            try:
                self.link.send_line("S")
            except (serial.SerialException, OSError) as exc:
                self._handle_serial_error(exc)
        self._mark_all_stopped()

    def _mark_all_stopped(self) -> None:
        for index in range(len(CHANNELS)):
            self.motor_vars[index]["direction"].set("S")
            self.local_state_labels[index].configure(
                text="停止", foreground="#b6424f"
            )

    def _direction_changed(self, index: int) -> None:
        self._update_local_motor_label(index)
        self._send_motor(index)

    def _throttle_changed(self, index: int, raw: str) -> None:
        value = max(1, min(100, int(round(float(raw)))))
        self.motor_vars[index]["throttle"].set(value)
        self.throttle_labels[index].configure(text=f"{value}%")
        job = self.motor_vars[index]["throttle_job"]
        if job is not None:
            self.root.after_cancel(job)
        self.motor_vars[index]["throttle_job"] = self.root.after(
            100, lambda i=index: self._send_motor(i)
        )

    def _step_throttle(self, index: int, delta: int) -> None:
        value = max(
            1,
            min(100, self.motor_vars[index]["throttle"].get() + delta),
        )
        self.throttle_scales[index].set(value)
        self._throttle_changed(index, str(value))

    def _send_motor(self, index: int) -> None:
        self.motor_vars[index]["throttle_job"] = None
        if self.link.connected:
            direction = self.motor_vars[index]["direction"].get()
            throttle = self.motor_vars[index]["throttle"].get()
            try:
                self.link.send_line(f"M,{index + 1},{direction},{throttle}")
            except (serial.SerialException, OSError) as exc:
                self._handle_serial_error(exc)
            return
        self._update_local_motor_label(index)

    def _update_local_motor_label(self, index: int) -> None:
        direction = self.motor_vars[index]["direction"].get()
        text = {"F": "正转", "S": "停止", "R": "反转"}[direction]
        color = "#176b78" if direction != "S" else "#b6424f"
        self.local_state_labels[index].configure(text=text, foreground=color)

    def _poll_events(self) -> None:
        try:
            while True:
                event, payload = self.events.get_nowait()
                if event == "line":
                    self._handle_board_line(str(payload))
                elif event == "state":
                    text, connected = payload
                    self._set_connection_state(str(text), bool(connected))
        except queue.Empty:
            pass
        self.root.after(50, self._poll_events)

    def _handle_board_line(self, line: str) -> None:
        self._append_log(line)
        if not line.startswith("STATUS,"):
            if line.startswith("ERR,PC_TIMEOUT"):
                self.pc_link_label.configure(text="等待")
            return

        values = {}
        for item in line.split(",")[1:]:
            if "=" in item:
                key, value = item.split("=", 1)
                values[key] = value

        for index, channel in enumerate(CHANNELS):
            value = values.get(channel["id"])
            if value:
                self.board_state_labels[index].configure(
                    text=self._translate_motor_state(value)
                )
        self.pc_link_label.configure(
            text="正常" if values.get("pc") == "1" else "等待"
        )
        try:
            uptime = int(values.get("uptime", "0")) // 1000
            self.uptime_label.configure(text=f"{uptime} 秒")
        except ValueError:
            self.uptime_label.configure(text="-")

    @staticmethod
    def _translate_motor_state(value: str) -> str:
        direction = value[:1]
        throttle = value[1:]
        names = {"F": "正转", "S": "停止", "R": "反转"}
        return f"{names.get(direction, '未知')} {throttle}%"

    def _set_connection_state(self, text: str, connected: bool) -> None:
        self.connection_label.configure(text=text)
        self.connection_dot.itemconfigure(
            self.dot_item,
            fill="#1f8a70" if connected else "#9aa7ad",
        )
        if connected:
            self.connect_button.configure(text="断开")
            self.port_box.configure(state="disabled")
            self.refresh_button.configure(state="disabled")
        else:
            self.connect_button.configure(text="连接")
            self.port_box.configure(state="readonly")
            self.refresh_button.configure(state="normal")
            self.pc_link_label.configure(text="等待")
        self._set_channel_controls_enabled(connected)
        self.status_button.configure(state="normal" if connected else "disabled")
        self.stop_all_button.configure(state="normal" if connected else "disabled")

    def _set_channel_controls_enabled(self, enabled: bool) -> None:
        state = "normal" if enabled else "disabled"
        for widgets in self.motor_widgets:
            for widget in widgets:
                widget.configure(state=state)

    def _append_log(self, line: str) -> None:
        self.log_text.configure(state="normal")
        self.log_text.insert("end", line + "\n")
        line_count = int(self.log_text.index("end-1c").split(".")[0])
        if line_count > 320:
            self.log_text.delete("1.0", f"{line_count - 300}.0")
        self.log_text.see("end")
        self.log_text.configure(state="disabled")

    def _handle_serial_error(self, exc: Exception) -> None:
        self._append_log(f"串口异常：{exc}")
        self.link.disconnect(send_stop=False, notify=False)
        self._set_connection_state("串口异常", False)

    def close(self) -> None:
        try:
            self.link.disconnect()
        finally:
            self.root.destroy()


def main() -> None:
    root = tk.Tk()
    RemoteApp(root)
    root.mainloop()


if __name__ == "__main__":
    main()
