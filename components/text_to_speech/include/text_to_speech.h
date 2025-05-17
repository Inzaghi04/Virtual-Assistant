#ifndef TEXT_TO_SPEECH_H
#define TEXT_TO_SPEECH_H

#ifdef __cplusplus
extern "C" {
#endif
void request_tts_from_rapidapi(const char *text);
const char* tts_get_audio_url(void);
#ifdef __cplusplus
}
#endif
#endif 