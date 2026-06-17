#include "ModeWorkers.hpp"
#include <algorithm>
#include <iostream>

namespace VoiceAssistant {

// ============================================================================
// NormalModeWorker Implementation
// ============================================================================

NormalModeWorker::NormalModeWorker(
    std::shared_ptr<CommandExecutor> executor,
    std::shared_ptr<SpeechSegmenter> segmenter
)
    : m_isRunning(false)
    , m_hotword("hey")
    , m_segmenter(segmenter)
{
    m_executor = executor;
}

void NormalModeWorker::start() {
    if (m_isRunning) return;
    
    m_isRunning = true;
    
    // Configure segmenter for hotword detection
    // Sensitive threshold for normal speech volume
    m_segmenter->setVADThreshold(0.0005f);
    // Shorter silence duration for faster response
    m_segmenter->setSilenceDuration(0.5f);
    // Shorter minimum speech for hotword
    m_segmenter->setMinSpeechDuration(0.15f);
    
    m_executor->log("INFO", "Normal mode worker started (hotword: " + m_hotword + ")");
}

void NormalModeWorker::stop() {
    if (!m_isRunning) return;
    
    m_isRunning = false;
    m_executor->log("INFO", "Normal mode worker stopped");
}

void NormalModeWorker::processTranscription(const std::string& text) {
    if (!m_isRunning) return;
    
    m_executor->log("INFO", "Normal mode: checking for hotword in: '" + text + "'");
    
    // Check for hotword (text is already lowercase from SpeechSegmenter)
    if (text.find(m_hotword) != std::string::npos) {
        m_executor->log("INFO", "Hotword detected: " + m_hotword);
        requestModeChange("command");
    }
}

// ============================================================================
// CommandModeWorker Implementation
// ============================================================================

CommandModeWorker::CommandModeWorker(
    std::shared_ptr<CommandExecutor> executor,
    std::shared_ptr<SpeechSegmenter> segmenter
)
    : m_isRunning(false)
    , m_segmenter(segmenter)
    , m_threshold(0.8)
{
    m_executor = executor;
}

void CommandModeWorker::start() {
    if (m_isRunning) return;
    
    m_isRunning = true;
    
    // Configure segmenter for command recognition
    // Normal VAD threshold
    m_segmenter->setVADThreshold(0.001f);
    // Shorter silence duration for faster response
    m_segmenter->setSilenceDuration(0.5f);
    // Normal minimum speech
    m_segmenter->setMinSpeechDuration(0.25f);
    
    {
        std::lock_guard<std::mutex> lock(m_bufferMutex);
        m_buffer.clear();
    }
    
    m_executor->log("INFO", "Command mode worker started");
}

void CommandModeWorker::stop() {
    if (!m_isRunning) return;
    
    m_isRunning = false;
    
    {
        std::lock_guard<std::mutex> lock(m_bufferMutex);
        m_buffer.clear();
    }
    
    m_executor->log("INFO", "Command mode worker stopped");
}

void CommandModeWorker::processTranscription(const std::string& text) {
    if (!m_isRunning) return;
    
    // Update buffer for display
    {
        std::lock_guard<std::mutex> lock(m_bufferMutex);
        m_buffer = text;
    }
    
    m_executor->log("INFO", "Command mode: processing '" + text + "'");
    
    // Check for smart "open/launch" commands
    if (processSmartOpen(text)) {
        return;
    }
    
    // Check for smart "search _ for" commands
    if (processSmartSearch(text)) {
        return;
    }
    
    // Find best matching command
    std::lock_guard<std::mutex> lock(m_commandsMutex);
    auto [bestCmd, confidence] = m_executor->findBestMatch(text, m_commands, m_threshold);
    
    m_executor->log("INFO", "Best match confidence: " + std::to_string(confidence) + 
                    ", threshold: " + std::to_string(m_threshold));
    
    if (bestCmd && confidence >= m_threshold) {
        m_executor->log("INFO", "Best matching command: " + bestCmd->name + 
                       " (confidence: " + std::to_string(confidence * 100) + "%)");
        
        // Check for duplicate execution
        if (isDuplicate(bestCmd->name)) {
            m_executor->log("INFO", "Command ignored: Duplicate detected");
            return;
        }
        
        // Record execution
        recordExecution(bestCmd->name);
        
        // Handle special commands
        if (bestCmd->command == "exit_command_mode") {
            m_executor->log("INFO", "Exit command mode");
            requestModeChange("normal");
            return;
        }
        
        if (bestCmd->command == "start_typing_mode") {
            m_executor->log("INFO", "Start typing mode");
            requestModeChange("typing");
            return;
        }
        
        // Execute command
        m_executor->executeCommand(bestCmd->command);
        
    } else if (bestCmd) {
        m_executor->log("INFO", "Command match below threshold, not executing");
    } else {
        m_executor->log("INFO", "No command matched");
    }
}

void CommandModeWorker::setCommands(const std::vector<Command>& commands) {
    std::lock_guard<std::mutex> lock(m_commandsMutex);
    m_commands = commands;
}

std::string CommandModeWorker::getBuffer() const {
    std::lock_guard<std::mutex> lock(m_bufferMutex);
    return m_buffer;
}

bool CommandModeWorker::isDuplicate(const std::string& commandName) {
    cleanHistory();
    
    std::lock_guard<std::mutex> lock(m_historyMutex);
    
    auto now = std::chrono::steady_clock::now();
    
    // Check if this command was executed in the last 2 seconds
    for (const auto& record : m_executionHistory) {
        if (record.commandName == commandName) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - record.timestamp).count();
            
            if (elapsed < 2000) {  // 2 second window
                return true;
            }
        }
    }
    
    return false;
}

