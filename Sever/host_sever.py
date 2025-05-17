from flask import Flask, request, jsonify
import os
import speech_recognition as sr
import google.generativeai as genai
from gtts import gTTS
import requests
from werkzeug.utils import secure_filename

# === Cấu hình ===
API_KEY = "AIzaSyDvDgIdNi8dsUslAK5yNE5C07nyt2OfBVU"  # ← thay bằng key của bạn
UPLOAD_FOLDER = "./uploads"
os.makedirs(UPLOAD_FOLDER, exist_ok=True)

# Cấu hình Gemini
genai.configure(api_key=API_KEY)
model = genai.GenerativeModel("gemini-2.0-flash")

app = Flask(__name__)
app.config['UPLOAD_FOLDER'] = UPLOAD_FOLDER

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

# === Gửi text tới Gemini và nhận file MP3 ===
def process_with_gemini_and_get_mp3_url(prompt, mp3_filename="response.mp3"):
    prompt = "Vui lòng trả lời ngắn gọn trong 2-3 câu: " + prompt
    response = model.generate_content(prompt)
    reply_text = response.text

    # Chuyển thành file MP3
    tts = gTTS(reply_text, lang="vi")
    tts.save(mp3_filename)

    # Upload lên tmpfiles.org
    with open(mp3_filename, "rb") as f:
        files = {'file': (mp3_filename, f)}
        response = requests.post("https://tmpfiles.org/api/v1/upload", files=files)

    if response.status_code == 200:
        data = response.json()
        raw_url = data.get("data", {}).get("url")
        if raw_url:
            return raw_url.replace("tmpfiles.org/", "tmpfiles.org/dl/")
    return None

# === API endpoint ===
@app.route("/upload-audio", methods=["POST"])
def upload_audio():
    if 'file' not in request.files:
        return jsonify({"error": "Không có file audio trong request"}), 400

    file = request.files['file']
    if file.filename == '':
        return jsonify({"error": "Tên file trống"}), 400

    filename = secure_filename(file.filename)
    filepath = os.path.join(app.config['UPLOAD_FOLDER'], filename)
    file.save(filepath)

    print(f"📥 Nhận file: {filepath}")

    # Chuyển thành văn bản
    text = audio_to_text(filepath)
    if not text:
        return jsonify({"error": "Không thể nhận dạng giọng nói"}), 400
    print(f"📝 Văn bản nhận dạng: {text}")

    # Gửi tới Gemini và nhận link âm thanh
    mp3_url = process_with_gemini_and_get_mp3_url(text)
    if not mp3_url:
        return jsonify({"error": "Không thể tạo file MP3 hoặc upload"}), 500

    return jsonify({
        "text": text,
        "mp3_url": mp3_url
    })

if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5000)
