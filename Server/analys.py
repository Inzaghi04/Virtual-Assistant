import numpy as np
import matplotlib.pyplot as plt
import librosa
import os

def plot_waveform(data, sample_rate):
    duration = len(data) / sample_rate
    time = np.linspace(0, duration, num=len(data))
    plt.figure(figsize=(12, 4))
    plt.plot(time, data, color='blue')
    plt.title("Biểu đồ Dạng Sóng (Waveform)")
    plt.xlabel("Thời gian (giây)")
    plt.ylabel("Biên độ")
    plt.grid(True)
    plt.tight_layout()
    plt.show()

def plot_fft(data, sample_rate):
    fft_data = np.fft.fft(data)
    frequencies = np.fft.fftfreq(len(fft_data), 1 / sample_rate)
    half = len(frequencies) // 2
    frequencies = frequencies[:half]
    magnitudes = np.abs(fft_data[:half])
    plt.figure(figsize=(12, 4))
    plt.plot(frequencies, magnitudes, color='red')
    plt.title("Phổ Tần Số (FFT Spectrum)")
    plt.xlabel("Tần số (Hz)")
    plt.ylabel("Biên độ")
    plt.grid(True)
    plt.tight_layout()
    plt.show()

# --- Phần chính ---
file_path = r'C:\esp\Sever\uploads\record.wav'  # Có thể là file .wav hoặc .mp3

# Kiểm tra phần mở rộng để đảm bảo xử lý được
ext = os.path.splitext(file_path)[-1].lower()
if ext in ['.mp3', '.wav']:
    y, sr = librosa.load(file_path, sr=None)  # Tự động hỗ trợ mp3 và wav
    print(f"Đã đọc file: {file_path}")
    print(f"Tần số mẫu: {sr} Hz")
    print(f"Số mẫu: {len(y)}")
    plot_waveform(y, sr)
    plot_fft(y, sr)
else:
    print("Chỉ hỗ trợ định dạng MP3 hoặc WAV.")