void CommandModeWorker::recordExecution(const std::string& commandName) {
    std::lock_guard<std::mutex> lock(m_historyMutex);
    
    ExecutionRecord record;
    record.commandName = commandName;
    record.timestamp = std::chrono::steady_clock::now();
    
    m_executionHistory.push_back(record);
}

void CommandModeWorker::cleanHistory() {
    std::lock_guard<std::mutex> lock(m_historyMutex);
    
    auto now = std::chrono::steady_clock::now();
    
    // Remove records older than 5 seconds
    m_executionHistory.erase(
        std::remove_if(m_executionHistory.begin(), m_executionHistory.end(),
            [&now](const ExecutionRecord& record) {
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    now - record.timestamp).count();
                return elapsed > 5;
            }),
        m_executionHistory.end()
    );
}

bool CommandModeWorker::processSmartOpen(const std::string& text) {
    // Check for "open" or "launch" triggers
    std::vector<std::string> triggers = {"open ", "launch ", "start "};
    
    for (const auto& trigger : triggers) {
        if (text.find(trigger) != std::string::npos) {
            std::string appName = extractAppName(text, trigger);
            if (!appName.empty()) {
                // Check for duplicate
                if (isDuplicate("smart_open_" + appName)) {
                    m_executor->log("INFO", "Smart open ignored: Duplicate detected");
                    return true;
                }
                
                recordExecution("smart_open_" + appName);
                
                if (m_executor->executeSmartOpen(appName)) {
                    m_executor->log("INFO", "Smart open executed successfully: " + appName);
                    return true;
                }
            }
        }
    }
    
    return false;
}

bool CommandModeWorker::processSmartSearch(const std::string& text) {
    // Check for "search X for Y" pattern
    auto [engine, query] = extractSearchQuery(text);
    
    if (!engine.empty() && !query.empty()) {
        // Check for duplicate
        std::string searchKey = "smart_search_" + engine + "_" + query;
        if (isDuplicate(searchKey)) {
            m_executor->log("INFO", "Smart search ignored: Duplicate detected");
            return true;
        }
        
        recordExecution(searchKey);
        
        if (m_executor->executeSmartSearch(engine, query)) {
            m_executor->log("INFO", "Smart search executed: " + engine + " for " + query);
            return true;
        }
    }
    
    return false;
}

