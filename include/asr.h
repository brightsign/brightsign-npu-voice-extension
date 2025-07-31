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

/**
 * @class ASRThread
 * @brief ASR thread class for for audio capture and speech-to-text conversion.
 * 
 * This class manages the ASR (Automatic Speech Recognition) functionality using Whisper model.
 * It runs in a separate thread and processes audio when triggered by face detection.
 */
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
    std::atomic<int>& current_faces_attending;
    std::atomic<int>& current_total_faces;
    std::vector<float> mel_filters;
    rknn_whisper_context_t rknn_app_ctx;
    VocabEntry vocab[VOCAB_NUM];
    /**
     * @brief Runs the automatic speech recognition process
     * @return InferenceResult containing the recognized text and face count information
     * 
     * This private method handles the complete ASR pipeline.
     * - Records audio using Voice Activity Detection (VAD)
     * - Preprocesses the audio (channel conversion, resampling)
     * - Runs Whisper model inference
     * - Returns the recognized text along with timing information
     */
    InferenceResult runASR();

public:
    /**
     * @brief Constructor for ASRThread
     * 
     * Initializes the ASR thread with model paths and parameters.
     * - Loads Whisper encoder and decoder models
     * - Reads vocabulary file
     * - Initializes mel filters
     */
    ASRThread(
        const std::string& model_path,
        ThreadSafeQueue<InferenceResult>& jsonQueue,
        ThreadSafeQueue<InferenceResult>& bsvarQueue,
        std::atomic<bool>& isRunning,
        std::atomic<bool>& triggerFlag,
	    std::mutex& gaze_mutex,
        std::condition_variable& gaze_cv,
        bool& trigger_asr,
	    std::atomic<bool>& asr_busy,
        std::atomic<int>& current_faces_attending,
        std::atomic<int>& current_total_faces,
	    std::string &alsa_device,
        int sample_rate = 16000,
        int channels = 1,
        int record_seconds = 3);
    /**
     * @brief Destructor for ASRThread
     * 
     * Releases allocated resources.
     * - Whisper encoder and decoder models
     * - Signals shutdown to result queues
     * - Sets running flag to false
     */
    ~ASRThread();
    /**
     * @brief Main thread execution operator
     * 
     * Runs the ASR thread main loop.
     * - Waits for gaze detection trigger
     * - Sends "Listening..." status message
     * - Executes speech recognition
     * - Publishes results to output queues
     * - Updates face count information from inference thread
     * - Manages thread synchronization and cleanup
     */
    void operator()();
};

#endif // ASR_H
