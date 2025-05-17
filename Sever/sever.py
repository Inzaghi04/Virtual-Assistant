import os
import speech_recognition as sr
import google.generativeai as genai
from gtts import gTTS
import requests

# === Nhập API Key của Gemini tại đây ===
API_KEY = "AIzaSyDvDgIdNi8dsUslAK5yNE5C07nyt2OfBVU"  # ← thay bằng key thật

# === Cấu hình Gemini ===
genai.configure(api_key=API_KEY)
model = genai.GenerativeModel("gemini-2.0-flash")

# Hàm để ghi âm từ file
def ghi_am_vao_text_tu_file():
    recognizer = sr.Recognizer()

    # Yêu cầu nhập đường dẫn file âm thanh
    duong_dan = r"C:\sever\uploads\record.wav"  # Thay đổi đường dẫn tới file âm thanh của bạn

    # Kiểm tra xem đường dẫn có hợp lệ không
    if not os.path.isfile(duong_dan):
        print("❌ Không tìm thấy file âm thanh. Vui lòng kiểm tra lại đường dẫn.")
        return None

    # Xử lý file âm thanh
    with sr.AudioFile(duong_dan) as source:
        print("🔊 Đang xử lý file âm thanh...")
        audio = recognizer.record(source)

    try:
        # Nhận diện văn bản từ âm thanh
        text = recognizer.recognize_google(audio, language="vi-VN")
        print("🗣️ Nội dung từ file âm thanh:", text)
        return text
    except sr.UnknownValueError:
        print("⚠️ Không thể nhận diện được nội dung trong file.")
    except sr.RequestError:
        print("⚠️ Lỗi kết nối tới dịch vụ Google Speech.")

    return None

# Hàm hỏi Gemini và lưu phản hồi thành file MP3
def hoi_gemini_va_luu_mp3(prompt, mp3_filename="response.mp3"):
    print("🤖 Đang gửi tới Gemini...")
    prompt = "Vui lòng trả lời ngắn gọn trong 2-3 câu: " + prompt
    response = model.generate_content(prompt)
    bot_text = response.text
    print("💬 Gemini trả lời:", bot_text)

    # Chuyển văn bản thành file MP3
    tts = gTTS(bot_text, lang="vi")
    tts.save(mp3_filename)
    print(f"✅ Đã lưu file âm thanh: {mp3_filename}")

    # Upload lên tmpfiles.org
    with open(mp3_filename, "rb") as f:
        files = {'file': (mp3_filename, f)}
        response = requests.post("https://tmpfiles.org/api/v1/upload", files=files)

    if response.status_code == 200:
        json_data = response.json()
        raw_url = json_data.get("data", {}).get("url")
        if raw_url:
            # Chuyển URL xem sang URL tải về
            download_url = raw_url.replace("tmpfiles.org/", "tmpfiles.org/dl/")
            print("🌐 Link tải file MP3:", download_url)
            return download_url
        else:
            print("⚠️ Không nhận được URL từ tmpfiles.org.")
    else:
        print("❌ Upload thất bại:", response.status_code)

    return None

def main():
    noi_dung = ghi_am_vao_text_tu_file()
    if noi_dung:
        hoi_gemini_va_luu_mp3(noi_dung)

if __name__ == "__main__":
    main()
