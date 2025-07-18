#include "asr.h"
#include <thread>
#include <iostream>
#include "audio_utils.h"
#include <sndfile.h>
#include <alsa/asoundlib.h>
#include <fvad.h>
#include <iomanip>

ASRThread::ASRThread(
    ThreadSafeQueue<InferenceResult>& jsonQueue,
    ThreadSafeQueue<InferenceResult>& bsvarQueue,
    std::atomic<bool>& isRunning,
    std::atomic<bool>& triggerFlag,
    std::mutex& gaze_mutex_,
    std::condition_variable& gaze_cv_,
    bool& trigger_asr_,
    std::atomic<bool>& asr_busy_,
    int sample_rate,
    int channels,
    int record_seconds,
    std::string alsa_device_)
    : jsonResultQueue(jsonQueue),
      bsvarResultQueue(bsvarQueue),
      running(isRunning),
      asr_trigger(triggerFlag),
      gaze_mutex(gaze_mutex_),
      gaze_cv(gaze_cv_),
      trigger_asr(trigger_asr_),
      asr_busy(asr_busy_),
      sample_rate(sample_rate),
      channels(channels),
      record_seconds(record_seconds),
      alsa_device(alsa_device_)
{
    vocab_path = "model/vocab_en.txt";
    task_code = TASK_CODE;
    asr_trigger = true;
    mel_filters.resize(N_MELS * MELS_FILTERS_SIZE);
    int ret;

    memset(&rknn_app_ctx, 0, sizeof(rknn_whisper_context_t));
    memset(vocab, 0, sizeof(vocab));

    ret = init_whisper_model("model/whisper_encoder_base_20s.rknn", &rknn_app_ctx.encoder_context);
    if (ret != 0)
    {
        printf("init_whisper_model fail! ret=%d \n", ret);
    }
    ret = init_whisper_model("model/whisper_decoder_base_20s.rknn", &rknn_app_ctx.decoder_context);
    if (ret != 0)
    {
        printf("init_whisper_model fail! ret=%d \n", ret);
    }
    ret = read_mel_filters(MEL_FILTERS_PATH, mel_filters.data(), mel_filters.size());
    if (ret != 0)
    {
        printf("read mel_filters fail! ret=%d mel_filters_path=%s\n", ret, MEL_FILTERS_PATH);
    }

    ret = read_vocab(vocab_path.c_str(), vocab);
    if (ret != 0)
    {
        printf("read vocab fail! ret=%d vocab_path=%s\n", ret, vocab_path);
    }

}

ASRThread::~ASRThread() {
    int ret;
    ret = release_whisper_model(&rknn_app_ctx.encoder_context);
    if (ret != 0)
    {
        printf("release_whisper_model encoder_context fail! ret=%d\n", ret);
    }
    ret = release_whisper_model(&rknn_app_ctx.decoder_context);
    if (ret != 0)
    {
        printf("release_ppocr_model decoder_context fail! ret=%d\n", ret);
    }
    running = false;
    jsonResultQueue.signalShutdown();
    bsvarResultQueue.signalShutdown();
}

