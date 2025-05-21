from flask import Flask, request, jsonify, send_from_directory
import os
import speech_recognition as sr
import google.generativeai as genai
from gtts import gTTS
from werkzeug.utils import secure_filename
from concurrent.futures import ThreadPoolExecutor

# === Cấu hình ===
API_KEY = "YOUR_API_KEY"  # ← thay bằng key thật của bạn
UPLOAD_FOLDER = "./uploads"
os.makedirs(UPLOAD_FOLDER, exist_ok=True)

# Cấu hình Gemini
genai.configure(api_key=API_KEY)
model = genai.GenerativeModel("gemini-2.0-flash")

app = Flask(__name__)
app.config['UPLOAD_FOLDER'] = UPLOAD_FOLDER

executor = ThreadPoolExecutor(max_workers=2)

# === Xử lý ghi âm => văn bản ===
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

# === Gửi text tới Gemini và tạo file MP3 trả lời ===
def process_with_gemini_and_save_mp3(prompt, mp3_filename):
    prompt = "Vui lòng trả lời ngắn gọn trong 2-3 câu: " + prompt
    response = model.generate_content(prompt)
    reply_text = response.text

    # Tạo file MP3 từ văn bản
    tts = gTTS(reply_text, lang="vi")
    tts.save(mp3_filename)
    print(f"✅ Đã lưu file MP3: {mp3_filename}")
    return reply_text

# === API upload audio và trả về text + URL mp3 nội bộ ===
@app.route("/upload-audio", methods=["POST"])
def upload_audio():
    if 'file' not in request.files:
        return jsonify({"error": "Không có file audio trong request"}), 400

    file = request.files['file']
    if file.filename == '':
        return jsonify({"error": "Tên file trống"}), 400

    filename = secure_filename(file.filename)
    audio_path = os.path.join(app.config['UPLOAD_FOLDER'], filename)
    file.save(audio_path)

    print(f"📥 Nhận file: {audio_path}")

    # Nhận dạng văn bản trong thread riêng
    future_text = executor.submit(audio_to_text, audio_path)
    text = future_text.result(timeout=15)
    if not text:
        return jsonify({"error": "Không thể nhận dạng giọng nói"}), 400
    print(f"📝 Văn bản nhận dạng: {text}")

    # Tên file mp3 cho kết quả trả lời, đặt khác với file upload audio
    mp3_filename = os.path.splitext(filename)[0] + "_response.mp3"
    mp3_path = os.path.join(app.config['UPLOAD_FOLDER'], mp3_filename)

    # Tạo file MP3 trong thread riêng
    future_reply = executor.submit(process_with_gemini_and_save_mp3, text, mp3_path)
    reply_text = future_reply.result(timeout=30)

    # Tạo URL nội bộ cho file MP3
    mp3_url = request.host_url + f"uploads/{mp3_filename}"
    print(f"🌐 URL nội bộ cho file MP3: {mp3_url}")

    return jsonify({
        "text": text,
        "reply_text": reply_text,
        "mp3_url": mp3_url
    })

# === Phục vụ file MP3 và các file trong thư mục uploads ===
@app.route('/uploads/<path:filename>')
def serve_uploaded_file(filename):
    return send_from_directory(app.config['UPLOAD_FOLDER'], filename)

if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5000)
