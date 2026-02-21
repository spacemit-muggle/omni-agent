#pragma once

#include <queue>
#include <string>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <regex>

class TextBuffer {
public:
    TextBuffer();
    ~TextBuffer();

    // Add streaming text to buffer
    void addText(const std::string& text);
    
    // Get next complete sentence (if available)
    std::string getNextSentence();
    
    // Check if there are complete sentences available
    bool hasSentence() const;
    
    // Clear all buffered text
    void clear();
    
    // Stop buffer processing
    void stop();

private:
    void processBuffer();
    bool isEndOfSentence(char c) const;
    
    std::string buffer_;
    std::queue<std::string> sentences_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> stop_flag_;
    
    // Chinese punctuation patterns
    static const std::string CHINESE_PUNCTUATION;
};