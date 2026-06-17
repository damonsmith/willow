#include "SpeechSegmenter.hpp"
#include <cmath>
#include <algorithm>
#include <regex>
#include <iostream>
#include <fstream>
#include <ctime>
#include <iomanip>

namespace VoiceAssistant {

SpeechSegmenter::SpeechSegmenter()
    : m_whisperCtx(nullptr)
    , m_vadThreshold(0.0003f)       // More sensitive threshold for normal speech
    , m_silenceDuration(0.8f)        // 800ms of silence ends segment
    , m_minSpeechDuration(0.25f)     // Minimum 250ms of speech
    , m_isSpeaking(false)
    , m_silenceFrames(0)
    , m_speechFrames(0)
    , m_stopTranscription(false)
{
}

SpeechSegmenter::~SpeechSegmenter() {
    shutdown();
}

bool SpeechSegmenter::initialize(const std::string& modelPath, const std::string& modelFile, bool useGPU) {
    m_modelPath = modelPath + "/" + modelFile;
    m_modelFile = modelFile;
    m_gpuEnabled = useGPU;
    
    // Initialize whisper context with GPU support
    whisper_context_params cparams = whisper_context_default_params();
    cparams.use_gpu = useGPU;
    cparams.gpu_device = 0;
    
    log("INFO", "Initializing Whisper from: " + m_modelPath + " (GPU: " + 
        std::string(useGPU ? "enabled" : "disabled") + ")");
    
    m_whisperCtx = whisper_init_from_file_with_params(m_modelPath.c_str(), cparams);
    
    if (!m_whisperCtx) {
        log("ERROR", "Failed to load Whisper model from: " + m_modelPath);
        return false;
    }
    
    // Setup whisper parameters for fast inference
    m_whisperParams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    m_whisperParams.print_progress = false;
    m_whisperParams.print_timestamps = false;
    m_whisperParams.print_special = false;
    m_whisperParams.language = "en";
    m_whisperParams.n_threads = 4;
    m_whisperParams.translate = false;
    m_whisperParams.no_context = true;  // Don't use context from previous segments
    m_whisperParams.single_segment = false;
    
    log("INFO", "Whisper initialized successfully");
    
    // Start the async transcription worker thread
    m_stopTranscription = false;
    m_transcriptionThread = std::thread(&SpeechSegmenter::transcriptionWorker, this);
    
    return true;
}

void SpeechSegmenter::shutdown() {
    // Stop the transcription worker thread
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_stopTranscription = true;
    }
    m_queueCV.notify_one();
    if (m_transcriptionThread.joinable()) {
        m_transcriptionThread.join();
    }
    
    if (m_whisperCtx) {
        whisper_free(m_whisperCtx);
        m_whisperCtx = nullptr;
        log("INFO", "Whisper context freed");
    }
}

void SpeechSegmenter::processAudioChunk(const std::vector<float>& chunk) {
    if (!m_whisperCtx) return;
    
    // Process in frames for VAD
    for (size_t i = 0; i + FRAME_SIZE <= chunk.size(); i += FRAME_SIZE) {
        std::vector<float> frame(chunk.begin() + i, chunk.begin() + i + FRAME_SIZE);
        
        bool voiceDetected = detectVoiceActivity(frame);
        
        if (voiceDetected) {
            // Voice detected - accumulate speech
            if (!m_isSpeaking) {
                log("INFO", "Speech started");
                m_isSpeaking = true;
                m_speechBuffer.clear();
            }
            
            m_speechBuffer.insert(m_speechBuffer.end(), frame.begin(), frame.end());
            m_silenceFrames = 0;
            m_speechFrames++;
            
        } else if (m_isSpeaking) {
            // In speech but current frame is silent
            m_speechBuffer.insert(m_speechBuffer.end(), frame.begin(), frame.end());
            m_silenceFrames++;
            
            // Check if we've had enough silence to end the segment
            int silenceThresholdFrames = static_cast<int>(m_silenceDuration * FRAMES_PER_SECOND);
            if (m_silenceFrames >= silenceThresholdFrames) {
                // End of speech segment
                float speechDuration = static_cast<float>(m_speechFrames) / FRAMES_PER_SECOND;
                
                log("INFO", "Speech ended (duration: " + std::to_string(speechDuration) + "s)");
                
                // Only transcribe if speech was long enough
                if (speechDuration >= m_minSpeechDuration) {
                    // Queue the segment for async transcription (non-blocking)
                    {
                        std::lock_guard<std::mutex> lock(m_queueMutex);
                        m_transcriptionQueue.push(std::move(m_speechBuffer));
                    }
                    m_queueCV.notify_one();
                } else {
                    log("INFO", "Speech too short, ignoring (duration: " + 
                        std::to_string(speechDuration) + "s)");
                }
                
                // Reset state
                m_isSpeaking = false;
                m_speechBuffer.clear();
                m_silenceFrames = 0;
                m_speechFrames = 0;
            }
        }
    }
}

