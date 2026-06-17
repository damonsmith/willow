#include "VoiceAssistantService.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <ctime>
#include <iomanip>
#include <cstdlib>
#include <filesystem>
#include <chrono>

namespace fs = std::filesystem;

namespace VoiceAssistant {

VoiceAssistantService::VoiceAssistantService(sdbus::IConnection& connection, std::string objectPath)
    : m_connection(connection)
    , m_objectPath(std::move(objectPath))
    , m_currentWorker(nullptr)
    , m_isRunning(false)
    , m_currentMode(Mode::Normal)
    , m_hotword("hey")
    , m_commandThreshold(0.8)
    , m_processingInterval(1.5)
    , m_whisperModel("ggml-tiny.en.bin")
    , m_gpuAcceleration(false)
    , m_typingExitPhrases({"stop typing", "exit typing", "normal mode", "go to normal mode"})
    , m_stopAudioThread(false)
    , m_pulseAudio(nullptr)
{
    // Create D-Bus object
    m_object = sdbus::createObject(m_connection, sdbus::ObjectPath(m_objectPath));

    // Register D-Bus methods using vtable API (sdbus-c++ v2.x)
    const char* interfaceName = "com.github.saim.Willow";
    
    // Helper lambdas for method callbacks
    auto setModeCallback = [this](sdbus::MethodCall call) {
        std::string mode;
        call >> mode;
        this->SetMode(mode);
        auto reply = call.createReply();
        reply.send();
    };
    
    auto getModeCallback = [this](sdbus::MethodCall call) {
        auto reply = call.createReply();
        reply << this->GetMode();
        reply.send();
    };
    
    auto getStatusCallback = [this](sdbus::MethodCall call) {
        auto reply = call.createReply();
        reply << this->GetStatus();
        reply.send();
    };
    
    auto getConfigCallback = [this](sdbus::MethodCall call) {
        auto reply = call.createReply();
        reply << this->GetConfig();
        reply.send();
    };
    
    auto updateConfigCallback = [this](sdbus::MethodCall call) {
        std::string config;
        call >> config;
        this->UpdateConfig(config);
        auto reply = call.createReply();
        reply.send();
    };
    
    auto setConfigValueCallback = [this](sdbus::MethodCall call) {
        std::string key;
        sdbus::Variant value;
        call >> key >> value;
        this->SetConfigValue(key, value);
        auto reply = call.createReply();
        reply.send();
    };
    
    auto startCallback = [this](sdbus::MethodCall call) {
        this->Start();
        auto reply = call.createReply();
        reply.send();
    };
    
    auto stopCallback = [this](sdbus::MethodCall call) {
        this->Stop();
        auto reply = call.createReply();
        reply.send();
    };
    
    auto restartCallback = [this](sdbus::MethodCall call) {
        this->Restart();
        auto reply = call.createReply();
        reply.send();
    };
    
    auto getBufferCallback = [this](sdbus::MethodCall call) {
        auto reply = call.createReply();
        reply << this->GetBuffer();
        reply.send();
    };
    
    // Register methods using addVTable with MethodVTableItem
    m_object->addVTable(
        sdbus::MethodVTableItem{sdbus::MethodName{"SetMode"}, sdbus::Signature{"s"}, {"mode"}, sdbus::Signature{""}, {}, setModeCallback, {}},
        sdbus::MethodVTableItem{sdbus::MethodName{"GetMode"}, sdbus::Signature{""}, {}, sdbus::Signature{"s"}, {"mode"}, getModeCallback, {}},
        sdbus::MethodVTableItem{sdbus::MethodName{"GetStatus"}, sdbus::Signature{""}, {}, sdbus::Signature{"a{sv}"}, {"status"}, getStatusCallback, {}},
        sdbus::MethodVTableItem{sdbus::MethodName{"GetConfig"}, sdbus::Signature{""}, {}, sdbus::Signature{"s"}, {"config"}, getConfigCallback, {}},
        sdbus::MethodVTableItem{sdbus::MethodName{"UpdateConfig"}, sdbus::Signature{"s"}, {"config"}, sdbus::Signature{""}, {}, updateConfigCallback, {}},
        sdbus::MethodVTableItem{sdbus::MethodName{"SetConfigValue"}, sdbus::Signature{"sv"}, {"key", "value"}, sdbus::Signature{""}, {}, setConfigValueCallback, {}},
        sdbus::MethodVTableItem{sdbus::MethodName{"Start"}, sdbus::Signature{""}, {}, sdbus::Signature{""}, {}, startCallback, {}},
        sdbus::MethodVTableItem{sdbus::MethodName{"Stop"}, sdbus::Signature{""}, {}, sdbus::Signature{""}, {}, stopCallback, {}},
        sdbus::MethodVTableItem{sdbus::MethodName{"Restart"}, sdbus::Signature{""}, {}, sdbus::Signature{""}, {}, restartCallback, {}},
        sdbus::MethodVTableItem{sdbus::MethodName{"GetBuffer"}, sdbus::Signature{""}, {}, sdbus::Signature{"s"}, {"buffer"}, getBufferCallback, {}},
        
        sdbus::SignalVTableItem{sdbus::SignalName{"ModeChanged"}, sdbus::Signature{"s"}, {"mode"}, {}},
        sdbus::SignalVTableItem{sdbus::SignalName{"StatusChanged"}, sdbus::Signature{"a{sv}"}, {"status"}, {}},
        sdbus::SignalVTableItem{sdbus::SignalName{"BufferChanged"}, sdbus::Signature{"s"}, {"buffer"}, {}},
        sdbus::SignalVTableItem{sdbus::SignalName{"ConfigChanged"}, sdbus::Signature{"s"}, {"config"}, {}}
    ).forInterface(interfaceName);

    // Set config path
    const char* home = std::getenv("HOME");
    m_configPath = std::string(home) + "/.config/willow/config.json";
    m_logFile = "/tmp/willow.log";
    m_modelPath = std::string(home) + "/.local/share/willow/models";

    // Load configuration
    loadConfig();

    // Create shared components
    m_executor = std::make_shared<CommandExecutor>();
    m_segmenter = std::make_shared<SpeechSegmenter>();
    
    // Initialize whisper in segmenter
    if (!m_segmenter->initialize(m_modelPath, m_whisperModel, m_gpuAcceleration)) {
        log("ERROR", "Failed to initialize Whisper model");
        emitError("Initialization Error", "Failed to load Whisper model from: " + m_modelPath);
    }
    
    // Setup transcription callback
    m_segmenter->setTranscriptionCallback([this](const std::string& text) {
        handleTranscription(text);
    });
    
    // Create mode workers
    m_normalWorker = std::make_unique<NormalModeWorker>(m_executor, m_segmenter);
    m_commandWorker = std::make_unique<CommandModeWorker>(m_executor, m_segmenter);
    m_typingWorker = std::make_unique<TypingModeWorker>(m_executor, m_segmenter);
    
    // Setup mode change callbacks
    auto modeChangeCallback = [this](const std::string& newMode) {
        SetMode(newMode);
    };
    
    m_normalWorker->setModeChangeCallback(modeChangeCallback);
    m_commandWorker->setModeChangeCallback(modeChangeCallback);
    m_typingWorker->setModeChangeCallback(modeChangeCallback);
    
    // Configure workers
    m_normalWorker->setHotword(m_hotword);
    m_commandWorker->setCommands(m_commands);
    m_commandWorker->setThreshold(m_commandThreshold);
    m_typingWorker->setExitPhrases(m_typingExitPhrases);
    
    // Set initial worker
    m_currentWorker = m_normalWorker.get();

    // Auto-start if whisper loaded successfully
    if (m_segmenter->isWhisperLoaded()) {
        log("INFO", "Auto-starting voice processing");
        Start();
    }

    log("INFO", "Voice Assistant Service initialized (refactored architecture)");
}

VoiceAssistantService::~VoiceAssistantService() {
    Stop();
    m_segmenter->shutdown();
}

// D-Bus Method Implementations

void VoiceAssistantService::SetMode(const std::string& mode) {
    Mode newMode = stringToMode(mode);
    std::string oldModeStr = modeToString(m_currentMode);
    
    std::lock_guard<std::mutex> lock(m_modeMutex);
    
    // Stop current worker
    if (m_currentWorker) {
        m_currentWorker->stop();
    }
    
    // Update mode
    m_currentMode = newMode;
    
    // Set and start new worker
    updateModeWorkers();
    
    emitModeChanged(mode, oldModeStr);
    
    log("INFO", "Mode changed from " + oldModeStr + " to " + mode);
}

std::string VoiceAssistantService::GetMode() {
    return modeToString(m_currentMode);
}

std::map<std::string, sdbus::Variant> VoiceAssistantService::GetStatus() {
    std::map<std::string, sdbus::Variant> status;
    
    status["is_running"] = sdbus::Variant(m_isRunning.load());
    status["current_mode"] = sdbus::Variant(modeToString(m_currentMode));
    status["current_buffer"] = sdbus::Variant(GetBuffer());
    status["command_count"] = sdbus::Variant(static_cast<int32_t>(m_commands.size()));
    status["whisper_loaded"] = sdbus::Variant(m_segmenter->isWhisperLoaded());
    
    return status;
}

std::string VoiceAssistantService::GetConfig() {
    std::lock_guard<std::mutex> lock(m_configMutex);
    Json::StreamWriterBuilder writer;
    return Json::writeString(writer, configToJson());
}

void VoiceAssistantService::UpdateConfig(const std::string& configJson) {
    std::lock_guard<std::mutex> lock(m_configMutex);
    
    Json::CharReaderBuilder reader;
    Json::Value root;
    std::string errs;
    std::istringstream stream(configJson);
    
    if (Json::parseFromStream(reader, stream, &root, &errs)) {
        // Store old settings to detect changes
        bool oldGpuSetting = m_gpuAcceleration;
        std::string oldModel = m_whisperModel;
        
        jsonToConfig(root);
        saveConfig();
        
        // Reload whisper if GPU setting or model changed
        if (oldGpuSetting != m_gpuAcceleration || oldModel != m_whisperModel) {
            log("INFO", "GPU acceleration or model changed, reloading whisper...");
            m_segmenter->shutdown();
            if (!m_segmenter->initialize(m_modelPath, m_whisperModel, m_gpuAcceleration)) {
                log("ERROR", "Failed to reload Whisper model");
                emitError("Reload Error", "Failed to reload Whisper model with new settings");
            }
        }
        
        // Update workers with new config
        m_normalWorker->setHotword(m_hotword);
        m_commandWorker->setCommands(m_commands);
        m_commandWorker->setThreshold(m_commandThreshold);
        m_typingWorker->setExitPhrases(m_typingExitPhrases);
        
        // Emit config changed signal
        emitConfigChanged(configJson);
        log("INFO", "Configuration updated via D-Bus");
    } else {
        emitError("Configuration Error", "Failed to parse JSON: " + errs);
    }
}

void VoiceAssistantService::SetConfigValue(const std::string& key, const sdbus::Variant& value) {
    std::lock_guard<std::mutex> lock(m_configMutex);
    
    bool reloadWhisper = false;
    
    if (key == "hotword") {
        m_hotword = value.get<std::string>();
        m_normalWorker->setHotword(m_hotword);
    } else if (key == "command_threshold") {
        m_commandThreshold = value.get<double>();
        m_commandWorker->setThreshold(m_commandThreshold);
    } else if (key == "processing_interval") {
        m_processingInterval = value.get<double>();
    } else if (key == "whisper_model") {
        m_whisperModel = value.get<std::string>();
        log("INFO", "Whisper model changed to: " + m_whisperModel);
        reloadWhisper = true;
    } else if (key == "gpu_acceleration") {
        m_gpuAcceleration = value.get<bool>();
        log("INFO", "GPU acceleration changed to: " + std::string(m_gpuAcceleration ? "enabled" : "disabled"));
        reloadWhisper = true;
    }
    
    saveConfig();
    
    // Reload whisper if needed (release lock first to avoid deadlock)
    if (reloadWhisper) {
        m_configMutex.unlock();
        log("INFO", "Reloading whisper with new settings...");
        m_segmenter->shutdown();
        if (!m_segmenter->initialize(m_modelPath, m_whisperModel, m_gpuAcceleration)) {
            log("ERROR", "Failed to reload Whisper model");
            emitError("Reload Error", "Failed to reload Whisper model with new settings");
        }
        m_configMutex.lock();
    }
    
    log("INFO", "Config value updated: " + key);
}

std::string VoiceAssistantService::GetCommands() {
    std::lock_guard<std::mutex> lock(m_commandsMutex);
    
    Json::Value root(Json::arrayValue);
    for (const auto& cmd : m_commands) {
        Json::Value cmdJson;
        cmdJson["name"] = cmd.name;
        cmdJson["command"] = cmd.command;
        
        Json::Value phrases(Json::arrayValue);
        for (const auto& phrase : cmd.phrases) {
            phrases.append(phrase);
        }
        cmdJson["phrases"] = phrases;
        
        root.append(cmdJson);
    }
    
    Json::StreamWriterBuilder writer;
    return Json::writeString(writer, root);
}

void VoiceAssistantService::AddCommand(const std::string& name, const std::string& command,
                                       const std::vector<std::string>& phrases) {
    std::lock_guard<std::mutex> lock(m_commandsMutex);
    
    // Remove existing command with same name
    auto it = std::remove_if(m_commands.begin(), m_commands.end(),
        [&name](const Command& cmd) { return cmd.name == name; });
    m_commands.erase(it, m_commands.end());
    
    // Add new command
    Command newCmd;
    newCmd.name = name;
    newCmd.command = command;
    newCmd.phrases = phrases;
    m_commands.push_back(newCmd);
    
    saveConfig();
    log("INFO", "Command added: " + name);
}

void VoiceAssistantService::RemoveCommand(const std::string& name) {
    std::lock_guard<std::mutex> lock(m_commandsMutex);
    
    auto it = std::remove_if(m_commands.begin(), m_commands.end(),
        [&name](const Command& cmd) { return cmd.name == name; });
    
    if (it != m_commands.end()) {
        m_commands.erase(it, m_commands.end());
        saveConfig();
        log("INFO", "Command removed: " + name);
    }
}

void VoiceAssistantService::Start() {
    if (m_isRunning) {
        log("WARNING", "Service already running");
        return;
    }
    
    if (!m_segmenter->isWhisperLoaded()) {
        emitError("Start Error", "Whisper model not loaded");
        return;
    }
    
    m_isRunning = true;
    m_stopAudioThread = false;
    
    // Start current mode worker
    if (m_currentWorker) {
        m_currentWorker->start();
    }
    
    startAudioCapture();
    
    log("INFO", "Voice Assistant started");
    emitNotification("Voice Assistant", "Service started", "normal");
}

void VoiceAssistantService::Stop() {
    if (!m_isRunning) {
        return;
    }
    
    m_isRunning = false;
    m_stopAudioThread = true;
    
    // Stop current mode worker
    if (m_currentWorker) {
        m_currentWorker->stop();
    }
    
    stopAudioCapture();
    
    log("INFO", "Voice Assistant stopped");
    emitNotification("Voice Assistant", "Service stopped", "normal");
}

void VoiceAssistantService::Restart() {
    log("INFO", "Restarting Voice Assistant");
    Stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    Start();
}

std::string VoiceAssistantService::GetBuffer() {
    if (m_currentWorker) {
        return m_currentWorker->getBuffer();
    }
    return "";
}

std::string VoiceAssistantService::CurrentBuffer() const {
    if (m_currentWorker) {
        return m_currentWorker->getBuffer();
    }
    return "";
}

// Signal emission methods

void VoiceAssistantService::emitModeChanged(const std::string& newMode, const std::string& oldMode) {
    m_object->emitSignal("ModeChanged").onInterface("com.github.saim.Willow")
        .withArguments(newMode, oldMode);
}

void VoiceAssistantService::emitBufferChanged(const std::string& buffer) {
    m_object->emitSignal("BufferChanged").onInterface("com.github.saim.Willow")
        .withArguments(buffer);
}

void VoiceAssistantService::emitCommandExecuted(const std::string& command, 
                                                const std::string& phrase, double confidence) {
    m_object->emitSignal("CommandExecuted").onInterface("com.github.saim.Willow")
        .withArguments(command, phrase, confidence);
}

void VoiceAssistantService::emitStatusChanged(const std::map<std::string, sdbus::Variant>& status) {
    m_object->emitSignal("StatusChanged").onInterface("com.github.saim.Willow")
        .withArguments(status);
}

void VoiceAssistantService::emitError(const std::string& message, const std::string& details) {
    m_object->emitSignal("Error").onInterface("com.github.saim.Willow")
        .withArguments(message, details);
}

void VoiceAssistantService::emitNotification(const std::string& title, 
                                             const std::string& message, 
                                             const std::string& urgency) {
    m_object->emitSignal("Notification").onInterface("com.github.saim.Willow")
        .withArguments(title, message, urgency);
}

void VoiceAssistantService::emitConfigChanged(const std::string& config) {
    m_object->emitSignal("ConfigChanged").onInterface("com.github.saim.Willow")
        .withArguments(config);
}

// Whisper integration

// Audio capture (using PulseAudio/PipeWire)

void VoiceAssistantService::startAudioCapture() {
    log("INFO", "Starting audio capture");
    
    // Initialize PulseAudio
    pa_sample_spec ss;
    ss.format = PA_SAMPLE_FLOAT32LE;
    ss.channels = 1;  // Mono
    ss.rate = 16000;  // 16kHz for Whisper
    
    pa_buffer_attr bufattr;
    bufattr.maxlength = (uint32_t) -1;
    bufattr.fragsize = 4096;  // Fragment size
    
    int error;
    m_pulseAudio = pa_simple_new(
        nullptr,                    // Use default server
        "Voice Assistant",          // Application name
        PA_STREAM_RECORD,           // Record stream
        nullptr,                    // Use default source
        "Voice Input",              // Stream description
        &ss,                        // Sample spec
        nullptr,                    // Use default channel map
        &bufattr,                   // Buffer attributes
        &error                      // Error code
    );
    
    if (!m_pulseAudio) {
        std::string errorMsg = "Failed to connect to PulseAudio: " + std::string(pa_strerror(error));
        log("ERROR", errorMsg);
        emitError("Audio Error", errorMsg);
        return;
    }
    
    log("INFO", "PulseAudio connected successfully");
    m_audioThread = std::thread(&VoiceAssistantService::audioProcessingLoop, this);
}

void VoiceAssistantService::stopAudioCapture() {
    log("INFO", "Stopping audio capture");
    
    if (m_audioThread.joinable()) {
        m_stopAudioThread = true;
        m_audioThread.join();
    }
    
    if (m_pulseAudio) {
        pa_simple_free(m_pulseAudio);
        m_pulseAudio = nullptr;
    }
}

void VoiceAssistantService::audioProcessingLoop() {
    log("INFO", "Audio processing loop started");
    
    const size_t CHUNK_SIZE = 4096;  // Number of samples per chunk
    std::vector<float> chunk(CHUNK_SIZE);
    int error;
    
    while (!m_stopAudioThread) {
        // Read audio chunk from PulseAudio
        if (pa_simple_read(m_pulseAudio, chunk.data(), 
                          chunk.size() * sizeof(float), &error) < 0) {
            std::string errorMsg = "Failed to read audio: " + std::string(pa_strerror(error));
            log("ERROR", errorMsg);
            emitError("Audio Error", errorMsg);
            break;
        }
        
        // Pass to speech segmenter for VAD-based processing
        m_segmenter->processAudioChunk(chunk);
        
        // Check if we should stop
        if (m_stopAudioThread) break;
    }
    
    log("INFO", "Audio processing loop stopped");
}

// Transcription handling

void VoiceAssistantService::handleTranscription(const std::string& text) {
    log("INFO", "Transcription received: '" + text + "'");
    
    // Pass to current mode worker
    if (m_currentWorker && m_isRunning) {
        m_currentWorker->processTranscription(text);
        
        // Emit buffer changed for UI update
        emitBufferChanged(GetBuffer());
    }
}

// Mode worker management

void VoiceAssistantService::updateModeWorkers() {
    // m_modeMutex should already be locked by caller
    
    Mode mode = m_currentMode.load();
    
    switch (mode) {
        case Mode::Normal:
            m_currentWorker = m_normalWorker.get();
            break;
        case Mode::Command:
            m_currentWorker = m_commandWorker.get();
            break;
        case Mode::Typing:
            m_currentWorker = m_typingWorker.get();
            break;
    }
    
    // Start the new worker if service is running
    if (m_isRunning && m_currentWorker) {
        m_currentWorker->start();
    }
}

// Configuration management

void VoiceAssistantService::loadConfig() {
    std::lock_guard<std::mutex> lock(m_configMutex);
    
    // Check if config file exists
    if (!fs::exists(m_configPath)) {
        // Try to copy default config from system location
        const std::string systemConfig = "/usr/share/willow/config.json";
        if (fs::exists(systemConfig)) {
            try {
                fs::path configPath(m_configPath);
                fs::create_directories(configPath.parent_path());
                fs::copy_file(systemConfig, m_configPath);
                log("INFO", "Created config from system default: " + m_configPath);
            } catch (const std::exception& e) {
                log("ERROR", "Failed to copy default config: " + std::string(e.what()));
                log("WARNING", "Using built-in defaults");
                return;
            }
        } else {
            log("WARNING", "Config file not found and no system default available, using built-in defaults");
            return;
        }
    }
    
    std::ifstream file(m_configPath);
    if (!file.is_open()) {
        log("WARNING", "Config file not found, using defaults");
        return;
    }
    
    Json::CharReaderBuilder reader;
    Json::Value root;
    std::string errs;
    
    if (Json::parseFromStream(reader, file, &root, &errs)) {
        jsonToConfig(root);
        log("INFO", "Configuration loaded from: " + m_configPath);
    } else {
        log("ERROR", "Failed to parse config: " + errs);
    }
}

void VoiceAssistantService::saveConfig() {
    // Note: m_configMutex should already be locked by caller
    
    Json::Value root = configToJson();
    
    // Ensure directory exists
    fs::path configPath(m_configPath);
    fs::create_directories(configPath.parent_path());
    
    std::ofstream file(m_configPath);
    if (file.is_open()) {
        Json::StreamWriterBuilder writer;
        writer["indentation"] = "  ";
        file << Json::writeString(writer, root);
        log("INFO", "Configuration saved");
    } else {
        log("ERROR", "Failed to save config to: " + m_configPath);
    }
}

Json::Value VoiceAssistantService::configToJson() const {
    Json::Value root;
    
    root["hotword"] = m_hotword;
    root["command_threshold"] = m_commandThreshold;
    root["processing_interval"] = m_processingInterval;
    root["whisper_model"] = m_whisperModel;
    root["gpu_acceleration"] = m_gpuAcceleration;
    
    Json::Value logging;
    logging["level"] = "INFO";
    logging["file"] = m_logFile;
    root["logging"] = logging;
    
    Json::Value commands(Json::arrayValue);
    for (const auto& cmd : m_commands) {
        Json::Value cmdJson;
        cmdJson["name"] = cmd.name;
        cmdJson["command"] = cmd.command;
        
        Json::Value phrases(Json::arrayValue);
        for (const auto& phrase : cmd.phrases) {
            phrases.append(phrase);
        }
        cmdJson["phrases"] = phrases;
        
        commands.append(cmdJson);
    }
    root["commands"] = commands;
    
    return root;
}

void VoiceAssistantService::jsonToConfig(const Json::Value& json) {
    if (json.isMember("hotword")) {
        m_hotword = json["hotword"].asString();
    }
    
    if (json.isMember("command_threshold")) {
        // Config stores as percentage (0-100), convert to decimal (0.0-1.0)
        m_commandThreshold = json["command_threshold"].asDouble() / 100.0;
    }
    
    if (json.isMember("processing_interval")) {
        m_processingInterval = json["processing_interval"].asDouble();
    }
    
    if (json.isMember("whisper_model")) {
        m_whisperModel = json["whisper_model"].asString();
        log("INFO", "Whisper model configured: " + m_whisperModel);
    }
    
    if (json.isMember("gpu_acceleration")) {
        m_gpuAcceleration = json["gpu_acceleration"].asBool();
        log("INFO", "GPU acceleration configured: " + std::string(m_gpuAcceleration ? "enabled" : "disabled"));
    }
    
    // Load typing mode exit phrases
    if (json.isMember("typing_mode") && json["typing_mode"].isMember("exit_phrases")) {
        m_typingExitPhrases.clear();
        const auto& exitPhrases = json["typing_mode"]["exit_phrases"];
        if (exitPhrases.isArray()) {
            for (const auto& phrase : exitPhrases) {
                std::string exitPhrase = phrase.asString();
                // Convert to lowercase for matching
                std::transform(exitPhrase.begin(), exitPhrase.end(), exitPhrase.begin(), ::tolower);
                m_typingExitPhrases.push_back(exitPhrase);
            }
            log("INFO", "Loaded " + std::to_string(m_typingExitPhrases.size()) + " typing exit phrases");
        }
    }
    
    if (json.isMember("commands") && json["commands"].isArray()) {
        std::lock_guard<std::mutex> lock(m_commandsMutex);
        m_commands.clear();
        
        for (const auto& cmdJson : json["commands"]) {
            // Skip if this is a comment-only object (all keys start with _)
            bool isCommentOnly = true;
            for (const auto& key : cmdJson.getMemberNames()) {
                if (!key.empty() && key[0] != '_') {
                    isCommentOnly = false;
                    break;
                }
            }
            if (isCommentOnly) {
                continue;
            }
            
            Command cmd;
            cmd.name = cmdJson["name"].asString();
            cmd.command = cmdJson["command"].asString();
            
            if (cmdJson.isMember("phrases") && cmdJson["phrases"].isArray()) {
                for (const auto& phrase : cmdJson["phrases"]) {
                    cmd.phrases.push_back(phrase.asString());
                }
            }
            
            m_commands.push_back(cmd);
            log("INFO", "Loaded command: " + cmd.name + " with " + std::to_string(cmd.phrases.size()) + " phrases");
        }
        
        log("INFO", "Total commands loaded: " + std::to_string(m_commands.size()));
    }
}

// Mode management

void VoiceAssistantService::setModeInternal(Mode mode) {
    m_currentMode = mode;
}

Mode VoiceAssistantService::stringToMode(const std::string& modeStr) const {
    if (modeStr == "command") return Mode::Command;
    if (modeStr == "typing") return Mode::Typing;
    return Mode::Normal;
}

std::string VoiceAssistantService::modeToString(Mode mode) const {
    switch (mode) {
        case Mode::Command: return "command";
        case Mode::Typing: return "typing";
        default: return "normal";
    }
}

// Helper methods

void VoiceAssistantService::log(const std::string& level, const std::string& message) {
    std::lock_guard<std::mutex> lock(m_logMutex);
    
    auto now = std::time(nullptr);
    auto tm = *std::localtime(&now);
    
    std::ofstream logFile(m_logFile, std::ios::app);
    if (logFile.is_open()) {
        logFile << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") 
                << " [" << level << "] " << message << std::endl;
    }
}

} // namespace VoiceAssistant
