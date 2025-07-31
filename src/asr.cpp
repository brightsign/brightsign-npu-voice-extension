#include "asr.h"
#include <thread>
#include <iostream>
#include "audio_utils.h"
#include <sndfile.h>
#include <alsa/asoundlib.h>
#include <fvad.h>
#include <iomanip>

ASRThread::ASRThread(
    const std::string& model_path,
    ThreadSafeQueue<InferenceResult>& jsonQueue,
    ThreadSafeQueue<InferenceResult>& bsvarQueue,
    std::atomic<bool>& isRunning,
    std::atomic<bool>& triggerFlag,
    std::mutex& gaze_mutex_,
    std::condition_variable& gaze_cv_,
    bool& trigger_asr_,
    std::atomic<bool>& asr_busy_,
    std::atomic<int>& current_faces_attending_,
    std::atomic<int>& current_total_faces_,
    std::string& alsa_device_,
    const int sample_rate,
    const int channels,
    const int record_seconds)
    : jsonResultQueue(jsonQueue),
      bsvarResultQueue(bsvarQueue),
      running(isRunning),
      asr_trigger(triggerFlag),
      gaze_mutex(gaze_mutex_),
      gaze_cv(gaze_cv_),
      trigger_asr(trigger_asr_),
      asr_busy(asr_busy_),
      current_faces_attending(current_faces_attending_),
      current_total_faces(current_total_faces_),
      alsa_device(alsa_device_),
      sample_rate(sample_rate),
      channels(channels),
      record_seconds(record_seconds),
      vocab_path{model_path + "/vocab_en.txt"},
      task_code{TASK_CODE},
      mel_filters(N_MELS * MELS_FILTERS_SIZE),
      rknn_app_ctx{},
      vocab{}
{
    std::string encoder_model = model_path + "/whisper_encoder_base_20s.rknn";
    std::string decoder_model = model_path + "/whisper_decoder_base_20s.rknn";
    asr_trigger = true;
    //Init whisper encode and decoder models.
    int ret = init_whisper_model(encoder_model.c_str(), &rknn_app_ctx.encoder_context);
    if (ret != 0)
    {
        std::cout << "init_whisper_model fail! ret=" << ret << std::endl;
    }
    ret = init_whisper_model(decoder_model.c_str(), &rknn_app_ctx.decoder_context);
    if (ret != 0)
    {
        std::cout << "init_whisper_model fail! ret=" << ret << std::endl;
    }
    ret = read_mel_filters(MEL_FILTERS_PATH, mel_filters.data(), mel_filters.size());
    if (ret != 0)
    {
        std::cout << "read mel_filters fail! ret=" << ret << " mel_filters_path=" << MEL_FILTERS_PATH << std::endl;
    }

    ret = read_vocab(vocab_path.c_str(), vocab);
    if (ret != 0)
    {
        std::cout << "read vocab fail! ret=" << ret << " vocab_path=" << vocab_path << std::endl;
    }
}

ASRThread::~ASRThread() {
    int ret;
    ret = release_whisper_model(&rknn_app_ctx.encoder_context);
    if (ret != 0)
    {
        std::cout << "release_whisper_model encoder_context fail! ret=" << ret << std::endl;
    }
    ret = release_whisper_model(&rknn_app_ctx.decoder_context);
    if (ret != 0)
    {
        std::cout << "release_ppocr_model decoder_context fail! ret=" << ret << std::endl;
    }
    running = false;
    jsonResultQueue.signalShutdown();
    bsvarResultQueue.signalShutdown();
}

/**
 * @brief Records audio using Voice Activity Detection (VAD)
 * @param device ALSA audio device name for recording
 * @param wav_path Output path for the recorded WAV file
 * @return true if speech was detected and recorded, false otherwise
 * 
 * This function performs the following steps.
 * - Initializes VAD (Voice Activity Detection) with mode 1
 * - Opens ALSA audio device for capture
 * - Records audio frames and processes them through VAD
 * - Stops recording after detecting speech end or timeout
 * - Saves the recorded audio as a WAV file
 * - Uses silence detection to determine speech boundaries
 */
