#pragma once

#include <whisper.h>
#include <vector>
#include <string>
#include <functional>
#include <mutex>
#include <atomic>
#include <thread>
#include <queue>
#include <condition_variable>

namespace VoiceAssistant {

/**
 * SpeechSegmenter - Accurate speech-to-text with VAD-based segmentation
 * 
 * This class addresses the duplicate/overlapping text issue by:
 * 1. Using Voice Activity Detection (VAD) to detect when speech starts/ends
 * 2. Only transcribing complete speech segments (no overlapping buffers)
 * 3. Properly clearing audio buffers after transcription
 */
class SpeechSegmenter {
public:
    using TranscriptionCallback = std::function<void(const std::string&)>;
    
    SpeechSegmenter();
    ~SpeechSegmenter();
    
    // Initialize with whisper model
    bool initialize(const std::string& modelPath, const std::string& modelFile, bool useGPU);
    void shutdown();
    
    // Process audio chunk and trigger callback when speech segment is complete
    void processAudioChunk(const std::vector<float>& chunk);
    
    // Set callback for transcription results
    void setTranscriptionCallback(TranscriptionCallback callback);
    
    // Configuration
    void setVADThreshold(float threshold) { m_vadThreshold = threshold; }
    void setSilenceDuration(float seconds) { m_silenceDuration = seconds; }
    void setMinSpeechDuration(float seconds) { m_minSpeechDuration = seconds; }
    
    // State
    bool isWhisperLoaded() const { return m_whisperCtx != nullptr; }
    bool isSpeaking() const { return m_isSpeaking; }
    bool isGPUEnabled() const { return m_gpuEnabled; }
    std::string getModelFile() const { return m_modelFile; }

private:
    // Whisper context
    whisper_context* m_whisperCtx;
    whisper_full_params m_whisperParams;
    std::string m_modelPath;
    std::string m_modelFile;
    bool m_gpuEnabled;
    
    // VAD parameters
    float m_vadThreshold;           // Energy threshold for voice detection
    float m_silenceDuration;        // Seconds of silence to end speech segment
    float m_minSpeechDuration;      // Minimum speech duration to transcribe
    
    // Speech state
    std::atomic<bool> m_isSpeaking;
    std::vector<float> m_speechBuffer;
    int m_silenceFrames;
    int m_speechFrames;
    
    // Constants
    static constexpr int SAMPLE_RATE = 16000;
    static constexpr int FRAMES_PER_SECOND = 50;  // 20ms frames
    static constexpr int FRAME_SIZE = SAMPLE_RATE / FRAMES_PER_SECOND;
    
    // Callback
    TranscriptionCallback m_callback;
    std::mutex m_callbackMutex;
    
    // Async transcription thread
    std::thread m_transcriptionThread;
    std::queue<std::vector<float>> m_transcriptionQueue;
    std::mutex m_queueMutex;
    std::condition_variable m_queueCV;
    std::atomic<bool> m_stopTranscription;
    void transcriptionWorker();
    
    // VAD helper
    bool detectVoiceActivity(const std::vector<float>& frame);
    float calculateEnergy(const std::vector<float>& frame);
    
    // Transcription
    std::string transcribe(const std::vector<float>& samples);
    std::string cleanTranscription(const std::string& text);
    
    // Logging
    void log(const std::string& level, const std::string& message);
    std::mutex m_logMutex;
};

} // namespace VoiceAssistant
