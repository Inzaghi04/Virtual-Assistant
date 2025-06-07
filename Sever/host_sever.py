from flask import Flask, request, jsonify, send_from_directory
import os
import speech_recognition as sr
import google.generativeai as genai
from gtts import gTTS
from werkzeug.utils import secure_filename
from concurrent.futures import ThreadPoolExecutor
import time
import pandas as pd
from datetime import datetime

# === Cấu hình ===
API_KEY = "API_KEY"  # ← thay bằng API key của bạn
UPLOAD_FOLDER = "./uploads"
os.makedirs(UPLOAD_FOLDER, exist_ok=True)

# Cấu hình Gemini
genai.configure(api_key=API_KEY)
model = genai.GenerativeModel("gemini-2.0-flash")

app = Flask(__name__)
app.config['UPLOAD_FOLDER'] = UPLOAD_FOLDER

executor = ThreadPoolExecutor(max_workers=2)

# === Hàm nhận dạng giọng nói từ file âm thanh ===
def audio_to_text(filepath):
    recognizer = sr.Recognizer()
    with sr.AudioFile(filepath) as source:
        audio = recognizer.record(source)
    try:
        text = recognizer.recognize_google(audio, language="vi-VN")
        return text
    except Exception as e:
        print("❌ Lỗi khi nhận dạng giọng nói:", e)
        return None

# === Hàm gửi văn bản tới Gemini và tạo file mp3 phản hồi ===
def process_with_gemini_and_save_mp3(prompt, mp3_filename):
    prompt = "Bạn hãy đóng vai trợ lí ảo, Vui lòng trả lời ngắn gọn trong 1 đến 2 câu (lưu ý các câu để trên cùng 1 dòng): " + prompt
    response = model.generate_content(prompt)
    reply_text = response.text

    # Tạo file mp3
    tts = gTTS(reply_text, lang="vi")
    tts.save(mp3_filename)
    print(f"✅ Đã lưu file MP3: {mp3_filename}")
    return reply_text

# === Ghi log vào Excel bằng pandas ===
def save_to_excel_pandas(text, reply_text, response_time, filename="logs.xlsx"):
    new_data = {
        "Thời gian": [datetime.now().strftime("%Y-%m-%d %H:%M:%S")],
        "Văn bản nhận dạng": [text],
        "Văn bản phản hồi": [reply_text],
        "Thời gian phản hồi (giây)": [round(response_time, 2)]
    }

    new_df = pd.DataFrame(new_data)

    if os.path.exists(filename):
        existing_df = pd.read_excel(filename)
        combined_df = pd.concat([existing_df, new_df], ignore_index=True)
    else:
        combined_df = new_df

    combined_df.to_excel(filename, index=False)
    print(f"📊 Đã lưu log vào file Excel: {filename}")
# === Hàm tạo file MP3 với nội dung "Mời bạn nói lại" ===
def create_retry_mp3(mp3_filename):
    retry_text = "Mời bạn nói lại"
    tts = gTTS(retry_text, lang="vi")
    tts.save(mp3_filename)
    print(f"✅ Đã tạo file MP3 retry: {mp3_filename}")
    return retry_text
# === API upload file âm thanh và xử lý ===
@app.route("/upload-audio", methods=["POST"])
def upload_audio():
    start_time = time.time()  # Bắt đầu tính thời gian

    if 'file' not in request.files:
        return jsonify({"error": "Không có file audio trong request"}), 400

    file = request.files['file']
    if file.filename == '':
        return jsonify({"error": "Tên file trống"}), 400

    filename = secure_filename(file.filename)
    audio_path = os.path.join(app.config['UPLOAD_FOLDER'], filename)
    file.save(audio_path)
    print(f"📥 Nhận file: {audio_path}")

    # Nhận dạng văn bản
    future_text = executor.submit(audio_to_text, audio_path)
    text = future_text.result(timeout=15)
    
    mp3_filename = os.path.splitext(filename)[0] + "_response.mp3"
    mp3_path = os.path.join(app.config['UPLOAD_FOLDER'], mp3_filename)
    
    if not text:
        # Nếu không nhận diện được, tạo file MP3 "Mời bạn nói lại"
        reply_text = create_retry_mp3(mp3_path)
        mp3_url = request.host_url + f"uploads/{mp3_filename}"
        elapsed_time = time.time() - start_time
        print(f"🌐 URL nội bộ cho file MP3: {mp3_url}")
        print(f"📝 Văn bản phản hồi: {reply_text}")
        
        # Ghi log vào Excel
        save_to_excel_pandas("Không nhận diện được", reply_text, elapsed_time)
        
        return jsonify({
            "text": "Không nhận diện được",
            "reply_text": reply_text,
            "mp3_url": mp3_url
        }), 200

    print(f"📝 Văn bản nhận dạng: {text.lower()}")

    # Tạo file phản hồi
    future_reply = executor.submit(process_with_gemini_and_save_mp3, text, mp3_path)
    reply_text = future_reply.result(timeout=30)

    mp3_url = request.host_url + f"uploads/{mp3_filename}"
    print(f"🌐 URL nội bộ cho file MP3: {mp3_url}")
    print(f"📝 Văn bản phản hồi: {reply_text}")

    elapsed_time = time.time() - start_time
    print(f"⚡ Thời gian phản hồi hệ thống: {elapsed_time:.2f} giây")

    # Ghi log vào Excel
    save_to_excel_pandas(text.lower(), reply_text, elapsed_time)

    return jsonify({
        "text": text.lower(),
        "reply_text": reply_text,
        "mp3_url": mp3_url
    })
# === Trả file MP3 khi truy cập URL ===
@app.route('/uploads/<path:filename>')
def serve_uploaded_file(filename):
    return send_from_directory(app.config['UPLOAD_FOLDER'], filename)

# === Chạy server Flask ===
if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5000)
