#include "asr.h"
#include <thread>
#include <iostream>
#include <algorithm>
#include "audio_utils.h"
#include <sndfile.h>
#include <alsa/asoundlib.h>
#include <fvad.h>
#include <iomanip>

ASRThread::ASRThread(
    const std::string& whisper_encoder_model,
    const std::string& whisper_decoder_model,
    const std::string& mel_filters_path,
    const std::string& vocabulary_path,
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
      task_code{TASK_CODE},
      mel_filters(N_MELS * MELS_FILTERS_SIZE),
      rknn_app_ctx{},
      vocab{}
{
    asr_trigger = true;
    std::cout << "ASRThread initialized with individual model files:" << std::endl;
    std::cout << "Whisper Encoder: " << whisper_encoder_model << std::endl;
    std::cout << "Whisper Decoder: " << whisper_decoder_model << std::endl;
    std::cout << "Mel Filters: " << mel_filters_path << std::endl;
    std::cout << "Vocabulary: " << vocabulary_path << std::endl;
    //Init whisper encode and decoder models.
    int ret = init_whisper_model(whisper_encoder_model.c_str(), &rknn_app_ctx.encoder_context);
    if (ret != 0)
    {
        std::cout << "init_whisper_model fail! ret=" << ret << std::endl;
    }
    ret = init_whisper_model(whisper_decoder_model.c_str(), &rknn_app_ctx.decoder_context);
    if (ret != 0)
    {
        std::cout << "init_whisper_model fail! ret=" << ret << std::endl;
    }
    ret = read_mel_filters(mel_filters_path.c_str(), mel_filters.data(), mel_filters.size());
    if (ret != 0)
    {
        std::cout << "read mel_filters fail! ret=" << ret << " mel_filters_path=" << mel_filters_path << std::endl;
    }

    ret = read_vocab(vocabulary_path.c_str(), vocab);
    if (ret != 0)
    {
        std::cout << "read vocab fail! ret=" << ret << " vocabulary_path=" << vocabulary_path << std::endl;
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
    constexpr int VAD_MODE = 2;  // Moderate aggressiveness
    constexpr int MAX_SILENCE_FRAMES = 80; // Reduced to 80 (1.6 seconds) for more responsive stopping
    constexpr int MIN_SAMPLES = 4000; // 0.25 second minimum
    constexpr int MAX_TOTAL_FRAMES_MULTIPLIER = 4; // Allow longer recordings
    constexpr int MIN_SPEECH_FRAMES = 6; // Reduced to 6 frames (120ms) for better single word detection

    std::unique_ptr<Fvad, decltype(&fvad_free)> vad{fvad_new(), fvad_free};
    if (!vad) {
        std::cout << "Failed to create VAD\n";
        return false;
    }
    fvad_set_mode(vad.get(), VAD_MODE); // 2 for moderate detection

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
    std::vector<short> pre_buffer; // Buffer to store frames before speech detection
    constexpr int PRE_BUFFER_FRAMES = 10; // Keep 10 frames (200ms) before speech
    int speech_frames = 0;
    int silence_frames = 0;
    bool in_speech = false;
    int max_speech_frames = (SAMPLE_RATE * MAX_SPEECH_SECONDS) / FRAME_LEN;
    int consecutive_speech_frames = 0;
    int max_consecutive_speech_frames = 0;  // Track maximum consecutive speech
    bool has_real_speech = false; 

    std::vector<short> frame(FRAME_LEN);
    constexpr int allowed_silence = MAX_SILENCE_FRAMES;
    constexpr int min_samples = MIN_SAMPLES;
    int total_frames = 0;
    float total_energy = 0.0f;  // Track audio energy for debugging
    int energy_frames = 0;
    
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

        // Calculate frame energy for debugging
        float frame_energy = 0.0f;
        for (int i = 0; i < FRAME_LEN; i++) {
            frame_energy += static_cast<float>(frame[i] * frame[i]);
        }
        frame_energy = std::sqrt(frame_energy / FRAME_LEN);
        total_energy += frame_energy;
        energy_frames++;

        if (vad_result == 1) {
            consecutive_speech_frames++;
            max_consecutive_speech_frames = std::max(max_consecutive_speech_frames, consecutive_speech_frames);
            
            if (!in_speech){
                std::cout << "Speech detected, starting recording.\n";
                in_speech = true;
                // Add pre-buffer content to recorded samples
                recorded_samples.insert(recorded_samples.end(), pre_buffer.begin(), pre_buffer.end());
                speech_frames += pre_buffer.size() / FRAME_LEN;
            }
            recorded_samples.insert(recorded_samples.end(), frame.begin(), frame.end());
            speech_frames++;
            silence_frames = 0;
            
            if (consecutive_speech_frames >= MIN_SPEECH_FRAMES) {
                if (!has_real_speech) {
                    has_real_speech = true;
                    std::cout << "Substantial speech detected (" << consecutive_speech_frames * 20 << "ms continuous)\n";
                }
            }
       } else {
            // Only reset consecutive speech frames if we're not in a brief silence gap
            if (silence_frames == 0) {
                consecutive_speech_frames = 0;  // Reset only when transitioning from speech to silence
            }
            
            if (in_speech) {
                silence_frames++;
                if (silence_frames < allowed_silence) {
                    recorded_samples.insert(recorded_samples.end(), frame.begin(), frame.end());
                    speech_frames++;
                } else {
                    std::cout << "NSR:Silence after speech, stopping.\n";
                    break;
                }
            } else {
                // Before speech detection, maintain a circular buffer
                pre_buffer.insert(pre_buffer.end(), frame.begin(), frame.end());
                if (pre_buffer.size() > PRE_BUFFER_FRAMES * FRAME_LEN) {
                    pre_buffer.erase(pre_buffer.begin(), pre_buffer.begin() + FRAME_LEN);
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
    
    // Audio quality diagnostics
    float avg_energy = energy_frames > 0 ? total_energy / energy_frames : 0.0f;
    std::cout << "Audio diagnostics: avg_energy=" << avg_energy 
              << ", total_frames=" << total_frames << ", vad_speech_frames=" << speech_frames << std::endl;
    
    if (avg_energy < 200.0f) {
        std::cout << "WARNING: Low audio energy detected. Check microphone volume/gain." << std::endl;
    } else if (avg_energy > 10000.0f) {
        std::cout << "WARNING: High audio energy detected. Audio may be clipping." << std::endl;
    }
    
    // Enhanced quality checks
    if (recorded_samples.size() < static_cast<size_t>(min_samples)) {
        std::cout << "Discarding: too short (" << recorded_samples.size() / SAMPLE_RATE << "s, need " << min_samples / SAMPLE_RATE << "s minimum)\n";
        return false;
    }
    
    if (!has_real_speech) {
        std::cout << "Discarding: no substantial continuous speech detected (likely noise)\n";
        return false;
    }
   
    std::cout << "Recording complete: " << static_cast<float>(recorded_samples.size()) / SAMPLE_RATE 
              << " seconds, " << speech_frames << " speech frames, " 
              << max_consecutive_speech_frames << " max consecutive speech frames\n";

    // Basic audio normalization to improve recognition
    if (!recorded_samples.empty()) {
        // Find peak amplitude
        short max_amplitude = 0;
        for (const auto& sample : recorded_samples) {
            max_amplitude = std::max(max_amplitude, static_cast<short>(std::abs(sample)));
        }
        
        // Normalize if peak is too low (but not if it's near clipping)
        if (max_amplitude > 0 && max_amplitude < 8000) {
            float gain = 10000.0f / max_amplitude;  // Increased target level
            if (gain > 1.0f && gain < 6.0f) { // Allow higher gain for very quiet audio
                std::cout << "Applying gain normalization: " << gain << "x\n";
                for (auto& sample : recorded_samples) {
                    sample = static_cast<short>(std::round(sample * gain));
                }
            }
        }
        
        // Simple noise gate to reduce background noise
        short noise_threshold = max_amplitude * 0.05f; // 5% of peak as noise threshold
        for (auto& sample : recorded_samples) {
            if (std::abs(sample) < noise_threshold) {
                sample = sample * 0.3f; // Reduce noise by 70%
            }
        }
    }

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
