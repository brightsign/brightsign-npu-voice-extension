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

void signalHandler(int signum) {
    std::cout << "Interrupt signal (" << signum << ") received.\n";

    // Cleanup and shutdown
    running = false;
    jsonResultQueue.signalShutdown();
    bsvarResultQueue.signalShutdown();
}

int main(int argc, char **argv) {
    char *model_name = NULL;
    freopen("/storage/sd/console.log", "a", stdout);
    if (argc != 4) {
        printf("Usage: %s <rknn model> <source> \n", argv[0]);
        return -1;
    }

    // The path where the model is located
    model_name = (char *)argv[1];
    char *source_name = argv[2];
    //USB Mic device
    std::string audio_device = "plug" + std::string(argv[3]);

    MLInferenceThread mlThread(
	model_name,
	source_name,
	jsonResultQueue,
	bsvarResultQueue,
	gaze_mutex,
	gaze_cv,
	trigger_asr,
	asr_busy,
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
    ASRThread asrThread(jsonResultQueue,
	bsvarResultQueue,
	running,
	asr_trigger,
	gaze_mutex,
	gaze_cv,
	trigger_asr,
	asr_busy,
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
    //asr_publisherThread.join();
    json_publisherThread.join();
    bsvar_publisherThread.join();

    return 0;
}
