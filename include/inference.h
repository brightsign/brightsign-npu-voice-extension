#ifndef INFERENCE_H
#define INFERENCE_H

#include <atomic>
#include <chrono>
#include <string>

#include "opencv2/core/core.hpp"
#include "opencv2/imgproc/imgproc.hpp"
#include <opencv2/highgui.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/videoio.hpp>

#include "queue.h"
#include "retinaface.h"

// Struct to hold ML inference results
struct InferenceResult {
    // float confidence;
    // std::string label;
    int count_all_faces_in_frame;
    int num_faces_attending;
    std::string asr;
    std::chrono::system_clock::time_point timestamp;
};

class MLInferenceThread {
private:
    ThreadSafeQueue<InferenceResult>& jsonResultQueue;
    ThreadSafeQueue<InferenceResult>& bsvarResultQueue;
    std::atomic<bool>& running;
    int target_fps;
    rknn_app_context_t rknn_app_ctx;
    cv::VideoCapture capture;
    int frames{0};
    std::mutex& gaze_mutex;
    std::condition_variable& gaze_cv;
    bool& trigger_asr;
    std::atomic<bool>& asr_busy;
    std::atomic<int>& current_faces_attending;
    std::atomic<int>& current_total_faces;
    // Simulated ML model inference
    InferenceResult runInference(cv::Mat& img);

public:
    MLInferenceThread(
        const std::string& model_path,
        const std::string& source_name,
        ThreadSafeQueue<InferenceResult>& jsonQueue,
        ThreadSafeQueue<InferenceResult>& bsvarQueue,
        std::mutex& gaze_mutex,
        std::condition_variable& gaze_cv,
        bool& trigger_asr,
        std::atomic<bool>& asr_busy,
        std::atomic<int>& current_faces_attending,
        std::atomic<int>& current_total_faces,
        std::atomic<bool>& isRunning,
        int target_fps);
    ~MLInferenceThread(); // Destructor declaration
    void operator()();
};

#endif // INFERENCE_H
