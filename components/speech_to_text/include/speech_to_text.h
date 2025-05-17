#ifndef SPEECH_TO_TEXT_H
#define SPEECH_TO_TEXT_H

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

extern SemaphoreHandle_t xUploadDoneSemaphore;
extern SemaphoreHandle_t xJsonReady;
extern char uploaded_url[256];

#ifdef __cplusplus
extern "C" {
#endif

void chatbot_task(void);
const char *text(void);

#ifdef __cplusplus
}
#endif
#endif
