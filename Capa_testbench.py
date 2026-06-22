import sys, serial, serial.tools.list_ports
import csv
import time
import os
from datetime import datetime
from PyQt5.QtWidgets import *
from PyQt5.QtCore import *
import pyqtgraph as pg
import collections

# mpl for post-rec plt
try:
    import matplotlib.pyplot as plt
    MATPLOTLIB_AVAILABLE = True
except ImportError:
    MATPLOTLIB_AVAILABLE = False

class FinalDiagnostic(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("Capacitive Test Bench")
        self.resize(1200, 750)

        # --- state vars ---
        self.ser = None
        self.recording = False
        self.csv_file = None
        self.csv_writer = None
        self.current_filename = ""
        self.start_time = 0
        
        # dflt buf/win params
        self.buffer_size = 300 
        self.data_f = collections.deque([0]*self.buffer_size, maxlen=self.buffer_size)
        self.data_b = collections.deque([0]*self.buffer_size, maxlen=self.buffer_size)

        # --- ui init ---
        container = QWidget()
        layout = QVBoxLayout(container)
        self.setCentralWidget(container)

        self.setup_toolbar(layout)

        self.status = QLabel("DISCONNECTED")
        self.status.setAlignment(Qt.AlignCenter)
        self.status.setStyleSheet("font-size: 40px; font-weight: bold; background: #7f8c8d; color: white; border-radius: 10px;")
        layout.addWidget(self.status)

        # --- graph cfg ---
        self.pw = pg.PlotWidget()
        self.pw.setBackground('k')
        self.pw.showGrid(x=True, y=True)
        self.pw.addLegend()
        
        # init axis view
        self.update_view_settings()
        
        layout.addWidget(self.pw)
        
        self.curve_f = self.pw.plot(pen=pg.mkPen(color='#00d1ff', width=2), name="Filtered")
        self.curve_b = self.pw.plot(pen=pg.mkPen(color='#ff4757', width=1, style=Qt.DashLine), name="Baseline")

        self.timer = QTimer()
        self.timer.timeout.connect(self.read_serial)

    def setup_toolbar(self, parent_layout):
        nav_layout = QHBoxLayout()
        
        # --- conn ctrl ---
        self.combo_ports = QComboBox()
        self.refresh_ports()
        self.btn_refresh = QPushButton("🔄")
        self.btn_refresh.setFixedWidth(30)
        self.btn_refresh.clicked.connect(self.refresh_ports)
        
        self.btn_connect = QPushButton("Connect")
        self.btn_connect.clicked.connect(self.toggle_connection)
        self.btn_connect.setStyleSheet("background-color: #3498db; color: white; font-weight: bold; padding: 5px;")

        # --- graph param ---
        self.txt_x_win = QLineEdit("300")
        self.txt_x_win.setFixedWidth(45)
        self.txt_y_min = QLineEdit("0")
        self.txt_y_min.setFixedWidth(45)
        self.txt_y_max = QLineEdit("255")
        self.txt_y_max.setFixedWidth(45)
        
        self.btn_apply = QPushButton("Update Graph")
        self.btn_apply.clicked.connect(self.update_view_settings)
        self.btn_apply.setStyleSheet("background-color: #f1c40f; color: black; font-weight: bold;")

        # new clr btn
        self.btn_clear = QPushButton("Clear Graph")
        self.btn_clear.clicked.connect(self.clear_graph)
        self.btn_clear.setStyleSheet("background-color: #34495e; color: white; font-weight: bold;")

        # --- rec ctrl ---
        self.btn_record = QPushButton("⏺ Record CSV")
        self.btn_record.clicked.connect(self.toggle_record)
        self.btn_record.setEnabled(False)
        self.btn_record.setStyleSheet("background-color: #95a5a6; color: white; font-weight: bold; padding: 5px;")

        # add to layout
        nav_layout.addWidget(QLabel("Port:"))
        nav_layout.addWidget(self.combo_ports)
        nav_layout.addWidget(self.btn_refresh)
        nav_layout.addWidget(self.btn_connect)
        
        nav_layout.addSpacing(30)
        nav_layout.addWidget(QLabel("X-Win:"))
        nav_layout.addWidget(self.txt_x_win)
        nav_layout.addWidget(QLabel("Y-Min:"))
        nav_layout.addWidget(self.txt_y_min)
        nav_layout.addWidget(QLabel("Y-Max:"))
        nav_layout.addWidget(self.txt_y_max)
        nav_layout.addWidget(self.btn_apply)
        nav_layout.addWidget(self.btn_clear) # added
        
        nav_layout.addStretch()
        nav_layout.addWidget(self.btn_record)
        
        parent_layout.addLayout(nav_layout)

    def clear_graph(self):
        """rst buf to 0 to clr dsplay"""
        self.data_f = collections.deque([0]*self.buffer_size, maxlen=self.buffer_size)
        self.data_b = collections.deque([0]*self.buffer_size, maxlen=self.buffer_size)
        self.curve_f.setData(list(self.data_f))
        self.curve_b.setData(list(self.data_b))
        self.statusBar().showMessage("Graph Cleared", 2000)

    def update_view_settings(self):
        try:
            ymin = int(self.txt_y_min.text())
            ymax = int(self.txt_y_max.text())
            self.pw.setYRange(ymin, ymax, padding=0)
            self.pw.enableAutoRange(axis='y', enable=False)

            new_x = int(self.txt_x_win.text())
            if new_x > 0 and new_x != self.buffer_size:
                self.buffer_size = new_x
                self.data_f = collections.deque(list(self.data_f), maxlen=self.buffer_size)
                self.data_b = collections.deque(list(self.data_b), maxlen=self.buffer_size)
            
            self.statusBar().showMessage(f"Applied: Y[{ymin}-{ymax}] X[{new_x}]", 2000)
        except ValueError:
            QMessageBox.warning(self, "Error", "Check your numeric values.")

    def refresh_ports(self):
        self.combo_ports.clear()
        self.combo_ports.addItems([p.device for p in serial.tools.list_ports.comports()])

    def toggle_connection(self):
        if self.ser is None or not self.ser.is_open:
            port = self.combo_ports.currentText()
            if not port: return
            try:
                self.ser = serial.Serial(port, 115200, timeout=0.01)
                self.btn_connect.setText("Disconnect")
                self.btn_connect.setStyleSheet("background-color: #e67e22; color: white;")
                self.btn_record.setEnabled(True)
                self.timer.start(16)
            except Exception as e:
                QMessageBox.critical(self, "Error", f"Fail: {e}")
        else:
            self.stop_all()

    def stop_all(self):
        if self.recording: self.toggle_record()
        self.timer.stop()
        if self.ser: self.ser.close()
        self.ser = None
        self.btn_connect.setText("Connect")
        self.btn_connect.setStyleSheet("background-color: #3498db; color: white;")
        self.btn_record.setEnabled(False)
        self.status.setText("DISCONNECTED")
        self.status.setStyleSheet("background: #7f8c8d; color: white; font-size: 40px;")

    def toggle_record(self):
        if not self.recording:
            folder = "recordings"
            if not os.path.exists(folder): os.makedirs(folder)
            self.current_filename = os.path.join(folder, f"rec_{datetime.now().strftime('%Y%m%d_%H%M%S')}.csv")
            try:
                self.csv_file = open(self.current_filename, 'w', newline='')
                self.csv_writer = csv.writer(self.csv_file)
                self.csv_writer.writerow(['Time_s', 'Filtered', 'Baseline', 'Touch'])
                self.start_time = time.time()
                self.recording = True
                self.btn_record.setText("⏹ Stop Record")
                self.btn_record.setStyleSheet("background-color: #c0392b; color: white;")
            except Exception as e:
                QMessageBox.critical(self, "Error", str(e))
        else:
            self.recording = False
            self.csv_file.close()
            self.btn_record.setText("⏺ Record CSV")
            self.btn_record.setStyleSheet("background-color: #27ae60; color: white;")
            if MATPLOTLIB_AVAILABLE: self.plot_recorded_data(self.current_filename)

    def plot_recorded_data(self, filename):
        t, f, b, s = [], [], [], []
        try:
            with open(filename, 'r') as csvfile:
                reader = csv.reader(csvfile); next(reader)
                for row in reader:
                    t.append(float(row[0])); f.append(int(row[1])); b.append(int(row[2])); s.append(int(row[3]))
            plt.figure("Analysis", figsize=(10, 6))
            plt.plot(t, f, color='#00d1ff', label='Filtered')
            plt.plot(t, b, color='#ff4757', linestyle='--', label='Baseline', alpha=0.5)
            is_grip = False
            start = 0
            for i in range(len(s)):
                if s[i] == 1 and not is_grip:
                    is_grip, start = True, t[i]
                elif s[i] == 0 and is_grip:
                    is_grip = False
                    plt.axvspan(start, t[i], color='red', alpha=0.2)
            plt.title(f"Log: {os.path.basename(filename)}")
            plt.xlabel("Seconds"); plt.ylabel("Value"); plt.legend(); plt.grid(True, alpha=0.3)
            plt.show()
        except Exception as e: print(f"Plot Error: {e}")

    def read_serial(self):
        try:
            while self.ser and self.ser.in_waiting > 0:
                line = self.ser.readline().decode('utf-8', errors='ignore').strip()
                if not line: continue
                vals = line.split(',')
                if len(vals) >= 3:
                    f_val, b_val, t_val = int(vals[0]), int(vals[1]), int(vals[2])
                    self.data_f.append(f_val); self.data_b.append(b_val)
                    self.curve_f.setData(list(self.data_f))
                    self.curve_b.setData(list(self.data_b))
                    if self.recording:
                        self.csv_writer.writerow([round(time.time() - self.start_time, 3), f_val, b_val, t_val])
                    if t_val == 1:
                        self.status.setText("GRIP")
                        self.status.setStyleSheet("font-size: 50px; font-weight: bold; background: #e74c3c; color: white; border-radius: 15px;")
                    else:
                        self.status.setText("RELEASE")
                        self.status.setStyleSheet("font-size: 50px; font-weight: bold; background: #2ecc71; color: white; border-radius: 15px;")
        except: self.stop_all()

if __name__ == "__main__":
    app = QApplication(sys.argv)
    demo = FinalDiagnostic()
    demo.show()
    sys.exit(app.exec_())