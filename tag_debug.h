#pragma once

#include <string>

#include <taglib/tdebuglistener.h>

extern thread_local bool t_has_legacy_frames;

class AudioWardenDebugListener : public TagLib::DebugListener {
public:
    void printMessage(const TagLib::String& msg) override {
        std::string m = msg.to8Bit(true);
        if (m.find("TDAT") != std::string::npos ||
            m.find("TYER") != std::string::npos ||
            m.find("TIME") != std::string::npos ||
            m.find("TORY") != std::string::npos ||
            m.find("TRDA") != std::string::npos) {
            t_has_legacy_frames = true;
        }
    }
};
