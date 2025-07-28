#ifndef ASR_H
#define ASR_H

#include <atomic>
#include <string>
#include <chrono>
#include "queue.h"
#include "whisper.h"
#include "process.h"
#include "inference.h"

#define SAMPLE_RATE 16000
#define CHANNELS 1
#define FRAME_MS 20
#define FRAME_LEN ((SAMPLE_RATE / 1000) * FRAME_MS) // samples per frame (320 for 20ms @ 16kHz)
#define MAX_SPEECH_SECONDS 5
#define TASK_CODE 50259

class ASRThread {
private:
    ThreadSafeQueue<InferenceResult>& jsonResultQueue;
    ThreadSafeQueue<InferenceResult>& bsvarResultQueue;
    std::atomic<bool>& running;
    std::atomic<bool>& asr_trigger;
    std::string asr_model_path;
    int sample_rate;
    int channels;
    int record_seconds;

    std::string encoder_model_path;
    std::string decoder_model_path;
    std::string vocab_path;
    std::string alsa_device;
    int task_code;
    std::string mel_filters_path;
    std::mutex& gaze_mutex;
    std::condition_variable& gaze_cv;
    bool& trigger_asr;
    std::atomic<bool>& asr_busy;
    std::vector<float> mel_filters;
    rknn_whisper_context_t rknn_app_ctx;
    VocabEntry vocab[VOCAB_NUM];
    InferenceResult runASR();

public:
    ASRThread(
        ThreadSafeQueue<InferenceResult>& jsonQueue,
        ThreadSafeQueue<InferenceResult>& bsvarQueue,
        std::atomic<bool>& isRunning,
        std::atomic<bool>& triggerFlag,
	std::mutex& gaze_mutex,
        std::condition_variable& gaze_cv,
        bool& trigger_asr,
	std::atomic<bool>& asr_busy,
	std::string &alsa_device,
        int sample_rate = 16000,
        int channels = 1,
        int record_seconds = 3);
    ~ASRThread(); // Destructor
    void operator()();
};

#endif // ASR_H