bool record_on_vad(const std::string& device, const std::string& wav_path) {
    constexpr int VAD_MODE = 3;
    constexpr int MAX_SILENCE_FRAMES = 100; //2 seconds of silence
    constexpr int MIN_SAMPLES = 16000; // 1 second minimum
    constexpr int MAX_TOTAL_FRAMES_MULTIPLIER = 2;
    constexpr int MIN_SPEECH_FRAMES = 25;

    std::unique_ptr<Fvad, decltype(&fvad_free)> vad{fvad_new(), fvad_free};
    if (!vad) {
        std::cout << "Failed to create VAD\n";
        return false;
    }
    fvad_set_mode(vad.get(), VAD_MODE); // 3 for more aggressive

    if (fvad_set_sample_rate(vad.get(), SAMPLE_RATE) < 0) {
        std::cout << "Invalid VAD sample rate\n";
        return false;
    }

    snd_pcm_t* pcm_handle = nullptr;
    snd_pcm_hw_params_t* hw_params = nullptr;
    const auto cleanup_alsa = [&pcm_handle]() {
        if (pcm_handle) {
            snd_pcm_close(pcm_handle);
            pcm_handle = nullptr;
        }
    };
    snd_pcm_format_t format = SND_PCM_FORMAT_S16_LE;

    if (const int err = snd_pcm_open(&pcm_handle, device.c_str(), SND_PCM_STREAM_CAPTURE, 0) < 0) {
        std::cout << "Cannot open device " << device << ": " << snd_strerror(err) << std::endl;
        return false;
    }

    snd_pcm_hw_params_alloca(&hw_params);
    snd_pcm_hw_params_any(pcm_handle, hw_params);
    snd_pcm_hw_params_set_access(pcm_handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(pcm_handle, hw_params, format);
    snd_pcm_hw_params_set_rate(pcm_handle, hw_params, SAMPLE_RATE, 0);
    snd_pcm_hw_params_set_channels(pcm_handle, hw_params, CHANNELS);

    if (const int err = snd_pcm_hw_params(pcm_handle, hw_params) < 0) {
        std::cout << "Cannot set HW params: " << snd_strerror(err) << std::endl;
        cleanup_alsa();
        return false;
    }

    std::vector<short> recorded_samples;
    recorded_samples.reserve(SAMPLE_RATE * MAX_SPEECH_SECONDS);
    int speech_frames = 0;
    int silence_frames = 0;
    bool in_speech = false;
    int max_speech_frames = (SAMPLE_RATE * MAX_SPEECH_SECONDS) / FRAME_LEN;
    int consecutive_speech_frames = 0;
    bool has_real_speech = false; 

    std::vector<short> frame(FRAME_LEN);
    constexpr int allowed_silence = MAX_SILENCE_FRAMES;
    constexpr int min_samples = MIN_SAMPLES;
    int total_frames = 0;
    while (speech_frames < max_speech_frames) {
        int r = snd_pcm_readi(pcm_handle, frame.data(), FRAME_LEN);
        if (r < 0) {
            std::cout << "ALSA read error: " << snd_strerror(r) << std::endl;
            break;
        }
        if (r != FRAME_LEN) {
            std::cout << "Short read from ALSA: " << r << "/" << FRAME_LEN << std::endl;
            continue;
        }

        int vad_result = fvad_process(vad.get(), frame.data(), FRAME_LEN);
        if (vad_result < 0) {
            std::cout << "VAD error!\n";
            break;
        }

        if (vad_result == 1) {
            consecutive_speech_frames++;
            if (!in_speech){
                std::cout << "Speech detected, starting recording.\n";
                in_speech = true;
                has_real_speech = true;
            }
            recorded_samples.insert(recorded_samples.end(), frame.begin(), frame.end());
            speech_frames++;
            silence_frames = 0;
            if (consecutive_speech_frames >= MIN_SPEECH_FRAMES) {
                has_real_speech = true;
                std::cout << "Substantial speech detected (" << consecutive_speech_frames * 20 << "ms continuous)\n";
            }
       } else {
            consecutive_speech_frames = 0;
            if (in_speech) {
                silence_frames++;
                if (silence_frames < allowed_silence) {
                    recorded_samples.insert(recorded_samples.end(), frame.begin(), frame.end());
                    speech_frames++;
                } else {
                    std::cout << "Silence after speech, stopping.\n";
                    break;
                }
            }
        }
        total_frames++;
        if (total_frames > max_speech_frames * MAX_TOTAL_FRAMES_MULTIPLIER){
            std::cout << "Maximum recording time reached\n";
            break;
        }
    }
    cleanup_alsa();
    // Enhanced quality checks
    if (recorded_samples.size() < static_cast<size_t>(min_samples)) {
        std::cout << "Discarding: too short (" << recorded_samples.size() / SAMPLE_RATE << "s, need " << min_samples / SAMPLE_RATE << "s minimum)\n";
        return false;
    }
    
    if (!has_real_speech) {
        std::cout << "Discarding: no substantial continuous speech detected (likely noise)\n";
        return false;
    }
   
    std::cout << "Recording complete: " << recorded_samples.size() / SAMPLE_RATE << " seconds, " 
              << speech_frames << " speech frames\n";

    SF_INFO sfinfo{
        .frames = 0,
        .samplerate = SAMPLE_RATE,
        .channels = CHANNELS,
        .format = SF_FORMAT_WAV | SF_FORMAT_PCM_16,
        .sections = 0,
        .seekable = 0
    };
    const auto sndfile_deleter = [](SNDFILE* file) { if (file) sf_close(file); };
    std::unique_ptr<SNDFILE, decltype(sndfile_deleter)> sndfile{
        sf_open(wav_path.c_str(), SFM_WRITE, &sfinfo), sndfile_deleter};
    if (!sndfile) {
        std::cout << "Failed to open output file\n";
        return false;
    }
    const sf_count_t written = sf_writef_short(sndfile.get(), recorded_samples.data(), 
                                               static_cast<sf_count_t>(recorded_samples.size() / CHANNELS));

    if (written != static_cast<sf_count_t>(recorded_samples.size() / CHANNELS)) {
        std::cout << "Warning: only wrote " << written << " of " << recorded_samples.size() / CHANNELS << " frames!\n";
    }
    return written > 0;
}

InferenceResult ASRThread::runASR() {
    InferenceResult result;
    int ret;
    TIMER timer;
    audio_buffer_t audio;
    memset(&audio, 0, sizeof(audio));
    struct AudioGuard {
        audio_buffer_t& audio_ref;
        explicit AudioGuard(audio_buffer_t& audio) : audio_ref(audio) {}
        ~AudioGuard() {
            if (audio_ref.data) {
                free(audio_ref.data);
                audio_ref.data = nullptr;
            }
        }
        AudioGuard(const AudioGuard&) = delete;
        AudioGuard& operator=(const AudioGuard&) = delete;
    } audio_guard(audio);

    std::vector<float> audio_data(N_MELS * MAX_AUDIO_LENGTH / HOP_LENGTH, 0.0f);
    std::vector<std::string> recognized_text;
    float infer_time = 0.0;
    float audio_length = 0.0;
    float rtf = 0.0;
    bool vad_detected = record_on_vad(alsa_device, "/tmp/capture.wav");
    if(!vad_detected)
    {
        result.asr ="";
        return result;
    }

    ret = read_audio("/tmp/capture.wav", &audio);
    if (ret != 0)
    {
        std::cout << "read audio fail! ret=" << ret << std::endl;
        result.asr ="";
        return result;
    }
    
    if (audio.num_channels == 2)
    {
        ret = convert_channels(&audio);
        if (ret != 0)
        {
            std::cout << "convert channels fail! ret=" << ret << std::endl;
            result.asr ="";
            return result;
        }
    }
    if (audio.sample_rate != SAMPLE_RATE)
    {
        ret = resample_audio(&audio, audio.sample_rate, SAMPLE_RATE);
        if (ret != 0)
        {
            std::cout << "resample audio fail! ret=" << ret << std::endl;
            result.asr ="";
            return result;
        }
    }
    timer.tik();
    audio_preprocess(&audio, mel_filters.data(), audio_data);
    ret = inference_whisper_model(&rknn_app_ctx, audio_data, mel_filters.data(), vocab, task_code, recognized_text);
    if (ret != 0)
    {
        std::cout << "inference_whisper_model fail! ret=" << ret << std::endl;
        result.asr ="";
        return result;
    }
    timer.tok();
    // print result
    std::cout << "\nWhisper output: ";
    for (const auto &str : recognized_text)
    {
        std::cout << str;
	    result.asr += str;
    }
    std::cout << std::endl;

    infer_time = timer.get_time() / 1000.0;               // sec
    audio_length = audio.num_frames / (float)SAMPLE_RATE; // sec
    audio_length = audio_length > (float)CHUNK_LENGTH ? (float)CHUNK_LENGTH : audio_length;
    rtf = infer_time / audio_length;
    std::cout << "\nReal Time Factor (RTF): " << std::fixed << std::setprecision(3) 
              << infer_time << " / " << audio_length << " = " << rtf << std::endl;
    return result;
}

void ASRThread::operator()() {
    while (running.load()) {
	    std::unique_lock<std::mutex> lock(gaze_mutex);
        gaze_cv.wait(lock, [&]{ return trigger_asr || !running.load(); });
        // Clean exit on shutdown
        if (!running.load()) {
            break;  
        }
        //Gaze detected send "Listening..." prompt
        // Get current values at the time of processing
        const int faces_attending = current_faces_attending.load();
        const int total_faces = current_total_faces.load();

        InferenceResult listening;
        listening.num_faces_attending = faces_attending;
        listening.count_all_faces_in_frame = total_faces;
        listening.timestamp = std::chrono::system_clock::now();
        listening.asr = "Listening...";
        jsonResultQueue.push(InferenceResult{listening});
        bsvarResultQueue.push(std::move(listening));

        InferenceResult result = runASR();
	    if(result.asr.empty())
        {
            std::cout<<"ASR is empty"<<std::endl;
        }
        else
        {
            // Fix update count_all_faces_in_frame from inference thread.
            result.num_faces_attending = faces_attending;
            result.count_all_faces_in_frame = total_faces;
            result.timestamp = std::chrono::system_clock::now();
            jsonResultQueue.push(InferenceResult{result});
            bsvarResultQueue.push(std::move(result));
        }
        trigger_asr = false;
	    asr_busy.store(false);
    }
}