std::string CommandModeWorker::extractAppName(const std::string& text, const std::string& trigger) {
    size_t pos = text.find(trigger);
    if (pos == std::string::npos) {
        return "";
    }
    
    // Extract everything after the trigger
    std::string appName = text.substr(pos + trigger.length());
    
    // Trim whitespace
    size_t start = appName.find_first_not_of(" \t");
    size_t end = appName.find_last_not_of(" \t");
    
    if (start != std::string::npos && end != std::string::npos) {
        appName = appName.substr(start, end - start + 1);
    }
    
    return appName;
}

std::pair<std::string, std::string> CommandModeWorker::extractSearchQuery(const std::string& text) {
    // Pattern: "search [engine] for [query]"
    size_t searchPos = text.find("search ");
    if (searchPos == std::string::npos) {
        return {"", ""};
    }
    
    size_t forPos = text.find(" for ", searchPos);
    if (forPos == std::string::npos) {
        return {"", ""};
    }
    
    // Extract engine name (between "search " and " for ")
    size_t engineStart = searchPos + 7; // length of "search "
    std::string engine = text.substr(engineStart, forPos - engineStart);
    
    // Extract query (everything after " for ")
    std::string query = text.substr(forPos + 5); // length of " for "
    
    // Trim whitespace
    auto trim = [](std::string& s) {
        size_t start = s.find_first_not_of(" \t");
        size_t end = s.find_last_not_of(" \t");
        if (start != std::string::npos && end != std::string::npos) {
            s = s.substr(start, end - start + 1);
        } else {
            s = "";
        }
    };
    
    trim(engine);
    trim(query);
    
    return {engine, query};
}

// ============================================================================
// TypingModeWorker Implementation
// ============================================================================

TypingModeWorker::TypingModeWorker(
    std::shared_ptr<CommandExecutor> executor,
    std::shared_ptr<SpeechSegmenter> segmenter
)
    : m_isRunning(false)
    , m_segmenter(segmenter)
{
    m_executor = executor;
    m_exitPhrases = {"stop typing", "exit typing", "normal mode", "go to normal mode"};
}

void TypingModeWorker::start() {
    if (m_isRunning) return;
    
    m_isRunning = true;
    
    // Configure segmenter for typing
    // Normal VAD threshold
    m_segmenter->setVADThreshold(0.001f);
    // Longer silence duration for natural pauses
    m_segmenter->setSilenceDuration(1.0f);
    // Normal minimum speech
    m_segmenter->setMinSpeechDuration(0.3f);
    
    {
        std::lock_guard<std::mutex> lock(m_bufferMutex);
        m_buffer.clear();
    }
    
    m_executor->log("INFO", "Typing mode worker started");
}

void TypingModeWorker::stop() {
    if (!m_isRunning) return;
    
    m_isRunning = false;
    
    {
        std::lock_guard<std::mutex> lock(m_bufferMutex);
        m_buffer.clear();
    }
    
    m_executor->log("INFO", "Typing mode worker stopped");
}

void TypingModeWorker::processTranscription(const std::string& text) {
    if (!m_isRunning) return;
    
    m_executor->log("INFO", "Typing mode: processing '" + text + "'");
    
    // Check for exit phrases
    if (checkExitPhrases(text)) {
        m_executor->log("INFO", "Typing exit phrase detected");
        requestModeChange("normal");
        return;
    }
    
    // Type the text
    m_executor->typeText(text);
    
    // Update buffer for display
    {
        std::lock_guard<std::mutex> lock(m_bufferMutex);
        m_buffer = text;
    }
}

std::string TypingModeWorker::getBuffer() const {
    std::lock_guard<std::mutex> lock(m_bufferMutex);
    return m_buffer;
}

bool TypingModeWorker::checkExitPhrases(const std::string& text) {
    for (const auto& exitPhrase : m_exitPhrases) {
        if (text.find(exitPhrase) != std::string::npos) {
            return true;
        }
    }
    return false;
}

} // namespace VoiceAssistant