bool record_on_vad(const std::string& device, const std::string& wav_path) {
    Fvad* vad = fvad_new();
    if (!vad) {
        std::cerr << "Failed to create VAD\n";
        return false;
    }
    fvad_set_mode(vad, 1); // Or 3 for more aggressive

    if (fvad_set_sample_rate(vad, SAMPLE_RATE) < 0) {
        std::cerr << "Invalid VAD sample rate\n";
        fvad_free(vad);
        return false;
    }

    snd_pcm_t* pcm_handle = nullptr;
    snd_pcm_hw_params_t* hw_params = nullptr;
    int err;
    snd_pcm_format_t format = SND_PCM_FORMAT_S16_LE;

    if ((err = snd_pcm_open(&pcm_handle, device.c_str(), SND_PCM_STREAM_CAPTURE, 0)) < 0) {
        std::cerr << "Cannot open device " << device << ": " << snd_strerror(err) << std::endl;
        fvad_free(vad);
        return false;
    }

    snd_pcm_hw_params_alloca(&hw_params);
    snd_pcm_hw_params_any(pcm_handle, hw_params);
    snd_pcm_hw_params_set_access(pcm_handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(pcm_handle, hw_params, format);
    snd_pcm_hw_params_set_rate(pcm_handle, hw_params, SAMPLE_RATE, 0);
    snd_pcm_hw_params_set_channels(pcm_handle, hw_params, CHANNELS);

    if ((err = snd_pcm_hw_params(pcm_handle, hw_params)) < 0) {
        std::cerr << "Cannot set HW params: " << snd_strerror(err) << std::endl;
        snd_pcm_close(pcm_handle);
        fvad_free(vad);
        return false;
    }

    std::vector<short> recorded_samples;
    int speech_frames = 0;
    int silence_frames = 0;
    bool in_speech = false;
    int max_speech_frames = (SAMPLE_RATE * MAX_SPEECH_SECONDS) / FRAME_LEN;

    std::vector<short> frame(FRAME_LEN);
    int allowed_silence = 50;
    int min_samples = 32000; // 2 second minimum
    int total_frames = 0;
    while (speech_frames < max_speech_frames) {
        int r = snd_pcm_readi(pcm_handle, frame.data(), FRAME_LEN);
        if (r < 0) {
            std::cerr << "ALSA read error: " << snd_strerror(r) << std::endl;
            break;
        }
        if (r != FRAME_LEN) {
            std::cerr << "Short read from ALSA: " << r << "/" << FRAME_LEN << std::endl;
            continue;
        }

        int vad_result = fvad_process(vad, frame.data(), FRAME_LEN);
        if (vad_result < 0) {
            std::cerr << "VAD error!\n";
            break;
        }

        if (vad_result == 1) {
            if (!in_speech) std::cout << "Speech detected!\n";
            in_speech = true;
            recorded_samples.insert(recorded_samples.end(), frame.begin(), frame.end());
            speech_frames++;
            silence_frames = 0;

       } else if (in_speech) {
            silence_frames++;
            if (silence_frames < allowed_silence) {
                recorded_samples.insert(recorded_samples.end(), frame.begin(), frame.end());
                speech_frames++;
            } else {
                std::cout << "Silence after speech, stopping.\n";
                break;
            }
        }
        total_frames++;
        if (total_frames > max_speech_frames * 2) break;
    }

    if (recorded_samples.size() < min_samples) {
        std::cout << "Discarding: too short (likely noise, no speech)\n";
        fvad_free(vad);
        snd_pcm_close(pcm_handle);
        return false;
    }
    SF_INFO sfinfo = {};
    sfinfo.samplerate = SAMPLE_RATE;
    sfinfo.channels = CHANNELS;
    sfinfo.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;

    SNDFILE* sndfile = sf_open(wav_path.c_str(), SFM_WRITE, &sfinfo);
    if (!sndfile) {
        std::cerr << "Failed to open output file\n";
        fvad_free(vad);
        snd_pcm_close(pcm_handle);
        return false;
    }
    sf_count_t written = sf_writef_short(sndfile, recorded_samples.data(), recorded_samples.size()/CHANNELS);
    sf_close(sndfile);

    fvad_free(vad);
    snd_pcm_close(pcm_handle);

    if (written != static_cast<sf_count_t>(recorded_samples.size()/CHANNELS)) {
        std::cerr << "Warning: only wrote " << written << " of " << recorded_samples.size()/CHANNELS << " frames!\n";
    }
    return written > 0;
}

// Records PCM from ALSA and saves to a WAV file using libsndfile
bool alsa_record_wav(const std::string& alsa_device,
                     const std::string& wav_path,
                     int sample_rate,
                     int channels,
                     int seconds)
{
    snd_pcm_t* pcm_handle = nullptr;
    snd_pcm_hw_params_t* hw_params = nullptr;
    int err;

    snd_pcm_format_t format = SND_PCM_FORMAT_S16_LE; // 16-bit signed PCM
    int frames_to_read = sample_rate * seconds;
    std::vector<int16_t> buffer(frames_to_read * channels);

    // Open ALSA device
    if ((err = snd_pcm_open(&pcm_handle, alsa_device.c_str(), SND_PCM_STREAM_CAPTURE, 0)) < 0) {
        std::cerr << "Cannot open audio device: " << alsa_device << " (" << snd_strerror(err) << ")" << std::endl;
        return false;
    }

    snd_pcm_hw_params_alloca(&hw_params);
    snd_pcm_hw_params_any(pcm_handle, hw_params);
    snd_pcm_hw_params_set_access(pcm_handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(pcm_handle, hw_params, format);
    snd_pcm_hw_params_set_rate(pcm_handle, hw_params, sample_rate, 0);
    snd_pcm_hw_params_set_channels(pcm_handle, hw_params, channels);

    if ((err = snd_pcm_hw_params(pcm_handle, hw_params)) < 0) {
        std::cerr << "Cannot set HW params: " << snd_strerror(err) << std::endl;
        snd_pcm_close(pcm_handle);
        return false;
    }

    std::cout << "Recording for " << seconds << " seconds..." << std::endl;

    int frames_captured = 0;
    while (frames_captured < frames_to_read) {
        int rc = snd_pcm_readi(pcm_handle, buffer.data() + frames_captured * channels, frames_to_read - frames_captured);
        if (rc < 0) {
            if (rc == -EPIPE) {
                std::cerr << "Overrun occurred, recovering..." << std::endl;
                snd_pcm_prepare(pcm_handle);
                continue;
            } else {
                std::cerr << "Read error: " << snd_strerror(rc) << std::endl;
                snd_pcm_close(pcm_handle);
                return false;
            }
        }
        frames_captured += rc;
    }

    snd_pcm_drain(pcm_handle);
    snd_pcm_close(pcm_handle);

    // Save to WAV file with libsndfile
    SF_INFO sfinfo;
    std::memset(&sfinfo, 0, sizeof(sfinfo));
    sfinfo.samplerate = sample_rate;
    sfinfo.channels   = channels;
    sfinfo.format     = SF_FORMAT_WAV | SF_FORMAT_PCM_16;

    SNDFILE* sndfile = sf_open(wav_path.c_str(), SFM_WRITE, &sfinfo);
    if (!sndfile) {
        std::cerr << "Failed to open output file for writing: " << wav_path << std::endl;
        return false;
    }

    sf_count_t written = sf_writef_short(sndfile, buffer.data(), frames_to_read);
    sf_close(sndfile);

    if (written != frames_to_read) {
        std::cerr << "Wrote only " << written << " of " << frames_to_read << " frames!" << std::endl;
        return false;
    }

    return true;
}

InferenceResult ASRThread::runASR() {
    InferenceResult result;
    int ret;
    TIMER timer;
    audio_buffer_t audio;
    memset(&audio, 0, sizeof(audio));
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
        printf("read audio fail! ret=%d\n",ret);
    }
    
    if (audio.num_channels == 2)
    {
        ret = convert_channels(&audio);
        if (ret != 0)
        {
            printf("convert channels fail! ret=%d\n", ret);
        }
    }
    if (audio.sample_rate != SAMPLE_RATE)
    {
        ret = resample_audio(&audio, audio.sample_rate, SAMPLE_RATE);
        if (ret != 0)
        {
            printf("resample audio fail! ret=%d\n", ret);
        }
    }
    timer.tik();
    audio_preprocess(&audio, mel_filters.data(), audio_data);
    ret = inference_whisper_model(&rknn_app_ctx, audio_data, mel_filters.data(), vocab, task_code, recognized_text);
    if (ret != 0)
    {
        printf("inference_whisper_model fail! ret=%d\n", ret);
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
    printf("\nReal Time Factor (RTF): %.3f / %.3f = %.3f\n", infer_time, audio_length, rtf);

    return result;
}

void ASRThread::operator()() {
    while (running) {
	    std::unique_lock<std::mutex> lock(gaze_mutex);
            gaze_cv.wait(lock, [&]{ return trigger_asr; });
            InferenceResult result = runASR();
	    if(result.asr.empty())
            {
                std::cout<<"ASR is empty"<<std::endl;
            }
            else
            {
		// Fix update count_all_faces_in_frame from inference thread.
		result.num_faces_attending = 1;
		result.count_all_faces_in_frame = 1;
		result. timestamp = std::chrono::system_clock::now();
		InferenceResult bsresult(result);
                jsonResultQueue.push(std::move(result));
                bsvarResultQueue.push(std::move(bsresult));
            }
            trigger_asr = false;
	    asr_busy = false;
            lock.unlock();
        //std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}
