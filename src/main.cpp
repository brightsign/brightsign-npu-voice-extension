#include <chrono>
#include <memory>
#include <sys/time.h>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <stdio.h>
#include <string>
#include <thread>

#include <opencv2/core/core.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/videoio.hpp>

#include "attention.h"
#include "image_utils.h"
#include "inference.h"
#include "publisher.h"
#include "queue.h"
#include "retinaface.h"
#include "utils.h"
#include "asr.h"
#include <mutex>
#include <condition_variable>

std::atomic<bool> running{true};
ThreadSafeQueue<InferenceResult> jsonResultQueue(1);
ThreadSafeQueue<InferenceResult> bsvarResultQueue(1);

std::atomic<bool> asr_trigger{false};
std::mutex gaze_mutex;
std::condition_variable gaze_cv;
bool trigger_asr = false;
std::atomic<bool> asr_busy{false};
std::atomic<int> current_faces_attending{0};
std::atomic<int> current_total_faces{0};

void signalHandler(int signum) {
    std::cout << "Interrupt signal (" << signum << ") received.\n";

    // Cleanup and shutdown
    running = false;
    jsonResultQueue.signalShutdown();
    bsvarResultQueue.signalShutdown();
}

int main(int argc, char **argv) {
    if (argc != 8) {
        printf("Usage: %s <retinaface_model> <whisper_encoder> <whisper_decoder> <mel_filters> <vocabulary> <source> <audio_device>\n", argv[0]);
        return -1;
    }
    // The path where the model is located
    std::string retinaface_model = argv[1];
    std::string whisper_encoder_model = argv[2];
    std::string whisper_decoder_model = argv[3];
    std::string mel_filters_path = argv[4];
    std::string vocabulary_path = argv[5];
    std::string source_name = argv[6];
    //USB Mic device
    std::string audio_device = "plug" + std::string(argv[7]);
    
    std::cout << "Model files:" << std::endl;
    std::cout << "RetinaFace: " << retinaface_model << std::endl;
    std::cout << "Whisper Encoder: " << whisper_encoder_model << std::endl;
    std::cout << "Whisper Decoder: " << whisper_decoder_model << std::endl;
    std::cout << "Mel Filters: " << mel_filters_path << std::endl;
    std::cout << "Vocabulary: " << vocabulary_path << std::endl;
    std::cout << "Source: " << source_name << std::endl;
    std::cout << "Audio Device: " << audio_device << std::endl;

    MLInferenceThread mlThread(
	retinaface_model,
	source_name,
	jsonResultQueue,
	bsvarResultQueue,
	gaze_mutex,
	gaze_cv,
	trigger_asr,
	asr_busy,
    current_faces_attending,
    current_total_faces,
	running,
	30);

    auto json_formatter = std::make_shared<JsonMessageFormatter>();
    UDPPublisher json_publisher(
        "127.0.0.1",
        5002,
        jsonResultQueue,
        running,
        json_formatter,
        1000);
    auto bsvar_formatter = std::make_shared<BSVariableMessageFormatter>();
    UDPPublisher bsvar_publisher(
        "127.0.0.1",
        5000,
        bsvarResultQueue,
        running,
        bsvar_formatter,
        10);
    ASRThread asrThread(
        whisper_encoder_model,
        whisper_decoder_model,
        mel_filters_path,
        vocabulary_path,
        jsonResultQueue,
	    bsvarResultQueue,
	    running,
	    asr_trigger,
	    gaze_mutex,
	    gaze_cv,
	    trigger_asr,
	    asr_busy,
        current_faces_attending,
        current_total_faces,
	    audio_device);

    std::thread inferenceThread(std::ref(mlThread));
    std::thread asr_thread_handle(std::ref(asrThread));
    std::thread json_publisherThread(std::ref(json_publisher));
    std::thread bsvar_publisherThread(std::ref(bsvar_publisher));

    while (running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    // Cleanup and shutdown
    running = false;
    jsonResultQueue.signalShutdown();
    bsvarResultQueue.signalShutdown();

    inferenceThread.join();
    json_publisherThread.join();
    bsvar_publisherThread.join();

    return 0;
}
