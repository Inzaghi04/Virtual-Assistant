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
from collections import defaultdict
import requests

# === Cấu hình ===
API_KEY = "API"  # ← Thay bằng API key của bạn
SERPER_API_KEY = "API"
UPLOAD_FOLDER = "./uploads"
os.makedirs(UPLOAD_FOLDER, exist_ok=True)

genai.configure(api_key=API_KEY)
model = genai.GenerativeModel("gemini-2.0-flash")

app = Flask(__name__)
app.config['UPLOAD_FOLDER'] = UPLOAD_FOLDER

executor = ThreadPoolExecutor(max_workers=2)
user_histories = defaultdict(list)

# === Hàm kiểm tra có nên tìm kiếm trên Google hay không ===
def should_search_web(prompt):
    prompt = prompt.lower()
    keywords = [
        "thời tiết", "hôm nay mấy độ", "trời",
        "ngày bao nhiêu", "thứ mấy", "hôm nay là ngày",
        "bao nhiêu độ", "nhiệt độ", "dự báo",
        "ngày mai", "lịch", "giờ", "mấy giờ", "giờ mấy"
    ]
    return any(keyword in prompt for keyword in keywords)

# === Hàm tìm kiếm web ===
def search_web(query):
    url = "https://google.serper.dev/search"
    headers = {
        "X-API-KEY": SERPER_API_KEY,
        "Content-Type": "application/json"
    }
    data = {"q": query, "gl": "vn", "hl": "vi"}
    try:
        response = requests.post(url, headers=headers, json=data)
        response.raise_for_status()
        results = response.json()
        snippets = [item["snippet"] for item in results.get("organic", [])[:3]]
        return "\n".join(snippets) if snippets else "Không tìm thấy thông tin mới."
    except Exception as e:
        print("❌ Lỗi khi tìm kiếm:", e)
        return "Không thể truy cập thông tin mới."

# === Nhận dạng giọng nói từ audio ===
def audio_to_text(filepath):
    recognizer = sr.Recognizer()
    with sr.AudioFile(filepath) as source:
        audio = recognizer.record(source)
    try:
        text = recognizer.recognize_google(audio, language="vi-VN")
        return text
    except Exception as e:
        print("❌ Lỗi nhận dạng giọng nói:", e)
        return None

# === Xử lý với Gemini và tạo file MP3 ===
def process_with_gemini_and_save_mp3(prompt, mp3_filename, user_ip):
    history = user_histories[user_ip]
    system_prompt = "Bạn hãy trả lời ngắn gọn trong 1-2 câu. (Lưu ý các câu hãy để hết trên 1 dòng.)\n"


    # Chỉ tìm kiếm nếu cần
    web_context = ""
    if should_search_web(prompt):
        web_info = search_web(prompt)
        web_context = f"Thông tin tìm kiếm từ Google:\n{web_info}\n\n"

    # Ghép prompt
    prompt_parts = [system_prompt, web_context]
    for turn in history:
        prompt_parts.append(f"Người dùng: {turn['user']}")
        prompt_parts.append(f"Trợ lý: {turn['assistant']}")
    prompt_parts.append(f"Người dùng: {prompt}")
    prompt_parts.append("Trợ lý:")

    full_prompt = "\n".join(prompt_parts)

    response = model.generate_content(full_prompt)
    reply_text = response.text.strip()
    history.append({"user": prompt, "assistant": reply_text})

    tts = gTTS(reply_text, lang="vi")
    tts.save(mp3_filename)
    print(f"✅ Đã lưu file MP3: {mp3_filename}")
    return reply_text

# === Ghi log vào Excel ===
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
    print(f"📊 Đã lưu log vào: {filename}")

# === Tạo file MP3 khi không nhận diện được ===
def create_retry_mp3(mp3_filename):
    retry_text = "Mời bạn nói lại"
    tts = gTTS(retry_text, lang="vi")
    tts.save(mp3_filename)
    print(f"✅ Đã tạo file MP3 retry: {mp3_filename}")
    return retry_text

# === API nhận file audio và xử lý ===
@app.route("/upload-audio", methods=["POST"])
def upload_audio():
    start_time = time.time()

    if 'file' not in request.files:
        return jsonify({"error": "Không có file audio trong request"}), 400

    file = request.files['file']
    if file.filename == '':
        return jsonify({"error": "Tên file trống"}), 400

    filename = secure_filename(file.filename)
    audio_path = os.path.join(app.config['UPLOAD_FOLDER'], filename)
    file.save(audio_path)
    print(f"📥 Đã nhận file: {audio_path}")

    user_ip = request.remote_addr

    # Nhận dạng giọng nói
    future_text = executor.submit(audio_to_text, audio_path)
    text = future_text.result(timeout=15)

    mp3_filename = os.path.splitext(filename)[0] + "_response.mp3"
    mp3_path = os.path.join(app.config['UPLOAD_FOLDER'], mp3_filename)

    if not text:
        reply_text = create_retry_mp3(mp3_path)
        mp3_url = request.host_url + f"uploads/{mp3_filename}"
        elapsed_time = time.time() - start_time
        save_to_excel_pandas("Không nhận diện được", reply_text, elapsed_time)
        return jsonify({
            "text": "Không nhận diện được",
            "reply_text": reply_text,
            "mp3_url": mp3_url
        })

    print(f"📝 Văn bản nhận dạng: {text.lower()}")

    future_reply = executor.submit(process_with_gemini_and_save_mp3, text, mp3_path, user_ip)
    reply_text = future_reply.result(timeout=120)

    mp3_url = request.host_url + f"uploads/{mp3_filename}"
    print(f"🌐 URL nội bộ: {mp3_url}")
    print(f"📝 Phản hồi: {reply_text}")

    elapsed_time = time.time() - start_time
    print(f"⚡ Thời gian phản hồi: {elapsed_time:.2f} giây")

    save_to_excel_pandas(text.lower(), reply_text, elapsed_time)

    return jsonify({
        "text": text.lower(),
        "reply_text": reply_text,
        "mp3_url": mp3_url
    })

# === Trả file MP3 ===
@app.route('/uploads/<path:filename>')
def serve_uploaded_file(filename):
    return send_from_directory(app.config['UPLOAD_FOLDER'], filename)

if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5000)