void SpeechSegmenter::setTranscriptionCallback(TranscriptionCallback callback) {
    std::lock_guard<std::mutex> lock(m_callbackMutex);
    m_callback = callback;
}

bool SpeechSegmenter::detectVoiceActivity(const std::vector<float>& frame) {
    float energy = calculateEnergy(frame);
    return energy > m_vadThreshold;
}

float SpeechSegmenter::calculateEnergy(const std::vector<float>& frame) {
    if (frame.empty()) return 0.0f;
    
    float sum = 0.0f;
    for (float sample : frame) {
        sum += sample * sample;
    }
    
    return sum / frame.size();
}

std::string SpeechSegmenter::transcribe(const std::vector<float>& samples) {
    if (!m_whisperCtx || samples.empty()) {
        return "";
    }
    
    // Run whisper inference
    if (whisper_full(m_whisperCtx, m_whisperParams, samples.data(), samples.size()) != 0) {
        log("ERROR", "Whisper transcription failed");
        return "";
    }
    
    // Get transcription result
    const int n_segments = whisper_full_n_segments(m_whisperCtx);
    std::string result;
    
    for (int i = 0; i < n_segments; ++i) {
        const char* text = whisper_full_get_segment_text(m_whisperCtx, i);
        if (text) {
            result += text;
        }
    }
    
    // Clean the transcription
    result = cleanTranscription(result);
    
    return result;
}

std::string SpeechSegmenter::cleanTranscription(const std::string& text) {
    std::string result = text;
    
    // Remove content inside brackets [], braces {}, and parentheses ()
    // This handles [BLANK_AUDIO], [MUSIC], etc.
    std::regex bracketPattern(R"(\[[^\]]*\]|\{[^\}]*\}|\([^\)]*\))");
    result = std::regex_replace(result, bracketPattern, "");
    
    // Remove punctuation (periods, commas, exclamation marks, question marks, etc.)
    std::regex punctPattern(R"([.,!?;:])");
    result = std::regex_replace(result, punctPattern, "");
    
    // Collapse multiple spaces into single space
    std::regex multiSpacePattern(R"(\s+)");
    result = std::regex_replace(result, multiSpacePattern, " ");
    
    // Trim whitespace
    result.erase(0, result.find_first_not_of(" \t\n\r"));
    result.erase(result.find_last_not_of(" \t\n\r") + 1);
    
    // Convert to lowercase for processing
    std::transform(result.begin(), result.end(), result.begin(), ::tolower);
    
    return result;
}

void SpeechSegmenter::transcriptionWorker() {
    log("INFO", "Transcription worker thread started");
    
    while (true) {
        std::vector<float> samples;
        
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_queueCV.wait(lock, [this] {
                return m_stopTranscription || !m_transcriptionQueue.empty();
            });
            
            if (m_stopTranscription && m_transcriptionQueue.empty()) {
                break;
            }
            
            samples = std::move(m_transcriptionQueue.front());
            m_transcriptionQueue.pop();
        }
        
        // Transcribe on this dedicated thread (doesn't block audio capture)
        std::string transcription = transcribe(samples);
        
        if (!transcription.empty()) {
            log("INFO", "Transcription: " + transcription);
            
            std::lock_guard<std::mutex> lock(m_callbackMutex);
            if (m_callback) {
                m_callback(transcription);
            }
        }
    }
    
    log("INFO", "Transcription worker thread stopped");
}

void SpeechSegmenter::log(const std::string& level, const std::string& message) {
    std::lock_guard<std::mutex> lock(m_logMutex);
    
    auto now = std::time(nullptr);
    auto tm = *std::localtime(&now);
    
    std::ofstream logFile("/tmp/willow.log", std::ios::app);
    if (logFile.is_open()) {
        logFile << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") 
                << " [SpeechSegmenter] [" << level << "] " << message << std::endl;
    }
    
    std::cout << "[SpeechSegmenter] [" << level << "] " << message << std::endl;
}

} // namespace VoiceAssistant
